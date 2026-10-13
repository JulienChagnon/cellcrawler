#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace pc {

namespace {
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
bool toBool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }
}  // namespace

bool loadConfig(const std::string& path, Config& c) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open config '%s' (copy fpv.ini.example to fpv.ini)\n", path.c_str());
        return false;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        size_t cmt = line.find_first_of(";#");
        if (cmt != std::string::npos) line.resize(cmt);
        line = trim(line);
        if (line.empty() || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "%s:%d: expected key = value\n", path.c_str(), lineNo);
            return false;
        }
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        float f = float(std::atof(v.c_str()));
        int i = std::atoi(v.c_str());

        if (k == "mode") {
            if (v == "udp") c.mode = Config::Mode::Udp;
            else if (v == "serial") c.mode = Config::Mode::Serial;
            else { std::fprintf(stderr, "%s:%d: mode must be udp or serial\n", path.c_str(), lineNo); return false; }
        }
        else if (k == "port") c.port = uint16_t(i);
        else if (k == "passphrase" || k == "key") c.secret = v;
        else if (k == "serial_port") c.serialPort = v;
        else if (k == "steer_deadzone") c.mapping.steerDeadzone = f;
        else if (k == "steer_expo") c.mapping.steerExpo = f;
        else if (k == "steer_invert") c.mapping.steerInvert = toBool(v);
        else if (k == "trigger_deadzone") c.mapping.triggerDeadzone = f;
        else if (k == "throttle_scale") c.mapping.throttleScale = f;
        else if (k == "steer_trim") c.mapping.steerTrim = i;
        else if (k == "rtt_limit_ms") c.rttLimitMs = uint32_t(i);
        else if (k == "hw_decode") c.hwDecode = toBool(v);
        else if (k == "fullscreen") c.fullscreen = toBool(v);
        else if (k == "vsync") c.vsync = toBool(v);
        else if (k == "window_width") c.windowWidth = i;
        else if (k == "window_height") c.windowHeight = i;
        else if (k == "cells") c.cells = uint8_t(i);
        else if (k == "chemistry") c.chemistry = v == "nimh" ? fpv::battery::Chemistry::NiMH : fpv::battery::Chemistry::LiPo;
        else std::fprintf(stderr, "%s:%d: unknown key '%s' ignored\n", path.c_str(), lineNo, k.c_str());
    }

    if (c.mode == Config::Mode::Udp) {
        if (const char* err = fpv::secretToKey(c.secret.c_str(), c.key)) {
            std::fprintf(stderr, "config: %s (set passphrase = ... in %s)\n", err, path.c_str());
            return false;
        }
    }
    return true;
}

bool saveConfigValue(const std::string& path, const std::string& key, const std::string& value) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();
    const std::string text = ss.str();
    const std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";

    std::string out;
    bool found = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string body = line.substr(0, line.find_first_of(";#"));
        size_t eq = body.find('=');
        if (!found && eq != std::string::npos && trim(body.substr(0, eq)) == key) {
            line = key + " = " + value;
            found = true;
        }
        out += line + nl;
    }
    if (!found) out += key + " = " + value + nl;

    const std::string tmp = path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o || !(o << out)) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0 ||
           (std::remove(path.c_str()) == 0 && std::rename(tmp.c_str(), path.c_str()) == 0);
}

}  // namespace pc
