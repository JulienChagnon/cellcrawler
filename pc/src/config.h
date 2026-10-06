#pragma once
#include <cstdint>
#include <string>

#include "fpv/battery.h"
#include "fpv/passphrase.h"
#include "mapping.h"

namespace pc {

struct Config {
    enum class Mode { Udp, Serial } mode = Mode::Udp;
    uint16_t port = 47000;
    std::string secret;  // passphrase or 32-hex key
    fpv::SipKey key;
    std::string serialPort = "COM3";

    MappingConfig mapping;
    uint32_t rttLimitMs = 250;  // above this, throttle is halved

    bool hwDecode = true;
    bool fullscreen = false;
    bool vsync = false;
    int windowWidth = 1280, windowHeight = 720;

    uint8_t cells = 0;  // 0 = use the ESP32's detection
    fpv::battery::Chemistry chemistry = fpv::battery::Chemistry::LiPo;
};

// Loads key=value lines (';' or '#' comments). Returns false and prints why on error.
bool loadConfig(const std::string& path, Config& out);

}  // namespace pc
