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
};

void drawOsd(SDL_Renderer* r, const OsdData& d);

}  // namespace pc
