// UDP link to the phone. The phone always initiates (HELLO), so its carrier CGNAT is never
// in the way; we reply to whatever address the authenticated packets come from, which also
// follows the phone across LTE IP changes.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>

#include "link.h"
#include "util.h"
#include "video.h"

namespace pc {

using namespace fpv;

namespace {

std::string addrToString(const sockaddr_in6& a) {
    char host[INET6_ADDRSTRLEN] = {0};
    const void* src = &a.sin6_addr;
    int family = AF_INET6;
    // Show v4-mapped addresses as plain IPv4.
    static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::memcmp(&a.sin6_addr, kMapped, 12) == 0) {
        src = reinterpret_cast<const uint8_t*>(&a.sin6_addr) + 12;
        family = AF_INET;
    }
    inet_ntop(family, src, host, sizeof host);
    return (family == AF_INET6 ? "[" + std::string(host) + "]" : std::string(host)) + ":" +
           std::to_string(ntohs(a.sin6_port));
}

class UdpLink final : public Link {
public:
    UdpLink(const Config& cfg, VideoReceiver* video) : cfg_(cfg), video_(video) {
        std::random_device rd;
        session_ = (uint32_t(rd()) << 1) | 1;
    }
    ~UdpLink() override { stop(); }

    bool start() override {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        sock_ = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) {
            logf("udp: socket failed (%d)", WSAGetLastError());
            return false;
        }
        DWORD off = 0;
        setsockopt(sock_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off);  // v4 + v6
        int rcvbuf = 4 << 20;
        setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof rcvbuf);
        DWORD timeoutMs = 50;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof timeoutMs);
        // Without this, an ICMP "port unreachable" makes the next recvfrom fail with WSAECONNRESET.
        BOOL noReset = FALSE;
        DWORD ret = 0;
        WSAIoctl(sock_, SIO_UDP_CONNRESET, &noReset, sizeof noReset, nullptr, 0, &ret, nullptr, nullptr);

        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = htons(cfg_.port);
        if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            logf("udp: bind to port %u failed (%d) - is another copy running?", cfg_.port, WSAGetLastError());
            return false;
        }
        logf("udp: listening on port %u (IPv4 + IPv6), waiting for the phone", cfg_.port);
        running_ = true;
        thread_ = std::thread(&UdpLink::recvLoop, this);
        return true;
    }

    void stop() override {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        closesocket(sock_);
        WSACleanup();
    }

    void sendControl(const DriveCommand& cmd) override {
        sockaddr_in6 peer;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!havePeer_) return;
            peer = peer_;
        }
        net::Control c;
        c.pcTimeUs = nowUs();
        c.steer = cmd.steer;
        c.throttle = cmd.throttle;
        c.brake = cmd.brake;
        c.flags = cmd.flags;
        uint8_t buf[64];
        ByteWriter w = net::beginPacket(buf, sizeof buf, {net::kControl, session_, seq_++});
        net::write(w, c);
        send(buf, net::finishPacket(w, buf, cfg_.key), peer);
    }

    LinkStatus status() override {
        std::lock_guard<std::mutex> lk(mu_);
        LinkStatus s = st_;
        s.connected = st_.lastRxUs && nowUs() - st_.lastRxUs < 1000000;
        return s;
    }

private:
    void send(const uint8_t* buf, size_t n, const sockaddr_in6& to) {
        if (n) sendto(sock_, reinterpret_cast<const char*>(buf), int(n), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }

    void recvLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        uint8_t buf[2048];
        uint64_t lastFeedbackUs = 0, lossWindowUs = 0;
        while (running_) {
            sockaddr_in6 from{};
            int fromLen = sizeof from;
            int n = recvfrom(sock_, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            const uint64_t now = nowUs();
            if (n > 0) handle(buf, size_t(n), from, now);

            bool key = video_ && video_->takeKeyframeRequest();
            if (key || now - lastFeedbackUs >= 200000) {
                lastFeedbackUs = now;
                sendFeedback(now, key);
            }
            if (now - lossWindowUs >= 1000000) {
                lossWindowUs = now;
                std::lock_guard<std::mutex> lk(mu_);
                uint32_t expected = haveWinSeq_ ? maxSeq_ - winStartSeq_ : 0;
                st_.lossPct = expected ? 100.0f * (1.0f - std::min(1.0f, float(winRecv_) / float(expected))) : 0.0f;
                winStartSeq_ = maxSeq_;
                winRecv_ = 0;
            }
        }
    }

    void sendFeedback(uint64_t now, bool key) {
        sockaddr_in6 peer;
        float loss;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!havePeer_) return;
            peer = peer_;
            loss = st_.lossPct;
        }
        net::VideoFeedback f;
        if (video_) f = video_->makeFeedback(now, loss);
        f.requestKeyframe = key ? 1 : 0;
        uint8_t buf[64];
        ByteWriter w = net::beginPacket(buf, sizeof buf, {net::kVideoFeedback, session_, seq_++});
        net::write(w, f);
        send(buf, net::finishPacket(w, buf, cfg_.key), peer);
    }

    void handle(const uint8_t* buf, size_t n, const sockaddr_in6& from, uint64_t now) {
        net::Header h;
        const uint8_t* pl;
        size_t plen;
        if (!net::openPacket(buf, n, cfg_.key, h, pl, plen)) {
            std::lock_guard<std::mutex> lk(mu_);
            ++st_.badPackets;
            st_.lastBadUs = now;
            // Usually the phone with a different passphrase: say so, at most every 5 s.
            if (now - lastBadLogUs_ > 5000000) {
                lastBadLogUs_ = now;
                logf("udp: ignoring packets from %s that fail the passphrase check (%u so far). "
                     "Is the phone's passphrase exactly the one in fpv.ini?",
                     addrToString(from).c_str(), st_.badPackets);
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!havePhoneSession_ || h.session != phoneSession_) {
                if (h.type != net::kHello) return;  // only a HELLO may start a new session
                havePhoneSession_ = true;
                phoneSession_ = h.session;
                replay_.reset();
                haveWinSeq_ = false;
                st_.rttMinMs = -1;
                st_.offsetValid = false;
                logf("udp: phone session %08x from %s", h.session, addrToString(from).c_str());
            }
            if (!replay_.accept(h.seq)) return;

            // Roaming: follow the phone to its newest authenticated address.
            if (!havePeer_ || std::memcmp(&peer_, &from, sizeof from) != 0) {
                if (havePeer_) logf("udp: phone moved to %s", addrToString(from).c_str());
                peer_ = from;
                havePeer_ = true;
                st_.peer = addrToString(from);
            }
            st_.lastRxUs = now;
            if (!haveWinSeq_) {
                haveWinSeq_ = true;
                winStartSeq_ = maxSeq_ = h.seq - 1;
                winRecv_ = 0;
            }
            if (int32_t(h.seq - maxSeq_) > 0) maxSeq_ = h.seq;
            ++winRecv_;
        }

        switch (h.type) {
            case net::kHello: {
                net::Hello m;
                if (net::read(ByteReader(pl, plen), m)) {
                    std::lock_guard<std::mutex> lk(mu_);
                    st_.videoW = m.width;
                    st_.videoH = m.height;
                    st_.videoFps = m.fps;
                }
                net::HelloAck ack{h.session};
                uint8_t out[64];
                ByteWriter w = net::beginPacket(out, sizeof out, {net::kHelloAck, session_, seq_++});
                net::write(w, ack);
                send(out, net::finishPacket(w, out, cfg_.key), from);
                break;
            }
            case net::kTelemetry: {
                net::Telemetry t;
                if (!net::read(ByteReader(pl, plen), t)) break;
                std::lock_guard<std::mutex> lk(mu_);
                st_.tel = t;
                st_.haveTelemetry = true;
                st_.viaPhone = true;
                st_.telRxUs = now;
                if (t.echoPcTimeUs && now > t.echoPcTimeUs + t.echoHoldUs) {
                    float rtt = float(now - t.echoPcTimeUs - t.echoHoldUs) / 1000.0f;
                    st_.rttMs = st_.rttMs < 0 ? rtt : st_.rttMs * 0.8f + rtt * 0.2f;
                    // Slowly-forgetting minimum, so a route change can raise it again.
                    st_.rttMinMs = st_.rttMinMs < 0 ? rtt : std::min(rtt, st_.rttMinMs + 0.01f);
                    // Clock offset from the least-queued samples (assumes symmetric path).
                    if (rtt <= st_.rttMinMs * 1.25f + 2.0f) {
                        int64_t off = int64_t(t.phoneTimeUs) - int64_t(now - uint64_t(rtt * 500.0f));
                        st_.phoneMinusPcUs = st_.offsetValid ? st_.phoneMinusPcUs + (off - st_.phoneMinusPcUs) / 8 : off;
                        st_.offsetValid = true;
                    }
                }
                break;
            }
            case net::kVideo: {
                ByteReader r(pl, plen);
                net::VideoHeader vh;
                if (video_ && net::read(r, vh)) video_->onChunk(vh, r.rest(), r.remaining(), now);
                break;
            }
            default: break;
        }
    }

    const Config cfg_;
    VideoReceiver* video_;
    SOCKET sock_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> running_{false};
    uint32_t session_;
    std::atomic<uint32_t> seq_{1};

    std::mutex mu_;
    LinkStatus st_;
    bool havePeer_ = false;
    sockaddr_in6 peer_{};
    bool havePhoneSession_ = false;
    uint32_t phoneSession_ = 0;
    net::ReplayWindow replay_;
    uint64_t lastBadLogUs_ = 0;
    bool haveWinSeq_ = false;
    uint32_t winStartSeq_ = 0, maxSeq_ = 0, winRecv_ = 0;
};

}  // namespace

std::unique_ptr<Link> makeUdpLink(const Config& cfg, VideoReceiver* video) {
    return std::make_unique<UdpLink>(cfg, video);
}

}  // namespace pc
