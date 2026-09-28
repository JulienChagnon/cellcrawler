#include "usb_serial.h"

#include <errno.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>

#include <cstring>

#include "common.h"

namespace rover {

using namespace fpv;

namespace {
constexpr uint8_t kVendorOutIface = 0x41;   // vendor | host->device | interface
constexpr uint8_t kVendorOutDevice = 0x40;  // vendor | host->device | device
constexpr uint8_t kVendorInDevice = 0xC0;
constexpr uint8_t kClassOutIface = 0x21;    // class | host->device | interface
constexpr unsigned kCtrlTimeoutMs = 200;
}  // namespace

int UsbSerial::controlOut(uint8_t reqType, uint8_t req, uint16_t value, uint16_t index, const void* data, uint16_t len) {
    usbdevfs_ctrltransfer ct{};
    ct.bRequestType = reqType;
    ct.bRequest = req;
    ct.wValue = value;
    ct.wIndex = index;
    ct.wLength = len;
    ct.timeout = kCtrlTimeoutMs;
    ct.data = const_cast<void*>(data);
    return ioctl(fd_, USBDEVFS_CONTROL, &ct);
}

int UsbSerial::controlIn(uint8_t reqType, uint8_t req, uint16_t value, uint16_t index, void* data, uint16_t len) {
    usbdevfs_ctrltransfer ct{};
    ct.bRequestType = reqType;
    ct.bRequest = req;
    ct.wValue = value;
    ct.wIndex = index;
    ct.wLength = len;
    ct.timeout = kCtrlTimeoutMs;
    ct.data = data;
    return ioctl(fd_, USBDEVFS_CONTROL, &ct);
}

// Silicon Labs AN571.
bool UsbSerial::initCp210x() {
    const uint16_t i = uint16_t(ctrlIface_);
    if (controlOut(kVendorOutIface, 0x00, 0x0001, i, nullptr, 0) < 0) return false;  // IFC_ENABLE
    uint32_t baud = ser::kBaud;
    uint8_t b[4] = {uint8_t(baud), uint8_t(baud >> 8), uint8_t(baud >> 16), uint8_t(baud >> 24)};
    if (controlOut(kVendorOutIface, 0x1E, 0, i, b, 4) < 0) return false;             // SET_BAUDRATE
    if (controlOut(kVendorOutIface, 0x03, 0x0800, i, nullptr, 0) < 0) return false;  // SET_LINE_CTL 8N1
    // SET_MHS: DTR and RTS released (mask bits 8/9 set, values 0) so the ESP32 is not reset.
    if (controlOut(kVendorOutIface, 0x07, 0x0300, i, nullptr, 0) < 0) return false;
    controlOut(kVendorOutIface, 0x12, 0x000F, i, nullptr, 0);  // PURGE
    return true;
}

// Sequence from the Linux ch341 driver / usb-serial-for-android.
bool UsbSerial::initCh34x() {
    uint8_t ver[2];
    if (controlIn(kVendorInDevice, 0x5F, 0, 0, ver, 2) < 0) return false;
    if (controlOut(kVendorOutDevice, 0xA1, 0, 0, nullptr, 0) < 0) return false;
    auto setBaud = [&]() -> bool {
        uint32_t divisor, factor;
        if (ser::kBaud == 921600) {
            divisor = 7;
            factor = 0xF300;
        } else {
            const uint64_t kFactor = 1532620800ULL;
            factor = uint32_t(kFactor / ser::kBaud);
            divisor = 3;
            while (factor > 0xFFF0 && divisor > 0) {
                factor >>= 3;
                --divisor;
            }
            factor = 0x10000 - factor;
        }
        divisor |= 0x80;
        return controlOut(kVendorOutDevice, 0x9A, 0x1312, uint16_t((factor & 0xFF00) | divisor), nullptr, 0) >= 0 &&
               controlOut(kVendorOutDevice, 0x9A, 0x0F2C, uint16_t(factor & 0xFF), nullptr, 0) >= 0;
    };
    if (!setBaud()) return false;
    if (controlOut(kVendorOutDevice, 0x9A, 0x2518, 0x00C3, nullptr, 0) < 0) return false;  // 8N1, RX/TX on
    if (controlOut(kVendorOutDevice, 0xA1, 0x501F, 0xD90A, nullptr, 0) < 0) return false;
    if (!setBaud()) return false;
    // Modem control: DTR/RTS released (active-low bits inverted).
    return controlOut(kVendorOutDevice, 0xA4, 0xFFFF, 0, nullptr, 0) >= 0;
}

bool UsbSerial::initCdc() {
    uint32_t baud = ser::kBaud;
    uint8_t lc[7] = {uint8_t(baud), uint8_t(baud >> 8), uint8_t(baud >> 16), uint8_t(baud >> 24), 0, 0, 8};
    if (controlOut(kClassOutIface, 0x20, 0, uint16_t(ctrlIface_), lc, 7) < 0) return false;  // SET_LINE_CODING
    return controlOut(kClassOutIface, 0x22, 0, uint16_t(ctrlIface_), nullptr, 0) >= 0;      // DTR/RTS off
}

bool UsbSerial::open(int fd, int vid, int pid, int ctrlIface, int epIn, int epOut) {
    close();
    fd_ = fd;
    ctrlIface_ = ctrlIface;
    epIn_ = uint8_t(epIn);
    epOut_ = uint8_t(epOut);
    if (vid == 0x10C4) chip_ = Chip::Cp210x;
    else if (vid == 0x1A86 && pid != 0x55D4) chip_ = Chip::Ch34x;  // 0x55D4 = CH9102, which is CDC
    else chip_ = Chip::Cdc;

    bool ok = chip_ == Chip::Cp210x ? initCp210x() : chip_ == Chip::Ch34x ? initCh34x() : initCdc();
    if (!ok) {
        LOGE("usb: init of %s (%04x:%04x) failed: %s", chip().c_str(), vid, pid, strerror(errno));
        chip_ = Chip::None;
        fd_ = -1;
        return false;
    }
    LOGI("usb: %s %04x:%04x open at %u baud", chip().c_str(), vid, pid, unsigned(ser::kBaud));
    open_ = true;
    running_ = true;
    reader_ = std::thread(&UsbSerial::readLoop, this);
    return true;
}

void UsbSerial::close() {
    running_ = false;
    if (reader_.joinable()) reader_.join();
    std::lock_guard<std::mutex> lk(writeMu_);  // no write may be using fd_ past this point
    open_ = false;
    fd_ = -1;  // owned by Java's UsbDeviceConnection, which closes it after we return
    chip_ = Chip::None;
    rttUs_ = 0;
}

std::string UsbSerial::chip() const {
    switch (chip_) {
        case Chip::Cp210x: return "CP210x";
        case Chip::Ch34x: return "CH34x";
        case Chip::Cdc: return "CDC-ACM";
        default: return "none";
    }
}

void UsbSerial::setTelemetryCallback(TelemetryFn fn) {
    std::lock_guard<std::mutex> lk(cbMu_);
    onTelemetry_ = std::move(fn);
}

bool UsbSerial::bulkWrite(const uint8_t* data, size_t len) {
    usbdevfs_bulktransfer bt{};
    bt.ep = epOut_;
    bt.len = unsigned(len);
    bt.timeout = 20;
    bt.data = const_cast<uint8_t*>(data);
    return ioctl(fd_, USBDEVFS_BULK, &bt) == int(len);
}

bool UsbSerial::sendControl(ser::Control c) {
    std::lock_guard<std::mutex> lk(writeMu_);
    if (!open_) return false;
    c.seq = seq_++;
    if (c.seq % ser::kEchoEvery == 0) sentUs_[(c.seq / ser::kEchoEvery) % 64] = monoUs();
    uint8_t frame[ser::kMaxFrame];
    size_t n = ser::encodeControl(c, frame);
    return bulkWrite(frame, n);
}

void UsbSerial::readLoop() {
    raiseThreadPriority(-10);
    ser::FrameParser parser;
    uint8_t buf[256];
    uint16_t lastEcho = 0xFFFF;
    while (running_) {
        usbdevfs_bulktransfer bt{};
        bt.ep = epIn_;
        bt.len = sizeof buf;
        bt.timeout = 100;
        bt.data = buf;
        int n = ioctl(fd_, USBDEVFS_BULK, &bt);
        if (n < 0) {
            if (errno == ETIMEDOUT) continue;
            LOGW("usb: read failed: %s", strerror(errno));
            open_ = false;
            break;
        }
        const uint64_t now = monoUs();
        parser.feed(buf, size_t(n), [&](uint8_t type, const uint8_t* p, size_t len) {
            ser::Telemetry t;
            if (type != ser::kTelemetry || !ser::decodeTelemetry(p, len, t)) return;
            // The first telemetry echoing an echo-seq is the ESP32's immediate reply.
            if (t.lastSeq % ser::kEchoEvery == 0 && t.lastSeq != lastEcho) {
                lastEcho = t.lastSeq;
                uint64_t sent = sentUs_[(t.lastSeq / ser::kEchoEvery) % 64];
                if (sent && now > sent && now - sent < 1000000) rttUs_ = uint32_t(now - sent);
            }
            std::lock_guard<std::mutex> lk(cbMu_);
            if (onTelemetry_) onTelemetry_(t, now);
        });
    }
}

}  // namespace rover
