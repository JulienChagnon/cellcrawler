// USB serial to the ESP32, driven from C++ through the kernel's usbdevfs interface.
// Java only obtains permission, claims the interface and hands us the file descriptor.
// Supports CP210x (most NodeMCU-32S), CH340/CH341 and CDC-ACM (CH9102, ESP32-S3 native).
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "fpv/serial_proto.h"

namespace rover {

class UsbSerial {
public:
    using TelemetryFn = std::function<void(const fpv::ser::Telemetry&, uint64_t rxUs)>;

    ~UsbSerial() { close(); }

    // ctrlIface: interface for control requests (CDC comm interface, else the data interface).
    bool open(int fd, int vid, int pid, int ctrlIface, int epIn, int epOut);
    void close();
    bool isOpen() const { return open_; }
    std::string chip() const;

    // Sends a control frame; the sequence number is assigned here. Thread-safe.
    bool sendControl(fpv::ser::Control c);

    void setTelemetryCallback(TelemetryFn fn);
    uint32_t rttUs() const { return rttUs_; }

private:
    enum class Chip { None, Cp210x, Ch34x, Cdc };

    int controlOut(uint8_t reqType, uint8_t req, uint16_t value, uint16_t index, const void* data, uint16_t len);
    int controlIn(uint8_t reqType, uint8_t req, uint16_t value, uint16_t index, void* data, uint16_t len);
    bool initCp210x();
    bool initCh34x();
    bool initCdc();
    bool bulkWrite(const uint8_t* data, size_t len);
    void readLoop();

    int fd_ = -1;
    Chip chip_ = Chip::None;
    int ctrlIface_ = 0;
    uint8_t epIn_ = 0, epOut_ = 0;
    std::atomic<bool> open_{false};
    std::atomic<bool> running_{false};
    std::thread reader_;

    std::mutex writeMu_;
    uint16_t seq_ = 1;
    std::atomic<uint64_t> sentUs_[64] = {};
    std::atomic<uint32_t> rttUs_{0};

    std::mutex cbMu_;
    TelemetryFn onTelemetry_;
};

}  // namespace rover
