// Host (phone or PC) <-> ESP32 serial link.
//
// Frame on the wire:  COBS( type:u8 | payload | crc16:u16le ) 0x00
// CRC covers type + payload. See docs/protocol.md.
#pragma once
#include <cstddef>
#include <cstdint>

#include "bytes.h"
#include "cobs.h"
#include "crc16.h"

namespace fpv::ser {

constexpr uint32_t kBaud = 921600;

// The ESP32 answers every control frame whose seq is a multiple of this with an immediate
// telemetry frame, so the host can time the serial round trip.
constexpr uint16_t kEchoEvery = 16;

enum Type : uint8_t {
    kControl = 0x01,    // host -> ESP32
    kTelemetry = 0x81,  // ESP32 -> host
};

// Control command flags.
enum ControlFlags : uint8_t {
    kArmed = 1 << 0,
    kGearReverse = 1 << 1,
};

// Normalised command, identical in meaning to the network CONTROL packet.
struct Control {
    uint16_t seq = 0;
    int16_t steer = 0;      // -1000 (full left) .. +1000 (full right)
    uint16_t throttle = 0;  // 0..1000, fraction of the firmware's hard cap for the current gear
    uint16_t brake = 0;     // 0..1000
    uint8_t flags = 0;      // ControlFlags
};

// Telemetry status flags.
enum TelemetryFlags : uint8_t {
    kTelFailsafe = 1 << 0,        // no valid control frame recently -> outputs neutral
    kTelArmed = 1 << 1,
    kTelGearReverse = 1 << 2,     // gear the firmware is actually in
    kTelReverseEngaged = 1 << 3,  // ESC reverse sequence complete
    kTelNeedNeutral = 1 << 4,     // waiting for throttle release before accepting throttle
    kTelLowBattery = 1 << 5,
};

struct Telemetry {
    uint16_t lastSeq = 0;  // seq of the last control frame applied
    uint16_t battMv = 0;   // filtered main battery voltage
    uint8_t cells = 0;     // detected/configured cell count
    uint8_t flags = 0;     // TelemetryFlags
    uint16_t steerUs = 0;  // pulse widths currently output
    uint16_t throttleUs = 0;
    uint16_t rxErrors = 0;  // CRC/COBS failures since boot (wraps)
    uint32_t uptimeMs = 0;
};

constexpr size_t kMaxPayload = 32;
constexpr size_t kMaxRaw = 1 + kMaxPayload + 2;
constexpr size_t kMaxFrame = cobsMaxEncoded(kMaxRaw) + 1;  // + delimiter

// Wraps type+payload into a delimited COBS frame. Returns frame length (0 on overflow).
inline size_t frame(uint8_t type, const uint8_t* payload, size_t len, uint8_t* out) {
    if (len > kMaxPayload) return 0;
    uint8_t raw[kMaxRaw];
    raw[0] = type;
    for (size_t i = 0; i < len; ++i) raw[1 + i] = payload[i];
    uint16_t crc = crc16(raw, 1 + len);
    raw[1 + len] = uint8_t(crc);
    raw[2 + len] = uint8_t(crc >> 8);
    size_t n = cobsEncode(raw, len + 3, out);
    out[n++] = 0;
    return n;
}

inline size_t encodeControl(const Control& c, uint8_t* out) {
    uint8_t p[kMaxPayload];
    ByteWriter w(p, sizeof p);
    w.u16(c.seq);
    w.i16(c.steer);
    w.u16(c.throttle);
    w.u16(c.brake);
    w.u8(c.flags);
    return frame(kControl, p, w.size(), out);
}

inline size_t encodeTelemetry(const Telemetry& t, uint8_t* out) {
    uint8_t p[kMaxPayload];
    ByteWriter w(p, sizeof p);
    w.u16(t.lastSeq);
    w.u16(t.battMv);
    w.u8(t.cells);
    w.u8(t.flags);
    w.u16(t.steerUs);
    w.u16(t.throttleUs);
    w.u16(t.rxErrors);
    w.u32(t.uptimeMs);
    return frame(kTelemetry, p, w.size(), out);
}

inline bool decodeControl(const uint8_t* p, size_t len, Control& c) {
    ByteReader r(p, len);
    c.seq = r.u16();
    c.steer = r.i16();
    c.throttle = r.u16();
    c.brake = r.u16();
    c.flags = r.u8();
    return r.ok();
}

inline bool decodeTelemetry(const uint8_t* p, size_t len, Telemetry& t) {
    ByteReader r(p, len);
    t.lastSeq = r.u16();
    t.battMv = r.u16();
    t.cells = r.u8();
    t.flags = r.u8();
    t.steerUs = r.u16();
    t.throttleUs = r.u16();
    t.rxErrors = r.u16();
    t.uptimeMs = r.u32();
    return r.ok();
}

// Incremental frame parser: feed it bytes as they arrive, it calls onFrame(type, payload, len)
// for every frame whose CRC checks out.
class FrameParser {
public:
    template <typename OnFrame>
    void feed(const uint8_t* data, size_t len, OnFrame&& onFrame) {
        for (size_t i = 0; i < len; ++i) {
            uint8_t b = data[i];
            if (b != 0) {
                if (n_ < sizeof buf_) buf_[n_++] = b;
                else overflow_ = true;
                continue;
            }
            if (n_ > 0) {
                uint8_t raw[sizeof buf_];
                size_t m = overflow_ ? 0 : cobsDecode(buf_, n_, raw);
                if (m >= 3 && crc16(raw, m - 2) == uint16_t(raw[m - 2] | (raw[m - 1] << 8)))
                    onFrame(raw[0], raw + 1, m - 3);
                else
                    ++errors_;
            }
            n_ = 0;
            overflow_ = false;
        }
    }
    uint16_t errors() const { return errors_; }

private:
    uint8_t buf_[kMaxFrame];
    size_t n_ = 0;
    bool overflow_ = false;
    uint16_t errors_ = 0;
};

}  // namespace fpv::ser
