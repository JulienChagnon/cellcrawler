// FPV truck ground station.
//
//   fpv-pc [--config fpv.ini]     run
//   fpv-pc --genkey               print a new random shared key
//
// Threads: control (1 kHz XInput poll -> send), network receive, video decode, and this
// main thread which only renders: it sleeps until a decoded frame arrives, then uploads and
// presents it immediately (vsync off by default).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <thread>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include "config.h"
#include "gamepad.h"
#include "link.h"
#include "mapping.h"
#include "osd.h"
#include "util.h"
#include "video.h"

using namespace pc;

namespace {

struct ControlView {
    bool padConnected = false;
    bool armed = false;
    bool gearRev = false;
    float scale = 1;
    int trim = 0;
    float limiter = 1;
    DriveCommand cmd;
    std::string note;
    uint64_t noteUs = 0;
};

std::mutex gViewMu;
ControlView gView;

void controlLoop(const Config& cfg, Link& link, const std::atomic<bool>& running) {
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    Gamepad pad;
    if (!pad.available()) logf("input: XInput not available");
    Mapper mapper(cfg.mapping);
    DriveCommand last;
    uint64_t lastSendUs = 0, lastLimiterUs = 0;
    float limiter = 1;
    bool wasConnected = false;

    while (running) {
        const uint64_t now = nowUs();
        if (now - lastLimiterUs > 50000) {
            lastLimiterUs = now;
            LinkStatus st = link.status();
            limiter = (cfg.mode == Config::Mode::Udp && st.rttMs > float(cfg.rttLimitMs)) ? 0.5f : 1.0f;
        }
        PadState p = pad.poll();
        if (p.connected != wasConnected) {
            logf("input: controller %s", p.connected ? "connected" : "disconnected");
            wasConnected = p.connected;
        }
        DriveCommand cmd = mapper.update(p, limiter);

        // Send on change (within 2 ms) plus a 100 Hz heartbeat. Every packet carries the
        // full state, so a lost one is simply superseded.
        if ((cmd != last && now - lastSendUs >= 2000) || now - lastSendUs >= 9500) {
            link.sendControl(cmd);
            last = cmd;
            lastSendUs = now;
        }

        std::string note = mapper.takeNote();
        {
            std::lock_guard<std::mutex> lk(gViewMu);
            gView.padConnected = p.connected;
            gView.armed = mapper.armed();
            gView.gearRev = mapper.gearReverse();
            gView.scale = mapper.scale();
            gView.trim = mapper.trim();
            gView.limiter = limiter;
            gView.cmd = cmd;
            if (!note.empty()) {
                gView.note = note;
                gView.noteUs = now;
            }
        }
        if (!note.empty()) logf("control: %s", note.c_str());
        Sleep(1);
    }
    // Leave the truck disarmed.
    link.sendControl(DriveCommand{});
    timeEndPeriod(1);
}

int genKey() {
    std::random_device rd;
    for (int i = 0; i < 4; ++i) std::printf("%08x", unsigned(rd()));
    std::printf("\n");
    return 0;
}

struct Renderer {
    SDL_Renderer* r = nullptr;
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0, fmt = -1;
    SDL_Colorspace cs = SDL_COLORSPACE_UNKNOWN;

    bool upload(const AVFrame* f) {
        SDL_PixelFormat pf;
        if (f->format == AV_PIX_FMT_NV12) pf = SDL_PIXELFORMAT_NV12;
        else if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) pf = SDL_PIXELFORMAT_IYUV;
        else {
            static bool warned = false;
            if (!warned) logf("video: unsupported pixel format %d", f->format);
            warned = true;
            return false;
        }
        const bool full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
        const bool bt709 = f->colorspace == AVCOL_SPC_BT709;
        SDL_Colorspace c = bt709 ? (full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED)
                                 : (full ? SDL_COLORSPACE_JPEG : SDL_COLORSPACE_BT601_LIMITED);
        if (!tex || w != f->width || h != f->height || fmt != int(pf) || cs != c) {
            if (tex) SDL_DestroyTexture(tex);
            SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, pf);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, f->width);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, f->height);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, c);
            tex = SDL_CreateTextureWithProperties(r, props);
            SDL_DestroyProperties(props);
            if (!tex) {
                logf("video: texture creation failed: %s", SDL_GetError());
                return false;
            }
            w = f->width;
            h = f->height;
            fmt = int(pf);
            cs = c;
            logf("video: %dx%d %s", w, h, pf == SDL_PIXELFORMAT_NV12 ? "NV12" : "I420");
        }
        if (pf == SDL_PIXELFORMAT_NV12)
            return SDL_UpdateNVTexture(tex, nullptr, f->data[0], f->linesize[0], f->data[1], f->linesize[1]);
        return SDL_UpdateYUVTexture(tex, nullptr, f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2],
                                    f->linesize[2]);
    }

    void drawVideo() {
        if (!tex) return;
        int ow = 0, oh = 0;
        SDL_GetRenderOutputSize(r, &ow, &oh);
        float s = std::min(float(ow) / float(w), float(oh) / float(h));
        SDL_FRect dst{(ow - w * s) / 2, (oh - h * s) / 2, w * s, h * s};
        SDL_RenderTexture(r, tex, nullptr, &dst);
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string configPath = "fpv.ini";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--genkey") return genKey();
        if (a == "--config" && i + 1 < argc) configPath = argv[++i];
        else {
            std::fprintf(stderr, "usage: fpv-pc [--config fpv.ini] | --genkey\n");
            return 2;
        }
    }
    Config cfg;
    if (!loadConfig(configPath, cfg)) return 1;

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        logf("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_WindowFlags wf = SDL_WINDOW_RESIZABLE | (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    SDL_Window* window = SDL_CreateWindow("FPV Truck", cfg.windowWidth, cfg.windowHeight, wf);
    Renderer ren;
    ren.r = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!ren.r) {
        logf("window/renderer creation failed: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(ren.r, cfg.vsync ? 1 : 0);
    logf("render: %s, vsync %s", SDL_GetRendererName(ren.r), cfg.vsync ? "on" : "off");

    const Uint32 frameEvent = SDL_RegisterEvents(1);
    VideoReceiver video(cfg.hwDecode);
    video.onFrameReady = [frameEvent] {
        SDL_Event e{};
        e.type = frameEvent;
        SDL_PushEvent(&e);
    };

    std::unique_ptr<Link> link = cfg.mode == Config::Mode::Udp ? makeUdpLink(cfg, &video) : makeSerialLink(cfg);
    if (!link->start()) return 1;
    if (cfg.mode == Config::Mode::Udp) video.start();

    std::atomic<bool> running{true};
    std::thread control(controlLoop, std::cref(cfg), std::ref(*link), std::cref(running));

    Latency lat;
    uint64_t lastDrawUs = 0;
    bool fullscreen = cfg.fullscreen;
    while (running) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 20)) {
            do {
                if (e.type == SDL_EVENT_QUIT) running = false;
                else if (e.type == SDL_EVENT_KEY_DOWN) {
                    if (e.key.key == SDLK_ESCAPE) running = false;
                    else if (e.key.key == SDLK_F11) SDL_SetWindowFullscreen(window, fullscreen = !fullscreen);
                }
            } while (SDL_PollEvent(&e));
        }

        FrameTiming timing;
        AVFrame* f = video.takeFrame(timing);
        bool newFrame = false;
        if (f) {
            newFrame = ren.upload(f);
            av_frame_free(&f);
        }
        const uint64_t now = nowUs();
        if (!newFrame && now - lastDrawUs < 33000) continue;  // OSD refresh at ~30 Hz without video
        lastDrawUs = now;

        OsdData d;
        d.cfg = &cfg;
        d.link = link->status();
        d.video = video.stats();
        {
            std::lock_guard<std::mutex> lk(gViewMu);
            d.padConnected = gView.padConnected;
            d.armed = gView.armed;
            d.gearRev = gView.gearRev;
            d.scale = gView.scale;
            d.trim = gView.trim;
            d.limiter = gView.limiter;
            d.cmd = gView.cmd;
            if (now - gView.noteUs < 2000000) d.note = gView.note;
        }
        d.latency = lat;
        d.nowUs = now;

        SDL_SetRenderDrawColor(ren.r, 0, 0, 0, 255);
        SDL_RenderClear(ren.r);
        ren.drawVideo();
        drawOsd(ren.r, d);
        SDL_RenderPresent(ren.r);

        if (newFrame && d.link.offsetValid) {
            // capture (phone clock) -> PC clock via the offset estimated from RTT/2.
            const uint64_t presentUs = nowUs();
            const double capturePc = double(timing.captureUs) - double(d.link.phoneMinusPcUs);
            const float total = float((double(presentUs) - capturePc) / 1000.0);
            const float phone = timing.phoneLatUs / 1000.0f;
            const float pcMs = float(presentUs - timing.lastRecvUs) / 1000.0f;
            const float k = lat.valid ? 0.1f : 1.0f;
            lat.totalMs += (total - lat.totalMs) * k;
            lat.phoneMs += (phone - lat.phoneMs) * k;
            lat.pcMs += (pcMs - lat.pcMs) * k;
            lat.netMs = lat.totalMs - lat.phoneMs - lat.pcMs;
            lat.valid = true;
        }
    }

    control.join();
    link->stop();
    video.stop();
    if (ren.tex) SDL_DestroyTexture(ren.tex);
    SDL_DestroyRenderer(ren.r);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
