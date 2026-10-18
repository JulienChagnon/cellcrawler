// FPV truck ground station.
//
//   fpv-pc [--config fpv.ini]     run (default: the fpv.ini next to fpv-pc.exe)
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
#include <cstdlib>
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
#include "keyboard.h"
#include "link.h"
#include "mapping.h"
#include "osd.h"
#include "recorder.h"
#include "util.h"
#include "video.h"

using namespace pc;

namespace {

struct ControlView {
    bool padConnected = false;     // the input in use (controller, or focused window for keyboard)
    bool controllerPresent = false;
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

// Keyboard state, written by the main (event) thread, read by the control loop.
std::atomic<uint16_t> gKeys{0};       // pc::Key bits currently held
std::atomic<bool> gFocused{false};    // window has keyboard focus
std::atomic<bool> gUseKeyboard{false};
std::atomic<bool> gMenuOpen{false};   // keys drive nothing while the menu is up
// Live tuning from the menu and the - / = keys, applied by the control loop.
std::atomic<int> gTrimDelta{0};
std::atomic<float> gScaleSet{-1.0f};  // < 0: no change
std::atomic<bool> gRecordPhone{false};  // F: ask the phone to record too (sent in every CONTROL)

uint16_t keyBit(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_W: return kKeyW;
        case SDL_SCANCODE_A: return kKeyA;
        case SDL_SCANCODE_S: return kKeyS;
        case SDL_SCANCODE_D: return kKeyD;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: return kKeyArm;
        case SDL_SCANCODE_BACKSPACE:
        case SDL_SCANCODE_B: return kKeyDisarm;
        case SDL_SCANCODE_R: return kKeyGear;
        case SDL_SCANCODE_UP: return kKeyUp;
        case SDL_SCANCODE_DOWN: return kKeyDown;
        case SDL_SCANCODE_LEFT: return kKeyLeft;
        case SDL_SCANCODE_RIGHT: return kKeyRight;
        default: return 0;
    }
}

void controlLoop(const Config& cfg, Link& link, const std::atomic<bool>& running) {
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    Gamepad pad;
    if (!pad.available()) logf("input: XInput not available");
    Mapper mapper(cfg.mapping);
    KeyboardPad keyboard;
    bool usingKeyboard = gUseKeyboard;
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
        const bool kb = gUseKeyboard;
        if (kb != usingKeyboard) {
            mapper.disarm(kb ? "INPUT: KEYBOARD - DISARMED" : "INPUT: CONTROLLER - DISARMED");
            logf("input: %s", kb ? "keyboard" : "controller");
            usingKeyboard = kb;
        }
        const PadState gp = pad.poll();
        if (gp.connected != wasConnected) {
            logf("input: controller %s", gp.connected ? "connected" : "disconnected");
            wasConnected = gp.connected;
        }
        if (const int dt = gTrimDelta.exchange(0)) mapper.setTrim(mapper.trim() + dt);
        if (const float sc = gScaleSet.exchange(-1.0f); sc > 0) mapper.setScale(sc);
        const PadState kp = keyboard.update(gFocused, gMenuOpen ? 0 : gKeys.load(), now);
        const PadState p = kb ? kp : gp;
        DriveCommand cmd = mapper.update(p, limiter);
        if (gRecordPhone) cmd.flags |= fpv::net::kCtlRecord;

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
            gView.controllerPresent = gp.connected;
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

// The folder fpv-pc.exe is in (with a trailing slash), so it works from any folder.
std::string exeDir() {
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    std::string p(buf, n);
    return p.substr(0, p.find_last_of("\\/") + 1);
}
std::string defaultConfigPath() { return exeDir() + "fpv.ini"; }

std::string espFlagNames(uint8_t f) {
    using namespace fpv::ser;
    std::string s;
    if (f & kTelFailsafe) s += " FAILSAFE";
    if (f & kTelArmed) s += " armed";
    if (f & kTelGearReverse) s += " gear-R";
    if (f & kTelReverseEngaged) s += " reverse-engaged";
    if (f & kTelNeedNeutral) s += " need-neutral";
    if (f & kTelLowBattery) s += " low-batt";
    return s.empty() ? " none" : s;
}
std::string phoneFlagNames(uint8_t f) {
    using namespace fpv;
    std::string s;
    if (f & net::kPhoneFailsafe) s += " FAILSAFE(no PC commands)";
    s += (f & net::kPhoneUsbOk) ? " usb-ok" : " USB-DISCONNECTED";
    s += (f & net::kPhoneEspOk) ? " esp-ok" : " ESP-NOT-RESPONDING";
    return s;
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
    std::string configPath = defaultConfigPath();
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--genkey") return genKey();
        if (a == "--config" && i + 1 < argc) configPath = argv[++i];
        else {
            std::fprintf(stderr, "usage: fpv-pc [--config fpv.ini] | --genkey\n");
            return 2;
        }
    }
    logToFile(exeDir() + "fpv-pc.log");
    logf("fpv-pc starting, config %s", configPath.c_str());
    Config cfg;
    if (!loadConfig(configPath, cfg)) return 1;
    logf("config: mode %s, input %s, throttle limit %.0f%%, steer trim %d",
         cfg.mode == Config::Mode::Udp ? "udp" : "serial", cfg.keyboard ? "keyboard" : "controller",
         cfg.mapping.throttleScale * 100, cfg.mapping.steerTrim);

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
    {
        // What the OSD shows (and the save check compares) before the control loop's first pass.
        Mapper seed(cfg.mapping);
        gView.scale = seed.scale();
        gView.trim = seed.trim();
    }
    std::thread control(controlLoop, std::cref(cfg), std::ref(*link), std::cref(running));

    Latency lat;
    uint64_t lastDrawUs = 0;
    bool fullscreen = cfg.fullscreen;
    gUseKeyboard = cfg.keyboard;
    gFocused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    int menuSel = 0;
    constexpr int kMenuItems = 5;  // input, throttle limit, steering trim, details, fullscreen
    bool details = false;
    std::string popup;
    uint64_t popupUs = 0;
    ScreenRecorder recorder;
    const std::string recordDir = cfg.recordDir.empty() ? exeDir() + "recordings" : cfg.recordDir;
    SDL_Texture* canvas = nullptr;  // off-screen frame while recording
    int canvasW = 0, canvasH = 0;
    // Throttle limit and trim as last shown, and as last written to fpv.ini.
    float shownScale = gView.scale, savedScale = cfg.mapping.throttleScale;
    int shownTrim = gView.trim, savedTrim = cfg.mapping.steerTrim;
    uint64_t tuneChangedUs = 0;
    // For the log: state changes, and a once-a-second line while armed.
    bool prevConnected = false, prevTelFresh = false, prevArmed = false, prevPad = true;
    uint8_t prevEsp = 0xFF, prevPhone = 0xFF;
    uint64_t lastDriveLogUs = 0;
    while (running) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 20)) {
            do {
                if (e.type == SDL_EVENT_QUIT) running = false;
                else if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) gFocused = true;
                else if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    gFocused = false;
                    gKeys = 0;
                } else if (e.type == SDL_EVENT_KEY_UP) {
                    gKeys &= uint16_t(~keyBit(e.key.scancode));
                } else if (e.type == SDL_EVENT_KEY_DOWN) {
                    if (!e.key.repeat) gKeys |= keyBit(e.key.scancode);
                    const SDL_Scancode sc = e.key.scancode;
                    if (sc == SDL_SCANCODE_TAB && !e.key.repeat) gMenuOpen = !gMenuOpen;
                    else if (sc == SDL_SCANCODE_F11) SDL_SetWindowFullscreen(window, fullscreen = !fullscreen);
                    else if (sc == SDL_SCANCODE_F3) details = !details;
                    else if (sc == SDL_SCANCODE_F && !e.key.repeat) {
                        if (!recorder.active()) {
                            int ow = 0, oh = 0;
                            SDL_GetRenderOutputSize(ren.r, &ow, &oh);
                            const bool ok = recorder.start(recordDir, ow, oh);
                            if (cfg.mode == Config::Mode::Udp) gRecordPhone = true;
                            popup = ok ? "RECORDING" : "PC RECORDING FAILED - SEE LOG";
                        } else {
                            recorder.stop();
                            gRecordPhone = false;
                            popup = "RECORDING SAVED";
                        }
                        popupUs = nowUs();
                    }
                    else if (sc == SDL_SCANCODE_MINUS || sc == SDL_SCANCODE_KP_MINUS) gTrimDelta -= 5;
                    else if (sc == SDL_SCANCODE_EQUALS || sc == SDL_SCANCODE_KP_PLUS) gTrimDelta += 5;
                    else if (gMenuOpen) {
                        if (sc == SDL_SCANCODE_ESCAPE) gMenuOpen = false;
                        else if (sc == SDL_SCANCODE_UP) menuSel = (menuSel + kMenuItems - 1) % kMenuItems;
                        else if (sc == SDL_SCANCODE_DOWN) menuSel = (menuSel + 1) % kMenuItems;
                        else if (sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT || sc == SDL_SCANCODE_RETURN ||
                                 sc == SDL_SCANCODE_SPACE) {
                            const int dir = sc == SDL_SCANCODE_LEFT ? -1 : sc == SDL_SCANCODE_RIGHT ? 1 : 0;
                            if (menuSel == 0) gUseKeyboard = !gUseKeyboard;
                            else if (menuSel == 1 && dir) gScaleSet = shownScale + 0.05f * float(dir);
                            else if (menuSel == 2) gTrimDelta += dir ? 5 * dir : -shownTrim;  // Enter: centre
                            else if (menuSel == 3) details = !details;
                            else if (menuSel == 4) SDL_SetWindowFullscreen(window, fullscreen = !fullscreen);
                        }
                    } else if (sc == SDL_SCANCODE_ESCAPE) running = false;
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
            d.controllerPresent = gView.controllerPresent;
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

        {
            const LinkStatus& L = d.link;
            const fpv::net::Telemetry& t = L.tel;
            const bool telFresh = L.haveTelemetry && now - L.telRxUs < 1000000;
            if (L.connected != prevConnected) logf("link: %s", L.connected ? "up" : "DOWN (no valid traffic for 1 s)");
            if (telFresh != prevTelFresh) logf("telemetry: %s", telFresh ? "receiving" : "STOPPED");
            if (d.armed != prevArmed) logf("control: %s", d.armed ? "armed" : "disarmed");
            if (d.padConnected != prevPad)
                logf("input: %s", d.padConnected ? "available" : (d.keyboard ? "window lost focus" : "no controller"));
            prevConnected = L.connected;
            prevTelFresh = telFresh;
            prevArmed = d.armed;
            prevPad = d.padConnected;
            if (telFresh && t.espFlags != prevEsp) {
                logf("esp32: flags%s", espFlagNames(t.espFlags).c_str());
                prevEsp = t.espFlags;
            }
            if (telFresh && cfg.mode == Config::Mode::Udp && t.phoneFlags != prevPhone) {
                logf("phone:%s", phoneFlagNames(t.phoneFlags).c_str());
                prevPhone = t.phoneFlags;
            }
            if (d.armed && now - lastDriveLogUs > 1000000) {
                lastDriveLogUs = now;
                logf("drive: sent thr %u brk %u steer %+d gear %c | esp32 out steer %uus esc %uus usb-rx-err %u | "
                     "rtt %.0fms loss %.1f%% | video %.0fkbps %.0ffps",
                     d.cmd.throttle, d.cmd.brake, d.cmd.steer, d.gearRev ? 'R' : 'F', t.steerUs, t.throttleUs,
                     t.espRxErrors, L.rttMs, L.lossPct, d.video.kbps, d.video.fps);
            }
        }

        // Throttle limit / trim changed (menu, arrows, - / =): save once they settle.
        if (d.scale != shownScale || d.trim != shownTrim) {
            char msg[32];
            if (d.trim != shownTrim) std::snprintf(msg, sizeof msg, "TRIM %+d%s", d.trim, std::abs(d.trim) >= kTrimMax ? " (MAX)" : "");
            else std::snprintf(msg, sizeof msg, "LIMIT %.0f%%", d.scale * 100);
            popup = msg;
            popupUs = now;
            shownScale = d.scale;
            shownTrim = d.trim;
            tuneChangedUs = now;
        }
        d.details = details;
        d.recPc = recorder.active();
        d.recPhoneWanted = gRecordPhone;
        if (now - popupUs < 1500000) d.popup = popup;
        if (tuneChangedUs && now - tuneChangedUs > 800000) {
            tuneChangedUs = 0;
            char v[32];
            if (shownScale != savedScale) {
                std::snprintf(v, sizeof v, "%.2f", shownScale);
                if (saveConfigValue(configPath, "throttle_scale", v)) savedScale = shownScale;
                logf("config: throttle_scale = %s %s", v, savedScale == shownScale ? "saved" : "NOT saved");
            }
            if (shownTrim != savedTrim) {
                std::snprintf(v, sizeof v, "%d", shownTrim);
                if (saveConfigValue(configPath, "steer_trim", v)) savedTrim = shownTrim;
                logf("config: steer_trim = %s %s", v, savedTrim == shownTrim ? "saved" : "NOT saved");
            }
        }
        d.keyboard = gUseKeyboard;
        d.focused = gFocused;
        d.menuOpen = gMenuOpen;
        d.menuSel = menuSel;
        d.fullscreen = fullscreen;

        // While recording, draw into an off-screen copy, show it, and only then read it back:
        // the readback never delays what's on screen.
        const bool rec = recorder.active();
        if (rec) {
            int ow = 0, oh = 0;
            SDL_GetRenderOutputSize(ren.r, &ow, &oh);
            if (!canvas || canvasW != ow || canvasH != oh) {
                if (canvas) SDL_DestroyTexture(canvas);
                canvas = SDL_CreateTexture(ren.r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, ow, oh);
                canvasW = ow;
                canvasH = oh;
            }
            if (canvas) SDL_SetRenderTarget(ren.r, canvas);
        }
        SDL_SetRenderDrawColor(ren.r, 0, 0, 0, 255);
        SDL_RenderClear(ren.r);
        ren.drawVideo();
        drawOsd(ren.r, d);
        if (rec && canvas) {
            SDL_SetRenderTarget(ren.r, nullptr);
            SDL_RenderTexture(ren.r, canvas, nullptr, nullptr);
        }
        SDL_RenderPresent(ren.r);
        if (rec && canvas) {
            SDL_SetRenderTarget(ren.r, canvas);
            if (SDL_Surface* px = SDL_RenderReadPixels(ren.r, nullptr)) {
                const bool bgra = px->format == SDL_PIXELFORMAT_ARGB8888 || px->format == SDL_PIXELFORMAT_XRGB8888;
                SDL_Surface* c = bgra ? px : SDL_ConvertSurface(px, SDL_PIXELFORMAT_ARGB8888);
                if (c) recorder.addFrame(static_cast<const uint8_t*>(c->pixels), c->pitch, c->w, c->h, nowUs());
                if (c && c != px) SDL_DestroySurface(c);
                SDL_DestroySurface(px);
            }
            SDL_SetRenderTarget(ren.r, nullptr);
        }

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
    recorder.stop();
    if (canvas) SDL_DestroyTexture(canvas);
    link->stop();
    video.stop();
    if (ren.tex) SDL_DestroyTexture(ren.tex);
    SDL_DestroyRenderer(ren.r);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
