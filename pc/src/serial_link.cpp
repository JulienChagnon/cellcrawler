// Direct USB serial to the ESP32, for bench testing controls without the phone.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "fpv/serial_proto.h"
#include "link.h"
#include "util.h"

namespace pc {

using namespace fpv;

namespace {

class SerialLink final : public Link {
public:
    explicit SerialLink(const Config& cfg) : cfg_(cfg) {
        writeEv_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    }
    ~SerialLink() override {
        stop();
        CloseHandle(writeEv_);
    }

    bool start() override {
        std::string path = "\\\\.\\" + cfg_.serialPort;
        // Overlapped so the reader thread never blocks writes (control latency).
        h_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND)
                logf("serial: %s doesn't exist. Is the ESP32 plugged into this PC? Its port is under "
                     "Device Manager -> Ports; put it in serial_port = in fpv.ini, or set mode = udp "
                     "to drive through the phone", cfg_.serialPort.c_str());
            else if (err == ERROR_ACCESS_DENIED)
                logf("serial: %s is in use by another program (close PlatformIO's monitor, Arduino, ...)",
                     cfg_.serialPort.c_str());
            else logf("serial: cannot open %s (error %lu)", cfg_.serialPort.c_str(), err);
            return false;
        }
        DCB dcb{};
        dcb.DCBlength = sizeof dcb;
        GetCommState(h_, &dcb);
        dcb.BaudRate = ser::kBaud;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        // DTR/RTS drive the NodeMCU's EN/IO0 auto-reset circuit: keep both released.
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        if (!SetCommState(h_, &dcb)) {
            logf("serial: SetCommState failed (%lu)", GetLastError());
            return false;
        }
        // Return as soon as any byte is available, or after 100 ms.
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = MAXDWORD;
        to.ReadTotalTimeoutMultiplier = MAXDWORD;
        to.ReadTotalTimeoutConstant = 100;
        to.WriteTotalTimeoutConstant = 50;
        SetCommTimeouts(h_, &to);
        PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);

        {
            std::lock_guard<std::mutex> lk(mu_);
            st_.peer = cfg_.serialPort;
        }
        logf("serial: %s open at %u baud", cfg_.serialPort.c_str(), unsigned(ser::kBaud));
        running_ = true;
        thread_ = std::thread(&SerialLink::readLoop, this);
        return true;
    }

    void stop() override {
        if (!running_.exchange(false)) return;
        CancelIoEx(h_, nullptr);
        if (thread_.joinable()) thread_.join();
        CloseHandle(h_);
    }

    void sendControl(const DriveCommand& cmd) override {
        ser::Control c;
        c.seq = seq_++;
        c.steer = cmd.steer;
        c.throttle = cmd.throttle;
        c.brake = cmd.brake;
        c.flags = cmd.flags;
        if (c.seq % ser::kEchoEvery == 0) sentUs_[(c.seq / ser::kEchoEvery) % kSentSlots] = nowUs();

        uint8_t buf[ser::kMaxFrame];
        size_t n = ser::encodeControl(c, buf);
        OVERLAPPED ov{};
        ov.hEvent = writeEv_;
        ResetEvent(writeEv_);
        DWORD written = 0;
        if (!WriteFile(h_, buf, DWORD(n), &written, &ov) && GetLastError() == ERROR_IO_PENDING)
            GetOverlappedResult(h_, &ov, &written, TRUE);
    }

    LinkStatus status() override {
        std::lock_guard<std::mutex> lk(mu_);
        LinkStatus s = st_;
        s.connected = st_.lastRxUs && nowUs() - st_.lastRxUs < 1000000;
        return s;
    }

private:
    static constexpr size_t kSentSlots = 64;

    void readLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        HANDLE ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        uint8_t buf[256];
        uint16_t lastEcho = 0xFFFF;
        while (running_) {
            OVERLAPPED ov{};
            ov.hEvent = ev;
            ResetEvent(ev);
            DWORD got = 0;
            if (!ReadFile(h_, buf, sizeof buf, &got, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(h_, &ov, &got, TRUE)) {
                    if (running_) {
                        logf("serial: read failed (%lu)", GetLastError());
                        Sleep(100);
                    }
                    continue;
                }
            }
            const uint64_t now = nowUs();
            parser_.feed(buf, got, [&](uint8_t type, const uint8_t* p, size_t len) {
                ser::Telemetry t;
                if (type != ser::kTelemetry || !ser::decodeTelemetry(p, len, t)) return;
                std::lock_guard<std::mutex> lk(mu_);
                st_.lastRxUs = st_.telRxUs = now;
                st_.haveTelemetry = true;
                st_.viaPhone = false;
                st_.tel.battMv = t.battMv;
                st_.tel.cells = t.cells;
                st_.tel.espFlags = t.flags;
                st_.tel.steerUs = t.steerUs;
                st_.tel.throttleUs = t.throttleUs;
                st_.tel.espRxErrors = t.rxErrors;
                st_.tel.phoneFlags = net::kPhoneUsbOk | net::kPhoneEspOk;
                // First telemetry echoing an echo-seq is the ESP32's immediate reply.
                if (t.lastSeq % ser::kEchoEvery == 0 && t.lastSeq != lastEcho) {
                    lastEcho = t.lastSeq;
                    uint64_t sent = sentUs_[(t.lastSeq / ser::kEchoEvery) % kSentSlots];
                    if (sent && now > sent) {
                        float rtt = float(now - sent) / 1000.0f;
                        st_.rttMs = st_.rttMs < 0 ? rtt : st_.rttMs * 0.8f + rtt * 0.2f;
                        st_.rttMinMs = st_.rttMinMs < 0 ? rtt : std::min(st_.rttMinMs, rtt);
                    }
                }
            });
        }
        CloseHandle(ev);
    }

    const Config cfg_;
    HANDLE h_ = INVALID_HANDLE_VALUE;
    HANDLE writeEv_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    uint16_t seq_ = 1;
    std::atomic<uint64_t> sentUs_[kSentSlots] = {};
    ser::FrameParser parser_;

    std::mutex mu_;
    LinkStatus st_;
};

}  // namespace

std::unique_ptr<Link> makeSerialLink(const Config& cfg) { return std::make_unique<SerialLink>(cfg); }

}  // namespace pc
