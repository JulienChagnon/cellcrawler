// On-screen display drawn over the video with SDL's built-in 8x8 debug font (no font files).
#include "osd.h"

#include <SDL3/SDL.h>

#include <cstdarg>
#include <cstdio>

#include "fpv/battery.h"
#include "fpv/serial_proto.h"

namespace pc {

using namespace fpv;

namespace {

struct Color {
    uint8_t r, g, b;
};
constexpr Color kWhite{235, 235, 235}, kGreen{80, 230, 100}, kYellow{250, 210, 60}, kRed{255, 70, 60},
    kGrey{160, 160, 160};
constexpr float kChar = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;

enum class Align { Left, Center, Right };

// Draws text with a translucent backing box. x/y in pixels; scale multiplies the 8 px font.
float text(SDL_Renderer* r, float x, float y, float scale, Color c, Align a, const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    float w = float(SDL_strlen(buf)) * kChar * scale;
    if (a == Align::Center) x -= w / 2;
    else if (a == Align::Right) x -= w;
    SDL_FRect bg{x - 3 * scale, y - 2 * scale, w + 6 * scale, kChar * scale + 4 * scale};
    SDL_SetRenderDrawColor(r, 0, 0, 0, 140);
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderScale(r, scale, scale);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    SDL_RenderDebugText(r, x / scale, y / scale, buf);
    SDL_SetRenderScale(r, 1, 1);
    return y + (kChar + 5) * scale;
}

void bar(SDL_Renderer* r, float x, float y, float w, float h, float frac, Color c) {
    SDL_FRect bg{x, y, w, h};
    SDL_SetRenderDrawColor(r, 0, 0, 0, 140);
    SDL_RenderFillRect(r, &bg);
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    SDL_FRect fg{x, y + h * (1 - frac), w, h * frac};
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 220);
    SDL_RenderFillRect(r, &fg);
}

}  // namespace

void drawOsd(SDL_Renderer* r, const OsdData& d) {
    int wi = 0, hi = 0;
    SDL_GetRenderOutputSize(r, &wi, &hi);
    const float W = float(wi), H = float(hi);
    const float s = H >= 1000 ? 3.0f : 2.0f;  // text scale
    const float pad = 10;
    const bool blink = (d.nowUs / 400000) % 2 == 0;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    const LinkStatus& L = d.link;
    const net::Telemetry& t = L.tel;
    const bool udp = d.cfg->mode == Config::Mode::Udp;
    const bool telFresh = L.haveTelemetry && d.nowUs - L.telRxUs < 1000000;

    // ---- Top left: battery, link, video --------------------------------------------
    float y = pad;
    if (telFresh && t.battMv > 1000) {
        uint8_t cells = d.cfg->cells ? d.cfg->cells : t.cells;
        int pct = cells ? battery::percent(t.battMv, cells, d.cfg->chemistry) : -1;
        bool low = (t.espFlags & ser::kTelLowBattery) != 0;
        Color c = low || (pct >= 0 && pct < 15) ? kRed : pct >= 0 && pct < 30 ? kYellow : kGreen;
        if (pct >= 0) y = text(r, pad, y, s, c, Align::Left, "BAT %5.2fV %uS %3d%%", t.battMv / 1000.0, cells, pct);
        else y = text(r, pad, y, s, c, Align::Left, "BAT %5.2fV", t.battMv / 1000.0);
    } else {
        y = text(r, pad, y, s, kGrey, Align::Left, "BAT --");
    }

    if (L.rttMs >= 0) {
        Color c = L.rttMs > d.cfg->rttLimitMs ? kRed : L.rttMs > d.cfg->rttLimitMs / 2 ? kYellow : kWhite;
        if (udp) y = text(r, pad, y, s, c, Align::Left, "RTT %3.0fms (min %3.0f)  LOSS %.1f%%", L.rttMs, L.rttMinMs, L.lossPct);
        else y = text(r, pad, y, s, c, Align::Left, "SERIAL RTT %.1fms", L.rttMs);
    }
    if (udp) {
        const VideoStats& v = d.video;
        if (v.haveStream)
            y = text(r, pad, y, s, kWhite, Align::Left, "VID %2.0ffps %5.0fkbps dec %.1fms %s  Q %ums", v.fps, v.kbps,
                     v.decodeMs, v.hw ? "HW" : "SW", v.queueDelayMs);
        if (d.latency.valid)
            y = text(r, pad, y, s, kWhite, Align::Left, "LAT ~%3.0fms = cam+enc %2.0f + net %3.0f + pc %2.0f",
                     d.latency.totalMs, d.latency.phoneMs, d.latency.netMs, d.latency.pcMs);
        if (telFresh && t.espRttUs)
            y = text(r, pad, y, s, kGrey, Align::Left, "ESP32 USB %.1fms  err %u", t.espRttUs / 1000.0, t.espRxErrors);
    } else if (telFresh) {
        y = text(r, pad, y, s, kGrey, Align::Left, "PWM steer %u  esc %u  err %u", t.steerUs, t.throttleUs, t.espRxErrors);
    }

    // ---- Top right: phone ----------------------------------------------------------
    if (udp) {
        float yr = pad;
        if (telFresh) {
            static const char* kNet[] = {"?", "WIFI", "LTE", "5G", "CELL"};
            const char* nt = t.netType < 5 ? kNet[t.netType] : "?";
            Color sc = t.rsrpDbm == 0 ? kGrey : t.rsrpDbm > -95 ? kGreen : t.rsrpDbm > -110 ? kYellow : kRed;
            if (t.rsrpDbm) yr = text(r, W - pad, yr, s, sc, Align::Right, "%s %ddBm", nt, t.rsrpDbm);
            else yr = text(r, W - pad, yr, s, sc, Align::Right, "%s", nt);
            Color pc = t.phoneBattPct < 20 ? kRed : kWhite;
            yr = text(r, W - pad, yr, s, t.phoneTempDeciC > 450 ? kRed : pc, Align::Right, "PHONE %u%% %.1fC%s",
                      t.phoneBattPct, t.phoneTempDeciC / 10.0, (t.phoneFlags & net::kPhoneCharging) ? " CHG" : "");
        }
        if (!L.peer.empty()) text(r, W - pad, yr, s * 0.75f, kGrey, Align::Right, "%s", L.peer.c_str());
    }

    // ---- Bottom: gear, arm state, inputs ---------------------------------------------
    const float big = s * 3;
    const float by = H - pad - kChar * big - 4 * s;
    text(r, W / 2, by, big, d.gearRev ? kYellow : kWhite, Align::Center, "%s", d.gearRev ? "R" : "F");
    const float infoY = by - (kChar + 6) * s * 2;
    if (d.armed) text(r, W / 2, infoY, s, kGreen, Align::Center, "ARMED  CAP %3.0f%%  TRIM %+d", d.scale * 100, d.trim);
    else text(r, W / 2, infoY, s, kRed, Align::Center, "DISARMED - press MENU to arm");

    const float bw = 14 * s / 2, bh = 60 * s / 2;
    bar(r, W / 2 + kChar * big + 10, H - pad - bh, bw, bh, d.cmd.throttle / 1000.0f, kGreen);
    bar(r, W / 2 - kChar * big - 10 - bw, H - pad - bh, bw, bh, d.cmd.brake / 1000.0f, kRed);
    // steering indicator
    const float sw = 160 * s / 2, sy = H - pad - 6;
    SDL_FRect track{W / 2 - sw / 2, sy, sw, 4};
    SDL_SetRenderDrawColor(r, 0, 0, 0, 140);
    SDL_RenderFillRect(r, &track);
    SDL_FRect knob{W / 2 + d.cmd.steer / 1000.0f * sw / 2 - 3, sy - 3, 6, 10};
    SDL_SetRenderDrawColor(r, 235, 235, 235, 230);
    SDL_RenderFillRect(r, &knob);

    // ---- Centre: the single most important warning ------------------------------------
    const char* warn = nullptr;
    Color wc = kRed;
    if (!d.padConnected) warn = "NO CONTROLLER";
    else if (!L.connected) warn = udp ? (L.peer.empty() ? "WAITING FOR PHONE" : "LINK LOST") : "NO ESP32 DATA";
    else if (udp && (t.phoneFlags & net::kPhoneFailsafe) && d.armed) warn = "PHONE FAILSAFE";
    else if (udp && telFresh && !(t.phoneFlags & net::kPhoneUsbOk)) warn = "ESP32 USB DISCONNECTED";
    else if (udp && telFresh && !(t.phoneFlags & net::kPhoneEspOk)) warn = "ESP32 NOT RESPONDING";
    else if (telFresh && (t.espFlags & ser::kTelFailsafe) && d.armed) warn = "ESP32 FAILSAFE";
    else if (telFresh && (t.espFlags & ser::kTelNeedNeutral) && d.armed) { warn = "RELEASE THROTTLE"; wc = kYellow; }
    else if (d.limiter < 1) { warn = "HIGH LATENCY - THROTTLE LIMITED"; wc = kYellow; }
    else if (telFresh && (t.espFlags & ser::kTelLowBattery) && blink) warn = "LOW BATTERY";
    if (warn) text(r, W / 2, H * 0.35f, s * 2, wc, Align::Center, "%s", warn);
    if (!d.note.empty()) text(r, W / 2, H * 0.35f + kChar * s * 3, s * 1.5f, kYellow, Align::Center, "%s", d.note.c_str());
}

}  // namespace pc
