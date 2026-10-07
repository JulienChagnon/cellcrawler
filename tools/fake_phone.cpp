// Stand-in for the phone: speaks the full phone side of the UDP protocol so the PC app can
// be tested without the truck. Video is a synthetic test pattern encoded with whatever
// H.264 encoder FFmpeg has on this machine (Media Foundation / NVENC / QSV / AMF).
//
//   fpv-fake-phone --key <passphrase or hex> [--host 127.0.0.1] [--port 47000] [--fps 60] [--kbps 4000]
//                  [--loss <percent>]
//
// Run on the same PC as fpv-pc and the OSD latency is exact (both share one clock).
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include "fpv/net_proto.h"
#include "fpv/passphrase.h"
#include "fpv/serial_proto.h"

using namespace fpv;

static uint64_t nowUs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

struct State {
    std::mutex mu;
    bool acked = false;
    uint64_t lastPcTimeUs = 0, lastCtrlRecvUs = 0;
    net::Control ctrl;
    uint32_t controls = 0;
    std::atomic<bool> keyframe{false};
    std::atomic<uint32_t> targetKbps{0};
};

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string host = "127.0.0.1", keyHex;
    int port = net::kDefaultPort, fps = 60, kbps = 4000;
    double lossPct = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i], v = argv[i + 1];
        if (a == "--host") host = v;
        else if (a == "--port") port = std::stoi(v);
        else if (a == "--key") keyHex = v;
        else if (a == "--fps") fps = std::stoi(v);
        else if (a == "--kbps") kbps = std::stoi(v);
        else if (a == "--loss") lossPct = std::stod(v);
    }
    SipKey key;
    if (const char* err = secretToKey(keyHex.c_str(), key)) {
        std::fprintf(stderr, "%s\nusage: fpv-fake-phone --key <passphrase or 32 hex> [--host h] [--port p] [--fps n] [--kbps n] [--loss pct]\n", err);
        return 2;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        std::fprintf(stderr, "cannot resolve %s\n", host.c_str());
        return 1;
    }
    SOCKET s = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    connect(s, res->ai_addr, int(res->ai_addrlen));
    freeaddrinfo(res);
    DWORD to = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);

    std::random_device rd;
    const uint32_t session = rd() | 1;
    std::atomic<uint32_t> seq{1};
    std::mt19937 rng(rd());
    std::uniform_real_distribution<double> uni(0, 100);
    auto sendPkt = [&](uint8_t* buf, ByteWriter& w, bool droppable) {
        size_t n = net::finishPacket(w, buf, key);
        if (droppable && uni(rng) < lossPct) return;  // simulated loss
        send(s, reinterpret_cast<const char*>(buf), int(n), 0);
    };

    State st;
    std::atomic<bool> running{true};
    std::thread rx([&] {
        uint8_t buf[2048];
        net::ReplayWindow replay;
        while (running) {
            int n = recv(s, reinterpret_cast<char*>(buf), sizeof buf, 0);
            if (n <= 0) continue;
            net::Header h;
            const uint8_t* pl;
            size_t plen;
            if (!net::openPacket(buf, size_t(n), key, h, pl, plen) || !replay.accept(h.seq)) continue;
            std::lock_guard<std::mutex> lk(st.mu);
            if (h.type == net::kHelloAck) {
                net::HelloAck a;
                if (net::read(ByteReader(pl, plen), a) && a.phoneSession == session && !st.acked) {
                    st.acked = true;
                    std::printf("connected (HELLO acknowledged)\n");
                }
            } else if (h.type == net::kControl) {
                if (net::read(ByteReader(pl, plen), st.ctrl)) {
                    st.lastPcTimeUs = st.ctrl.pcTimeUs;
                    st.lastCtrlRecvUs = nowUs();
                    ++st.controls;
                }
            } else if (h.type == net::kVideoFeedback) {
                net::VideoFeedback f;
                if (net::read(ByteReader(pl, plen), f)) {
                    if (f.requestKeyframe) st.keyframe = true;
                }
            }
        }
    });

    // ---- encoder ----
    const char* names[] = {"libopenh264", "h264_nvenc", "h264_qsv", "h264_amf", "h264_mf"};
    const AVCodec* codec = nullptr;
    AVCodecContext* enc = nullptr;
    const int W = 1280, H = 720;
    for (const char* name : names) {
        codec = avcodec_find_encoder_by_name(name);
        if (!codec) continue;
        enc = avcodec_alloc_context3(codec);
        enc->width = W;
        enc->height = H;
        enc->time_base = {1, fps};
        enc->framerate = {fps, 1};
        enc->bit_rate = int64_t(kbps) * 1000;
        enc->gop_size = fps * 2;
        enc->max_b_frames = 0;
        enc->flags |= AV_CODEC_FLAG_LOW_DELAY;
        const void* fmts = nullptr;
        int nfmts = 0;
        enc->pix_fmt = AV_PIX_FMT_YUV420P;
        if (avcodec_get_supported_config(enc, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &fmts, &nfmts) >= 0 && fmts) {
            const AVPixelFormat* f = static_cast<const AVPixelFormat*>(fmts);
            enc->pix_fmt = f[0];
            for (int i = 0; i < nfmts; ++i)
                if (f[i] == AV_PIX_FMT_NV12 || f[i] == AV_PIX_FMT_YUV420P) { enc->pix_fmt = f[i]; break; }
        }
        av_opt_set(enc->priv_data, "preset", "p1", 0);
        av_opt_set(enc->priv_data, "tune", "ull", 0);
        av_opt_set(enc->priv_data, "zerolatency", "1", 0);
        av_opt_set(enc->priv_data, "usage", "ultralowlatency", 0);
        av_opt_set(enc->priv_data, "scenario", "display_remoting", 0);
        av_opt_set(enc->priv_data, "rate_control", "cbr", 0);
        av_opt_set(enc->priv_data, "hw_encoding", "1", 0);
        if (avcodec_open2(enc, codec, nullptr) == 0) break;
        avcodec_free_context(&enc);
    }
    if (!enc) {
        std::fprintf(stderr, "no usable H.264 encoder in this FFmpeg build\n");
        return 1;
    }
    std::printf("encoder %s %dx%d@%d %d kbps, sending to %s:%d\n", codec->name, W, H, fps, kbps, host.c_str(), port);

    AVFrame* fr = av_frame_alloc();
    fr->format = enc->pix_fmt;
    fr->width = W;
    fr->height = H;
    av_frame_get_buffer(fr, 0);
    AVPacket* pkt = av_packet_alloc();

    uint64_t start = nowUs(), nextFrameUs = start, lastTelUs = 0, lastHelloUs = 0, lastPrintUs = start;
    uint32_t frameId = 0;
    int64_t pts = 0;
    uint16_t battMv = 12400;
    while (true) {
        uint64_t now = nowUs();
        bool acked;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            acked = st.acked;
        }
        if (now - lastHelloUs > (acked ? 2000000u : 500000u)) {
            lastHelloUs = now;
            uint8_t buf[64];
            ByteWriter w = net::beginPacket(buf, sizeof buf, {net::kHello, session, seq++});
            net::write(w, net::Hello{uint16_t(W), uint16_t(H), uint8_t(fps)});
            sendPkt(buf, w, false);
        }
        if (now - lastTelUs >= 50000) {
            lastTelUs = now;
            net::Telemetry t;
            std::lock_guard<std::mutex> lk(st.mu);
            if (st.lastPcTimeUs) {
                t.echoPcTimeUs = st.lastPcTimeUs;
                t.echoHoldUs = uint32_t(now - st.lastCtrlRecvUs);
            }
            t.phoneTimeUs = nowUs();
            if (frameId % 600 == 0 && battMv > 10500) battMv -= 10;
            t.battMv = battMv;
            t.cells = 3;
            bool fresh = st.lastCtrlRecvUs && now - st.lastCtrlRecvUs < 250000;
            t.espFlags = fresh ? 0 : ser::kTelFailsafe;
            if (fresh && (st.ctrl.flags & ser::kArmed)) t.espFlags |= ser::kTelArmed;
            if (st.ctrl.flags & ser::kGearReverse) t.espFlags |= ser::kTelGearReverse;
            t.steerUs = uint16_t(1500 + st.ctrl.steer * 400 / 1000);
            t.throttleUs = 1500;
            t.espRttUs = 1300;
            t.phoneFlags = net::kPhoneUsbOk | net::kPhoneEspOk | net::kPhoneVideoOn | (fresh ? 0 : net::kPhoneFailsafe);
            t.phoneBattPct = 81;
            t.phoneTempDeciC = 362;
            t.rsrpDbm = -93;
            t.netType = 2;
            t.videoBitrate = uint32_t(kbps) * 1000;
            t.videoFps = uint8_t(fps);
            uint8_t buf[128];
            ByteWriter w = net::beginPacket(buf, sizeof buf, {net::kTelemetry, session, seq++});
            net::write(w, t);
            sendPkt(buf, w, true);
        }
        if (now - lastPrintUs > 1000000) {
            lastPrintUs = now;
            std::lock_guard<std::mutex> lk(st.mu);
            std::printf("controls/s %3u  steer %+5d thr %4u brk %4u flags %02x\n", st.controls, st.ctrl.steer,
                        st.ctrl.throttle, st.ctrl.brake, st.ctrl.flags);
            st.controls = 0;
        }

        if (now < nextFrameUs) {
            Sleep(1);
            continue;
        }
        nextFrameUs += 1000000 / fps;
        if (!acked) continue;

        // Test pattern: grey ramp, moving white bar, frame counter as binary squares.
        const uint64_t captureUs = nowUs();
        av_frame_make_writable(fr);
        for (int y = 0; y < H; ++y) {
            uint8_t* row = fr->data[0] + y * fr->linesize[0];
            for (int x = 0; x < W; ++x) row[x] = uint8_t(40 + x * 120 / W);
        }
        int bx = int((captureUs - start) / 2500 % W);
        for (int y = 0; y < H; ++y) std::memset(fr->data[0] + y * fr->linesize[0] + (bx < W - 40 ? bx : W - 40), 235, 40);
        for (int b = 0; b < 16; ++b)
            if (frameId >> b & 1)
                for (int y = 300; y < 340; ++y) std::memset(fr->data[0] + y * fr->linesize[0] + 100 + b * 60, 235, 40);
        if (enc->pix_fmt == AV_PIX_FMT_NV12) {
            for (int y = 0; y < H / 2; ++y) std::memset(fr->data[1] + y * fr->linesize[1], 128, W);
        } else {
            for (int y = 0; y < H / 2; ++y) {
                std::memset(fr->data[1] + y * fr->linesize[1], 128, W / 2);
                std::memset(fr->data[2] + y * fr->linesize[2], 128, W / 2);
            }
        }
        fr->pts = pts++;
        fr->pict_type = st.keyframe.exchange(false) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (fr->pict_type == AV_PICTURE_TYPE_I) fr->flags |= AV_FRAME_FLAG_KEY;
        else fr->flags &= ~AV_FRAME_FLAG_KEY;
        if (avcodec_send_frame(enc, fr) < 0) continue;
        while (avcodec_receive_packet(enc, pkt) == 0) {
            const uint32_t id = ++frameId;
            const uint16_t count = uint16_t((pkt->size + net::kMaxVideoChunk - 1) / net::kMaxVideoChunk);
            const uint32_t phoneLat = uint32_t(nowUs() - captureUs);
            for (uint16_t i = 0; i < count; ++i) {
                net::VideoHeader vh;
                vh.frameId = id;
                vh.chunkIdx = i;
                vh.chunkCount = count;
                vh.flags = (pkt->flags & AV_PKT_FLAG_KEY) ? net::kVidKeyframe : 0;
                vh.captureUs = captureUs;
                vh.phoneLatUs = phoneLat;
                size_t off = size_t(i) * net::kMaxVideoChunk;
                size_t len = std::min<size_t>(net::kMaxVideoChunk, size_t(pkt->size) - off);
                uint8_t buf[net::kMaxDatagram];
                ByteWriter w = net::beginPacket(buf, sizeof buf, {net::kVideo, session, seq++});
                net::write(w, vh);
                w.bytes(pkt->data + off, len);
                sendPkt(buf, w, true);
            }
            av_packet_unref(pkt);
        }
    }
}
