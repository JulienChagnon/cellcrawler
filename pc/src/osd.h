#pragma once
#include <string>

#include "config.h"
#include "link.h"
#include "mapping.h"
#include "video.h"

struct SDL_Renderer;

namespace pc {

struct Latency {
    bool valid = false;
    float totalMs = 0, phoneMs = 0, netMs = 0, pcMs = 0;
};

struct OsdData {
    const Config* cfg = nullptr;
    LinkStatus link;
    VideoStats video;
    Latency latency;

    bool padConnected = false;
    bool armed = false;
    bool gearRev = false;
    float scale = 1;
    int trim = 0;
    float limiter = 1;
    DriveCommand cmd;
    std::string note;  // shown briefly
    uint64_t nowUs = 0;

    bool keyboard = false;           // driving with WASD instead of the controller
    bool focused = false;            // window has keyboard focus
    bool controllerPresent = false;  // an Xbox controller is plugged in (used or not)
    bool menuOpen = false;
    int menuSel = 0;
    bool fullscreen = false;
    bool details = false;   // F3 / menu: the full diagnostic lines
    std::string popup;      // "TRIM +305" while it's being changed
    bool recPc = false;          // this window is being recorded
    bool recPhoneWanted = false; // the phone has been asked to record
};

void drawOsd(SDL_Renderer* r, const OsdData& d);

}  // namespace pc
