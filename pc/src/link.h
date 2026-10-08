// Transport to the truck: UDP to the phone (normal) or USB serial straight to the ESP32
// (bench testing without the phone).
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "config.h"
#include "fpv/net_proto.h"
#include "mapping.h"

namespace pc {

class VideoReceiver;

struct LinkStatus {
    bool connected = false;  // valid traffic from the far end within the last second
    std::string peer;        // phone address or serial port name
    uint64_t lastRxUs = 0;

    bool haveTelemetry = false;
    bool viaPhone = false;  // phone fields of tel are meaningful
    fpv::net::Telemetry tel;
    uint64_t telRxUs = 0;

    float rttMs = -1;     // PC <-> phone (UDP) or PC <-> ESP32 (serial)
    float rttMinMs = -1;
    bool offsetValid = false;
    int64_t phoneMinusPcUs = 0;  // phone clock - PC clock
    float lossPct = 0;           // phone -> PC datagram loss, last second
    uint32_t badPackets = 0;     // failed MAC / malformed
    uint64_t lastBadUs = 0;      // when the last one arrived (a wrong passphrase looks like this)

    uint16_t videoW = 0, videoH = 0;
    uint8_t videoFps = 0;
};

class Link {
public:
    virtual ~Link() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
    // Called from the control thread; must not block.
    virtual void sendControl(const DriveCommand& cmd) = 0;
    virtual LinkStatus status() = 0;
};

std::unique_ptr<Link> makeUdpLink(const Config& cfg, VideoReceiver* video);
std::unique_ptr<Link> makeSerialLink(const Config& cfg);

}  // namespace pc
