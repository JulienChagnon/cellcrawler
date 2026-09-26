// PC <-> phone UDP protocol.
//
// Every datagram:  header(12) | payload | mac:u64le
//   header = 'F' 'V' version:u8 type:u8 session:u32 seq:u32
//   mac    = SipHash-2-4(key, header | payload)
//
// session is random per sender start-up; seq increases by one for every datagram the sender
// emits (all types share it) and is checked against a 64-entry sliding replay window.
// All integers are little-endian. See docs/protocol.md.
#pragma once
#include <cstddef>
#include <cstdint>

#include "bytes.h"
#include "siphash.h"

namespace fpv::net {

constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 12;
constexpr size_t kMacSize = 8;
constexpr uint16_t kDefaultPort = 47000;

// Keeps every datagram under 1232 bytes: fits IPv6 minimum MTU and Tailscale's 1280 MTU.
constexpr size_t kMaxDatagram = 1200;
constexpr size_t kVideoHeaderSize = 24;
constexpr size_t kMaxVideoChunk = kMaxDatagram - kHeaderSize - kVideoHeaderSize - kMacSize;

enum Type : uint8_t {
    kHello = 1,          // phone -> PC, until acked; also every 2 s as a keepalive
    kHelloAck = 2,       // PC -> phone
    kControl = 3,        // PC -> phone, event driven + >=100 Hz heartbeat
    kTelemetry = 4,      // phone -> PC, 20 Hz
    kVideo = 5,          // phone -> PC, one encoded frame split into chunks
    kVideoFeedback = 6,  // PC -> phone, 5 Hz
};

struct Header {
    uint8_t type = 0;
    uint32_t session = 0;
    uint32_t seq = 0;
};

struct Hello {
    uint16_t width = 0, height = 0;
    uint8_t fps = 0;
};

struct HelloAck {
    uint32_t phoneSession = 0;  // echo, so a stale ack is not mistaken for ours
};

// Same semantics as ser::Control.
struct Control {
    uint64_t pcTimeUs = 0;  // echoed back in telemetry for RTT measurement
    int16_t steer = 0;
    uint16_t throttle = 0;
    uint16_t brake = 0;
    uint8_t flags = 0;  // ser::ControlFlags
};

enum PhoneFlags : uint8_t {
    kPhoneFailsafe = 1 << 0,  // phone has not received control from the PC recently
    kPhoneUsbOk = 1 << 1,     // USB serial to the ESP32 is open
    kPhoneEspOk = 1 << 2,     // ESP32 telemetry is arriving
    kPhoneCharging = 1 << 3,
    kPhoneVideoOn = 1 << 4,
};

struct Telemetry {
    uint64_t echoPcTimeUs = 0;  // pcTimeUs of the newest control received
    uint32_t echoHoldUs = 0;    // time between receiving that control and sending this packet
    uint64_t phoneTimeUs = 0;   // phone monotonic clock at send
    // ESP32 (copied from ser::Telemetry)
    uint16_t battMv = 0;
    uint8_t cells = 0;
    uint8_t espFlags = 0;
    uint16_t steerUs = 0;
    uint16_t throttleUs = 0;
    uint16_t espRxErrors = 0;
    uint32_t espRttUs = 0;  // phone <-> ESP32 serial round trip, 0 = unknown
    // Phone
    uint8_t phoneFlags = 0;
    uint8_t phoneBattPct = 0;
    int16_t phoneTempDeciC = 0;
    int16_t rsrpDbm = 0;  // LTE RSRP, 0 = unknown
    uint8_t netType = 0;  // 0 unknown, 1 wifi, 2 lte, 3 nr, 4 other cellular
    uint32_t videoBitrate = 0;
    uint8_t videoFps = 0;
};

enum VideoFlags : uint8_t {
    kVidKeyframe = 1 << 0,
    kVidHasConfig = 1 << 1,  // frame begins with SPS/PPS
};

struct VideoHeader {
    uint32_t frameId = 0;
    uint16_t chunkIdx = 0;
    uint16_t chunkCount = 0;
    uint8_t flags = 0;
    uint8_t reserved = 0;
    uint16_t reserved2 = 0;
    uint64_t captureUs = 0;  // phone monotonic clock, camera sensor timestamp
    uint32_t phoneLatUs = 0;  // capture -> first chunk sent, measured on the phone
};

struct VideoFeedback {
    uint8_t requestKeyframe = 0;
    uint16_t lossPermille = 0;  // video datagram loss in the last interval
    uint16_t queueDelayMs = 0;  // one-way delay above the recent minimum (network queueing)
    uint32_t recvKbps = 0;
};

// ---------------------------------------------------------------------------------------
// Datagram assembly

// Begins a datagram: writes the header, returns a writer positioned at the payload.
inline ByteWriter beginPacket(uint8_t* buf, size_t cap, const Header& h) {
    ByteWriter w(buf, cap);
    w.u8('F');
    w.u8('V');
    w.u8(kVersion);
    w.u8(h.type);
    w.u32(h.session);
    w.u32(h.seq);
    return w;
}

// Appends the MAC. Returns the final datagram length, 0 on overflow.
inline size_t finishPacket(ByteWriter& w, uint8_t* buf, const SipKey& key) {
    uint64_t mac = siphash24(key, buf, w.size());
    w.u64(mac);
    return w.ok() ? w.size() : 0;
}

// Verifies magic, version and MAC. On success fills h and payload/payloadLen.
inline bool openPacket(const uint8_t* buf, size_t len, const SipKey& key, Header& h,
                       const uint8_t*& payload, size_t& payloadLen) {
    if (len < kHeaderSize + kMacSize) return false;
    if (buf[0] != 'F' || buf[1] != 'V' || buf[2] != kVersion) return false;
    size_t body = len - kMacSize;
    ByteReader macR(buf + body, kMacSize);
    if (macR.u64() != siphash24(key, buf, body)) return false;
    ByteReader r(buf, kHeaderSize);
    r.u8();
    r.u8();
    r.u8();
    h.type = r.u8();
    h.session = r.u32();
    h.seq = r.u32();
    payload = buf + kHeaderSize;
    payloadLen = body - kHeaderSize;
    return true;
}

// 64-entry sliding window replay filter (same idea as IPsec / WireGuard).
class ReplayWindow {
public:
    // Returns true if seq is new; records it.
    bool accept(uint32_t seq) {
        if (!init_) {
            init_ = true;
            top_ = seq;
            bits_ = 1;
            return true;
        }
        if (seq > top_) {
            uint32_t shift = seq - top_;
            bits_ = shift >= 64 ? 0 : bits_ << shift;
            bits_ |= 1;
            top_ = seq;
            return true;
        }
        uint32_t back = top_ - seq;
        if (back >= 64) return false;
        uint64_t mask = 1ULL << back;
        if (bits_ & mask) return false;
        bits_ |= mask;
        return true;
    }
    void reset() { init_ = false; }

private:
    bool init_ = false;
    uint32_t top_ = 0;
    uint64_t bits_ = 0;
};

// ---------------------------------------------------------------------------------------
// Payload codecs

inline void write(ByteWriter& w, const Hello& m) {
    w.u16(m.width);
    w.u16(m.height);
    w.u8(m.fps);
}
inline bool read(ByteReader r, Hello& m) {
    m.width = r.u16();
    m.height = r.u16();
    m.fps = r.u8();
    return r.ok();
}

inline void write(ByteWriter& w, const HelloAck& m) { w.u32(m.phoneSession); }
inline bool read(ByteReader r, HelloAck& m) {
    m.phoneSession = r.u32();
    return r.ok();
}

inline void write(ByteWriter& w, const Control& m) {
    w.u64(m.pcTimeUs);
    w.i16(m.steer);
    w.u16(m.throttle);
    w.u16(m.brake);
    w.u8(m.flags);
}
inline bool read(ByteReader r, Control& m) {
    m.pcTimeUs = r.u64();
    m.steer = r.i16();
    m.throttle = r.u16();
    m.brake = r.u16();
    m.flags = r.u8();
    return r.ok();
}

inline void write(ByteWriter& w, const Telemetry& m) {
    w.u64(m.echoPcTimeUs);
    w.u32(m.echoHoldUs);
    w.u64(m.phoneTimeUs);
    w.u16(m.battMv);
    w.u8(m.cells);
    w.u8(m.espFlags);
    w.u16(m.steerUs);
    w.u16(m.throttleUs);
    w.u16(m.espRxErrors);
    w.u32(m.espRttUs);
    w.u8(m.phoneFlags);
    w.u8(m.phoneBattPct);
    w.i16(m.phoneTempDeciC);
    w.i16(m.rsrpDbm);
    w.u8(m.netType);
    w.u32(m.videoBitrate);
    w.u8(m.videoFps);
}
inline bool read(ByteReader r, Telemetry& m) {
    m.echoPcTimeUs = r.u64();
    m.echoHoldUs = r.u32();
    m.phoneTimeUs = r.u64();
    m.battMv = r.u16();
    m.cells = r.u8();
    m.espFlags = r.u8();
    m.steerUs = r.u16();
    m.throttleUs = r.u16();
    m.espRxErrors = r.u16();
    m.espRttUs = r.u32();
    m.phoneFlags = r.u8();
    m.phoneBattPct = r.u8();
    m.phoneTempDeciC = r.i16();
    m.rsrpDbm = r.i16();
    m.netType = r.u8();
    m.videoBitrate = r.u32();
    m.videoFps = r.u8();
    return r.ok();
}

inline void write(ByteWriter& w, const VideoHeader& m) {
    w.u32(m.frameId);
    w.u16(m.chunkIdx);
    w.u16(m.chunkCount);
    w.u8(m.flags);
    w.u8(m.reserved);
    w.u16(m.reserved2);
    w.u64(m.captureUs);
    w.u32(m.phoneLatUs);
}
// Reads the video header; the remaining bytes of r are the chunk data.
inline bool read(ByteReader& r, VideoHeader& m) {
    m.frameId = r.u32();
    m.chunkIdx = r.u16();
    m.chunkCount = r.u16();
    m.flags = r.u8();
    m.reserved = r.u8();
    m.reserved2 = r.u16();
    m.captureUs = r.u64();
    m.phoneLatUs = r.u32();
    return r.ok() && m.chunkCount > 0 && m.chunkIdx < m.chunkCount;
}

inline void write(ByteWriter& w, const VideoFeedback& m) {
    w.u8(m.requestKeyframe);
    w.u16(m.lossPermille);
    w.u16(m.queueDelayMs);
    w.u32(m.recvKbps);
}
inline bool read(ByteReader r, VideoFeedback& m) {
    m.requestKeyframe = r.u8();
    m.lossPermille = r.u16();
    m.queueDelayMs = r.u16();
    m.recvKbps = r.u32();
    return r.ok();
}

}  // namespace fpv::net
