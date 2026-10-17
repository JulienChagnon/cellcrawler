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

// Tab menu: input source and fullscreen, plus the key help. Drawn last, over everything.
void drawMenu(SDL_Renderer* r, const OsdData& d, float W, float H, float s) {
    SDL_FRect shade{0, 0, W, H};
    SDL_SetRenderDrawColor(r, 0, 0, 0, 170);
    SDL_RenderFillRect(r, &shade);

    float y = H * 0.18f;
    const float x = W / 2 - 30 * kChar * s / 2;
    y = text(r, W / 2, y, s * 1.5f, kWhite, Align::Center, "MENU");
    y += kChar * s;
    char limit[48], trim[48];
    std::snprintf(limit, sizeof limit, "Throttle limit:  < %.0f%% >", d.scale * 100);
    std::snprintf(trim, sizeof trim, "Steering trim:  < %+d >   (Enter: 0)", d.trim);
    const char* items[] = {d.keyboard ? "Input:  < Keyboard (WASD) >" : "Input:  < Xbox controller >", limit, trim,
                           d.details ? "HUD details (F3):  < On >" : "HUD details (F3):  < Off >",
                           d.fullscreen ? "Fullscreen:  < On >" : "Fullscreen:  < Off >"};
    for (int i = 0; i < 5; ++i)
        y = text(r, x, y, s, i == d.menuSel ? kYellow : kWhite, Align::Left, "%s %s", i == d.menuSel ? ">" : " ",
                 items[i]);
    y = text(r, x, y, s, d.controllerPresent ? kGreen : kGrey, Align::Left, "  Controller %s",
             d.controllerPresent ? "connected" : "not connected");
    y += kChar * s;

    if (d.keyboard) {
        static const char* kHelp[] = {
            "W  throttle        S  brake",
            "A / D  steer       R  gear F/R",
            "Enter  arm         Backspace / B  disarm",
            "Up / Down  throttle limit +/-10%      F  record",
            "- / =  steering trim (fine)   Left / Right  (coarse)",
            "Clicking away from this window brakes the truck",
        };
        for (const char* h : kHelp) y = text(r, x, y, s, kGrey, Align::Left, "%s", h);
    } else {
        static const char* kHelp[] = {
            "RT  throttle       LT  brake",
            "Left stick  steer  LB  gear F/R",
            "Menu  arm          View / B  disarm",
            "D-pad  throttle limit and trim   - / =  fine trim",
            "F (keyboard)  record PC window + phone",
        };
        for (const char* h : kHelp) y = text(r, x, y, s, kGrey, Align::Left, "%s", h);
    }
    y = text(r, x, y, s, kGrey, Align::Left, "Limit and trim are saved to fpv.ini");
    y += kChar * s;
    text(r, W / 2, y, s, kWhite, Align::Center, "Up/Down select   Left/Right/Enter change   Tab or Esc close");
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

    // ---- Top left: latency (and battery, if monitored) --------------------------------
    float y = pad;
    if (udp) {
        if (d.latency.valid) {
            const float ms = d.latency.totalMs;
            y = text(r, pad, y, s, ms > 250 ? kRed : ms > 150 ? kYellow : kWhite, Align::Left, "LAT ~%.0fms", ms);
        } else if (L.connected) {
            y = text(r, pad, y, s, kGrey, Align::Left, "LAT --");
        }
        if (L.lossPct >= 2) y = text(r, pad, y, s, L.lossPct >= 10 ? kRed : kYellow, Align::Left, "LOSS %.0f%%", L.lossPct);
    }
    if (d.cfg->batteryMonitor && telFresh && t.battMv > 1000) {
        uint8_t cells = d.cfg->cells ? d.cfg->cells : t.cells;
        int pct = cells ? battery::percent(t.battMv, cells, d.cfg->chemistry) : -1;
        bool low = (t.espFlags & ser::kTelLowBattery) != 0;
        Color c = low || (pct >= 0 && pct < 15) ? kRed : pct >= 0 && pct < 30 ? kYellow : kWhite;
        if (pct >= 0) y = text(r, pad, y, s, c, Align::Left, "BAT %.1fV %d%%", t.battMv / 1000.0, pct);
        else y = text(r, pad, y, s, c, Align::Left, "BAT %.1fV", t.battMv / 1000.0);
    }

    // F3 / menu: everything else, for diagnosing.
    if (d.details) {
        const float ds = s * 0.75f;
        if (L.rttMs >= 0) {
            if (udp) y = text(r, pad, y, ds, kGrey, Align::Left, "RTT %.0fms (min %.0f)  LOSS %.1f%%", L.rttMs, L.rttMinMs, L.lossPct);
            else y = text(r, pad, y, ds, kGrey, Align::Left, "SERIAL RTT %.1fms", L.rttMs);
        }
        if (udp && d.video.haveStream) {
            const VideoStats& v = d.video;
            y = text(r, pad, y, ds, kGrey, Align::Left, "VID %.0ffps %.0fkbps dec %.1fms %s  Q %ums", v.fps, v.kbps,
                     v.decodeMs, v.hw ? "HW" : "SW", v.queueDelayMs);
        }
        if (udp && d.latency.valid)
            y = text(r, pad, y, ds, kGrey, Align::Left, "LAT = cam+enc %.0f + net %.0f + pc %.0f", d.latency.phoneMs,
                     d.latency.netMs, d.latency.pcMs);
        if (telFresh)
            y = text(r, pad, y, ds, kGrey, Align::Left, "ESP32 PWM steer %u esc %u  usb %.1fms err %u", t.steerUs,
                     t.throttleUs, t.espRttUs / 1000.0, t.espRxErrors);
        if (udp && telFresh)
            y = text(r, pad, y, ds, kGrey, Align::Left, "PHONE %u%% %.1fC%s  %s", t.phoneBattPct, t.phoneTempDeciC / 10.0,
                     (t.phoneFlags & net::kPhoneCharging) ? " CHG" : "", L.peer.c_str());
        y = text(r, pad, y, ds, kGrey, Align::Left, "LIMIT %.0f%%  TRIM %+d", d.scale * 100, d.trim);
    }

    // ---- Top right: network and signal; phone only when it needs attention -------------
    if (udp && telFresh) {
        float yr = pad;
        static const char* kNet[] = {"?", "WIFI", "LTE", "5G", "CELL"};
        const char* nt = t.netType < 5 ? kNet[t.netType] : "?";
        Color sc = t.rsrpDbm == 0 ? kWhite : t.rsrpDbm > -95 ? kGreen : t.rsrpDbm > -110 ? kYellow : kRed;
        if (t.rsrpDbm) yr = text(r, W - pad, yr, s, sc, Align::Right, "%s %d", nt, t.rsrpDbm);
        else yr = text(r, W - pad, yr, s, sc, Align::Right, "%s", nt);
        if (t.phoneTempDeciC > 450) yr = text(r, W - pad, yr, s, kRed, Align::Right, "PHONE HOT %.0fC", t.phoneTempDeciC / 10.0);
        if (t.phoneBattPct < 20 && !(t.phoneFlags & net::kPhoneCharging))
            text(r, W - pad, yr, s, kRed, Align::Right, "PHONE %u%%", t.phoneBattPct);
    }

    // ---- Top centre: recording ---------------------------------------------------------
    if (d.recPc || d.recPhoneWanted) {
        const bool phoneRec = udp && telFresh && (t.phoneFlags & net::kPhoneRecording);
        const char* what = !udp ? "REC" : phoneRec ? (d.recPc ? "REC PC+PHONE" : "REC PHONE")
                                                   : (d.recPc ? "REC PC  (phone...)" : "REC (phone...)");
        const float tw = float(SDL_strlen(what)) * kChar * s;
        if (blink) {
            SDL_FRect dot{W / 2 - tw / 2 - 4 * s - kChar * s, pad + 1, kChar * s, kChar * s};
            SDL_SetRenderDrawColor(r, kRed.r, kRed.g, kRed.b, 255);
            SDL_RenderFillRect(r, &dot);
        }
        text(r, W / 2, pad, s, phoneRec || !udp ? kRed : kYellow, Align::Center, "%s", what);
    }

    // ---- Bottom: gear, inputs; a hint only while disarmed ------------------------------
    const float big = s * 3;
    const float by = H - pad - kChar * big - 4 * s;
    text(r, W / 2, by, big, d.gearRev ? kYellow : d.armed ? kGreen : kGrey, Align::Center, "%s", d.gearRev ? "R" : "F");
    const float infoY = by - (kChar + 6) * s * 2;
    if (!d.popup.empty()) text(r, W / 2, infoY, s * 1.5f, kYellow, Align::Center, "%s", d.popup.c_str());
    else if (!d.armed) text(r, W / 2, infoY, s, kRed, Align::Center, "DISARMED - press %s to arm", d.keyboard ? "ENTER" : "MENU");

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
    if (!d.padConnected && d.keyboard) warn = "CLICK THE WINDOW TO DRIVE";
    else if (!d.padConnected) warn = "NO CONTROLLER - TAB FOR KEYBOARD";
    else if (!L.connected && udp && L.lastBadUs && d.nowUs - L.lastBadUs < 2000000)
        warn = "PHONE PASSPHRASE DOESN'T MATCH FPV.INI";
    else if (!L.connected) warn = udp ? (L.peer.empty() ? "WAITING FOR PHONE" : "LINK LOST") : "NO ESP32 DATA";
    else if (udp && (t.phoneFlags & net::kPhoneFailsafe) && d.armed) warn = "PHONE FAILSAFE";
    else if (udp && telFresh && !(t.phoneFlags & net::kPhoneUsbOk)) warn = "ESP32 USB DISCONNECTED";
    else if (udp && telFresh && !(t.phoneFlags & net::kPhoneEspOk)) warn = "ESP32 NOT RESPONDING";
    else if (telFresh && (t.espFlags & ser::kTelFailsafe) && d.armed) warn = "ESP32 FAILSAFE";
    else if (telFresh && (t.espFlags & ser::kTelNeedNeutral) && d.armed) { warn = "RELEASE THROTTLE"; wc = kYellow; }
    else if (d.limiter < 1) { warn = "HIGH LATENCY - THROTTLE LIMITED"; wc = kYellow; }
    else if (d.cfg->batteryMonitor && telFresh && (t.espFlags & ser::kTelLowBattery) && blink) warn = "LOW BATTERY";
    if (warn) text(r, W / 2, H * 0.35f, s * 2, wc, Align::Center, "%s", warn);
    if (!d.note.empty()) text(r, W / 2, H * 0.35f + kChar * s * 3, s * 1.5f, kYellow, Align::Center, "%s", d.note.c_str());

    if (d.menuOpen) drawMenu(r, d, W, H, s);
}

}  // namespace pc
