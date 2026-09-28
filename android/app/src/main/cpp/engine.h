// Phone-side session: UDP link to the PC, control relay to the ESP32, telemetry, failsafe
// and video packetisation.
//
// Control path latency is one UDP receive + one USB bulk write: each CONTROL packet is
// forwarded to the ESP32 on the network thread the moment it is authenticated.
#pragma once
#include <android/multinetwork.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "fpv/net_proto.h"
#include "fpv/serial_proto.h"
#include "usb_serial.h"
#include "video.h"

namespace rover {

struct EngineConfig {
    std::string host;
    uint16_t port = fpv::net::kDefaultPort;
    fpv::SipKey key;
    VideoConfig video;
};

struct PhoneStats {
    uint8_t battPct = 0;
    int16_t tempDeciC = 0;
    bool charging = false;
    int16_t rsrpDbm = 0;
    uint8_t netType = 0;
};

class Engine {
public:
    Engine(const EngineConfig& cfg, UsbSerial& usb);
    ~Engine();

    bool start();
    void stop();

    void setNetwork(net_handle_t handle);  // 0 = system default
    void setPhoneStats(const PhoneStats& s);
    std::string status();

private:
    void netLoop();
    bool openSocket();
    void closeSocket();
    void handlePacket(const uint8_t* buf, size_t n, uint64_t now);
    void sendHello();
    void sendTelemetry(uint64_t now);
    void sendDatagram(const uint8_t* buf, size_t n);
    void onVideoFrame(const uint8_t* data, size_t len, bool key, uint64_t captureUs);
    void failsafeTick(uint64_t now);

    const EngineConfig cfg_;
    UsbSerial& usb_;
    VideoPipeline video_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex sockMu_;
    int sock_ = -1;
    std::atomic<net_handle_t> network_{0};
    net_handle_t boundNetwork_ = 0;
    std::string peerDesc_;

    uint32_t session_;
    std::atomic<uint32_t> seq_{1};
    uint32_t frameId_ = 0;

    // PC session (network thread only)
    std::atomic<bool> acked_{false};  // read by the video thread
    bool havePcSession_ = false;
    uint32_t pcSession_ = 0;
    fpv::net::ReplayWindow replay_;
    uint64_t lastPcRxUs_ = 0;

    // Control / failsafe
    std::mutex stateMu_;
    uint64_t lastControlRxUs_ = 0;
    uint64_t lastPcTimeUs_ = 0;
    bool failsafe_ = true;
    uint64_t lastFailsafeTxUs_ = 0;
    uint32_t controlsRx_ = 0;

    // ESP32 telemetry
    fpv::ser::Telemetry esp_{};
    uint64_t espRxUs_ = 0;

    PhoneStats phone_;
};

}  // namespace rover
