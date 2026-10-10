#include "engine.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "common.h"

namespace rover {

using namespace fpv;

namespace {
constexpr uint64_t kHelloFastUs = 500000;
constexpr uint64_t kHelloSlowUs = 2000000;  // doubles as NAT keepalive
constexpr uint64_t kPcTimeoutUs = 3000000;
constexpr uint64_t kTelemetryUs = 50000;
constexpr uint64_t kFailsafeUs = 250000;
constexpr uint64_t kReopenUs = 10000000;
}  // namespace

void raiseThreadPriority(int nice) { setpriority(PRIO_PROCESS, 0, nice); }

Engine::Engine(const EngineConfig& cfg, UsbSerial& usb) : cfg_(cfg), usb_(usb) { session_ = arc4random() | 1; }

Engine::~Engine() { stop(); }

bool Engine::start() {
    usb_.setTelemetryCallback([this](const ser::Telemetry& t, uint64_t rxUs) {
        std::lock_guard<std::mutex> lk(stateMu_);
        esp_ = t;
        espRxUs_ = rxUs;
    });
    running_ = true;
    thread_ = std::thread(&Engine::netLoop, this);
    if (!video_.start(cfg_.video, [this](const uint8_t* d, size_t n, bool key, uint64_t cap) { onVideoFrame(d, n, key, cap); }))
        LOGW("engine: video failed to start, will retry");
    return true;
}

void Engine::stop() {
    if (!running_.exchange(false)) return;
    // Network thread first: it calls into the video pipeline (feedback, restarts).
    if (thread_.joinable()) thread_.join();
    video_.stop();
    usb_.setTelemetryCallback(nullptr);
    usb_.sendControl(ser::Control{});  // neutral, disarmed
    closeSocket();
}

void Engine::setNetwork(net_handle_t handle) { network_ = handle; }

void Engine::setPhoneStats(const PhoneStats& s) {
    std::lock_guard<std::mutex> lk(stateMu_);
    phone_ = s;
}

bool Engine::openSocket() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    const std::string port = std::to_string(cfg_.port);
    const net_handle_t net = network_;
    int rc = net ? android_getaddrinfofornetwork(net, cfg_.host.c_str(), port.c_str(), &hints, &res)
                 : getaddrinfo(cfg_.host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0 || !res) {
        LOGW("net: cannot resolve %s: %s", cfg_.host.c_str(), gai_strerror(rc));
        return false;
    }
    int fd = -1;
    char desc[INET6_ADDRSTRLEN + 8] = {0};
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_UDP);
        if (fd < 0) continue;
        if (net && android_setsocknetwork(net, fd) != 0) LOGW("net: could not bind socket to the requested network");
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            const void* a = ai->ai_family == AF_INET6 ? static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(ai->ai_addr)->sin6_addr)
                                                      : static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr);
            inet_ntop(ai->ai_family, a, desc, sizeof desc);
            // Expedited-forwarding DSCP; most carriers ignore it but it costs nothing.
            int tos = 0xB8;
            if (ai->ai_family == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof tos);
            else setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
            int sndbuf = 1 << 20;
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
            break;
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        LOGW("net: could not connect a socket to %s", cfg_.host.c_str());
        return false;
    }
    std::lock_guard<std::mutex> lk(sockMu_);
    sock_ = fd;
    boundNetwork_ = net;
    peerDesc_ = std::string(desc) + ":" + port;
    LOGI("net: socket to %s (%s)", peerDesc_.c_str(), net ? "forced network" : "default network");
    return true;
}

void Engine::closeSocket() {
    std::lock_guard<std::mutex> lk(sockMu_);
    if (sock_ >= 0) ::close(sock_);
    sock_ = -1;
}

void Engine::sendDatagram(const uint8_t* buf, size_t n) {
    if (!n) return;
    std::lock_guard<std::mutex> lk(sockMu_);
    // Non-blocking: if the uplink is backed up, dropping is better than queueing.
    if (sock_ >= 0) send(sock_, buf, n, MSG_DONTWAIT | MSG_NOSIGNAL);
}

void Engine::sendHello() {
    uint8_t buf[64];
    ByteWriter w = net::beginPacket(buf, sizeof buf, {net::kHello, session_, seq_++});
    net::write(w, net::Hello{uint16_t(cfg_.video.width), uint16_t(cfg_.video.height), uint8_t(cfg_.video.fps)});
    sendDatagram(buf, net::finishPacket(w, buf, cfg_.key));
}

void Engine::netLoop() {
    raiseThreadPriority(-16);
    uint64_t lastHelloUs = 0, lastTelUs = 0, lastOpenUs = 0, lastVideoCheckUs = monoUs();
    uint8_t buf[2048];
    while (running_) {
        uint64_t now = monoUs();
        // (Re)open: first run, network changed, or no answer for a while (DNS may have moved).
        bool reopen = sock_ < 0 || network_ != boundNetwork_ || (!acked_ && now - lastOpenUs > kReopenUs);
        if (reopen && now - lastOpenUs > 2000000) {
            lastOpenUs = now;
            closeSocket();
            acked_ = false;
            openSocket();
        }
        int fd;
        {
            std::lock_guard<std::mutex> lk(sockMu_);
            fd = sock_;
        }
        if (fd >= 0) {
            pollfd p{fd, POLLIN, 0};
            if (poll(&p, 1, 5) > 0) {
                ssize_t n;
                while ((n = recv(fd, buf, sizeof buf, MSG_DONTWAIT)) > 0) handlePacket(buf, size_t(n), monoUs());
            }
        } else {
            usleep(20000);
        }

        now = monoUs();
        if (acked_ && now - lastPcRxUs_ > kPcTimeoutUs) {
            LOGW("net: PC silent for %llu ms, reconnecting", (unsigned long long)((now - lastPcRxUs_) / 1000));
            acked_ = false;
        }
        if (now - lastHelloUs > (acked_ ? kHelloSlowUs : kHelloFastUs)) {
            lastHelloUs = now;
            sendHello();
        }
        if (acked_ && now - lastTelUs >= kTelemetryUs) {
            lastTelUs = now;
            sendTelemetry(now);
        }
        failsafeTick(now);

        if (now - lastVideoCheckUs > 3000000) {
            lastVideoCheckUs = now;
            if (!video_.running() || video_.failed()) {
                LOGW("engine: restarting video");
                video_.start(cfg_.video, [this](const uint8_t* d, size_t n, bool key, uint64_t cap) { onVideoFrame(d, n, key, cap); });
            }
        }
    }
}

void Engine::handlePacket(const uint8_t* buf, size_t n, uint64_t now) {
    net::Header h;
    const uint8_t* pl;
    size_t plen;
    if (!net::openPacket(buf, n, cfg_.key, h, pl, plen)) return;

    if (h.type == net::kHelloAck) {
        net::HelloAck a;
        if (!net::read(ByteReader(pl, plen), a) || a.phoneSession != session_) return;
        if (!havePcSession_ || h.session != pcSession_) {
            havePcSession_ = true;
            pcSession_ = h.session;
            replay_.reset();
        }
    } else if (!havePcSession_ || h.session != pcSession_) {
        return;  // only a HELLO_ACK for our session may establish the PC session
    }
    if (!replay_.accept(h.seq)) return;
    lastPcRxUs_ = now;

    switch (h.type) {
        case net::kHelloAck:
            if (!acked_) {
                LOGI("net: connected to PC %s", peerDesc_.c_str());
                video_.requestKeyframe();  // the PC needs one to start decoding
            }
            acked_ = true;
            break;
        case net::kControl: {
            net::Control c;
            if (!net::read(ByteReader(pl, plen), c)) break;
            ser::Control sc;
            sc.steer = c.steer;
            sc.throttle = c.throttle;
            sc.brake = c.brake;
            sc.flags = c.flags;
            usb_.sendControl(sc);  // straight out to the ESP32
            std::lock_guard<std::mutex> lk(stateMu_);
            lastControlRxUs_ = now;
            lastPcTimeUs_ = c.pcTimeUs;
            ++controlsRx_;
            if (failsafe_) {
                failsafe_ = false;
                LOGI("control: link up");
            }
            break;
        }
        case net::kVideoFeedback: {
            net::VideoFeedback f;
            if (net::read(ByteReader(pl, plen), f)) video_.onFeedback(f);
            break;
        }
        default: break;
    }
}

void Engine::failsafeTick(uint64_t now) {
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        const uint64_t sinceControl = lastControlRxUs_ ? now - lastControlRxUs_ : UINT64_MAX;
        if (sinceControl <= kFailsafeUs) return;
        if (!failsafe_) {
            failsafe_ = true;
            LOGW("control: no commands from PC, failsafe");
        }
        if (now - lastFailsafeTxUs_ < 20000) return;
        lastFailsafeTxUs_ = now;
    }
    // Tell the ESP32 the link is lost (it would also time out on its own). It applies its
    // failsafe brake and handles re-latching the throttle when commands resume.
    ser::Control lost;
    lost.flags = ser::kFailsafe;
    usb_.sendControl(lost);
}

void Engine::sendTelemetry(uint64_t now) {
    net::Telemetry t;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        t.echoPcTimeUs = lastPcTimeUs_;
        t.echoHoldUs = lastControlRxUs_ ? uint32_t(now - lastControlRxUs_) : 0;
        const bool espFresh = espRxUs_ && now - espRxUs_ < 500000;
        if (espFresh) {
            t.battMv = esp_.battMv;
            t.cells = esp_.cells;
            t.espFlags = esp_.flags;
            t.steerUs = esp_.steerUs;
            t.throttleUs = esp_.throttleUs;
            t.espRxErrors = esp_.rxErrors;
        }
        t.phoneFlags = uint8_t((failsafe_ ? net::kPhoneFailsafe : 0) | (usb_.isOpen() ? net::kPhoneUsbOk : 0) |
                               (espFresh ? net::kPhoneEspOk : 0) | (phone_.charging ? net::kPhoneCharging : 0) |
                               (video_.running() ? net::kPhoneVideoOn : 0));
        t.phoneBattPct = phone_.battPct;
        t.phoneTempDeciC = phone_.tempDeciC;
        t.rsrpDbm = phone_.rsrpDbm;
        t.netType = phone_.netType;
    }
    t.espRttUs = usb_.rttUs();
    t.videoBitrate = video_.bitrate();
    t.videoFps = uint8_t(video_.fps() + 0.5f);
    t.phoneTimeUs = monoUs();

    uint8_t buf[128];
    ByteWriter w = net::beginPacket(buf, sizeof buf, {net::kTelemetry, session_, seq_++});
    net::write(w, t);
    sendDatagram(buf, net::finishPacket(w, buf, cfg_.key));
}

void Engine::onVideoFrame(const uint8_t* data, size_t len, bool key, uint64_t captureUs) {
    if (!acked_) return;
    const size_t count = (len + net::kMaxVideoChunk - 1) / net::kMaxVideoChunk;
    if (count == 0 || count > 0xFFFF) return;
    const uint32_t id = ++frameId_;
    const uint64_t now = monoUs();
    net::VideoHeader vh;
    vh.frameId = id;
    vh.chunkCount = uint16_t(count);
    vh.flags = key ? net::kVidKeyframe : 0;
    vh.captureUs = captureUs;
    vh.phoneLatUs = now > captureUs ? uint32_t(now - captureUs) : 0;
    uint8_t buf[net::kMaxDatagram];
    for (size_t i = 0; i < count; ++i) {
        vh.chunkIdx = uint16_t(i);
        const size_t off = i * net::kMaxVideoChunk;
        const size_t n = std::min(net::kMaxVideoChunk, len - off);
        ByteWriter w = net::beginPacket(buf, sizeof buf, {net::kVideo, session_, seq_++});
        net::write(w, vh);
        w.bytes(data + off, n);
        sendDatagram(buf, net::finishPacket(w, buf, cfg_.key));
    }
}

std::string Engine::status() {
    std::ostringstream s;
    const uint64_t now = monoUs();
    std::string peer;
    {
        std::lock_guard<std::mutex> lk(sockMu_);
        peer = peerDesc_;
    }
    std::lock_guard<std::mutex> lk(stateMu_);
    s << "PC:     " << (acked_ ? "connected " : "waiting for ") << cfg_.host << ":" << cfg_.port;
    if (!peer.empty()) s << " (" << peer << ")";
    s << "\nControl: " << (failsafe_ ? "FAILSAFE (no commands)" : "receiving") << "\n";
    s << "USB:    " << (usb_.isOpen() ? usb_.chip() : std::string("not connected"));
    if (usb_.rttUs()) s << "  rtt " << usb_.rttUs() / 1000.0 << " ms";
    s << "\n";
    if (espRxUs_ && now - espRxUs_ < 500000) {
        s << "ESP32:  batt " << esp_.battMv / 1000.0 << " V (" << int(esp_.cells) << "S)  steer " << esp_.steerUs
          << "  thr " << esp_.throttleUs << ((esp_.flags & ser::kTelFailsafe) ? "  FAILSAFE" : "")
          << ((esp_.flags & ser::kTelArmed) ? "  ARMED" : "") << "\n";
    } else {
        s << "ESP32:  no telemetry\n";
    }
    s << "Video:  " << (video_.running() ? video_.info() : std::string("stopped")) << "  " << int(video_.fps() + 0.5f)
      << " fps  " << video_.bitrate() / 1000 << " kbps\n";
    return s.str();
}

}  // namespace rover
