// Controller -> drive command mapping. Pure logic (unit-tested in tests/test_main.cpp).
//
//   RT          throttle in the current gear
//   LT          brake
//   LB          toggle gear F <-> R (only while RT is released)
//   Left X      steering
//   Menu/Start  arm           View/Back or B  disarm
//   D-pad up/dn throttle scale +/-10%   D-pad l/r  steering trim
#pragma once
#include <cmath>
#include <cstdint>
#include <string>

#include "fpv/serial_proto.h"

namespace pc {

// XInput button bits (same values as XINPUT_GAMEPAD_*).
enum Button : uint16_t {
    kDpadUp = 0x0001,
    kDpadDown = 0x0002,
    kDpadLeft = 0x0004,
    kDpadRight = 0x0008,
    kStart = 0x0010,
    kBack = 0x0020,
    kLB = 0x0100,
    kRB = 0x0200,
    kA = 0x1000,
    kB = 0x2000,
    kX = 0x4000,
    kY = 0x8000,
};

struct PadState {
    bool connected = false;
    float lx = 0;  // -1..1
    float lt = 0;  // 0..1
    float rt = 0;  // 0..1
    uint16_t buttons = 0;
};

struct MappingConfig {
    float steerDeadzone = 0.05f;
    float steerExpo = 0.25f;  // 0 = linear, 1 = fully cubic
    bool steerInvert = false;
    float triggerDeadzone = 0.03f;
    float throttleScale = 1.0f;  // initial live scale (fraction of the firmware cap)
};

struct DriveCommand {
    int16_t steer = 0;
    uint16_t throttle = 0;
    uint16_t brake = 0;
    uint8_t flags = 0;  // fpv::ser::ControlFlags

    bool operator==(const DriveCommand& o) const {
        return steer == o.steer && throttle == o.throttle && brake == o.brake && flags == o.flags;
    }
    bool operator!=(const DriveCommand& o) const { return !(*this == o); }
};

class Mapper {
public:
    explicit Mapper(const MappingConfig& c) : cfg_(c), scale_(c.throttleScale) {}

    // limiter: extra 0..1 throttle factor from link quality (1 = no limit).
    DriveCommand update(const PadState& pad, float limiter = 1.0f) {
        DriveCommand cmd;
        if (!pad.connected) {
            if (armed_) note("CONTROLLER LOST - DISARMED");
            armed_ = false;
            prev_ = 0;
            return cmd;
        }
        const uint16_t pressed = pad.buttons & ~prev_;
        prev_ = pad.buttons;

        const float rt = trigger(pad.rt);
        const float lt = trigger(pad.lt);

        if (pressed & (kBack | kB)) {
            if (armed_) note("DISARMED");
            armed_ = false;
        } else if (pressed & kStart) {
            if (!armed_) note("ARMED");
            armed_ = true;
        }
        if (pressed & kLB) {
            if (rt > 0) note("RELEASE THROTTLE TO SHIFT");
            else {
                gearRev_ = !gearRev_;
                note(gearRev_ ? "GEAR: REVERSE" : "GEAR: FORWARD");
            }
        }
        if (pressed & kDpadUp) scale_ = std::fmin(1.0f, scale_ + 0.1f);
        if (pressed & kDpadDown) scale_ = std::fmax(0.1f, scale_ - 0.1f);
        if (pressed & kDpadRight) trim_ = trim_ + 10 > 200 ? 200 : trim_ + 10;
        if (pressed & kDpadLeft) trim_ = trim_ - 10 < -200 ? -200 : trim_ - 10;

        float s = pad.lx;
        float a = std::fabs(s);
        a = a <= cfg_.steerDeadzone ? 0.0f : (a - cfg_.steerDeadzone) / (1.0f - cfg_.steerDeadzone);
        a = (1.0f - cfg_.steerExpo) * a + cfg_.steerExpo * a * a * a;
        s = std::copysign(a, s);
        if (cfg_.steerInvert) s = -s;
        int steer = int(std::lround(s * 1000.0f)) + trim_;
        cmd.steer = int16_t(steer < -1000 ? -1000 : steer > 1000 ? 1000 : steer);

        if (armed_) {
            float lim = limiter < 0 ? 0 : limiter > 1 ? 1 : limiter;
            cmd.throttle = uint16_t(std::lround(rt * scale_ * lim * 1000.0f));
            cmd.brake = uint16_t(std::lround(lt * 1000.0f));
            cmd.flags |= fpv::ser::kArmed;
        }
        if (gearRev_) cmd.flags |= fpv::ser::kGearReverse;
        return cmd;
    }

    bool armed() const { return armed_; }
    bool gearReverse() const { return gearRev_; }
    float scale() const { return scale_; }
    int trim() const { return trim_; }

    // Latest one-shot notification for the OSD (cleared when read).
    std::string takeNote() {
        std::string n;
        n.swap(note_);
        return n;
    }

private:
    float trigger(float v) const {
        if (v <= cfg_.triggerDeadzone) return 0.0f;
        return std::fmin(1.0f, (v - cfg_.triggerDeadzone) / (1.0f - cfg_.triggerDeadzone));
    }
    void note(const char* n) { note_ = n; }

    MappingConfig cfg_;
    float scale_;
    int trim_ = 0;
    bool armed_ = false;
    bool gearRev_ = false;
    uint16_t prev_ = 0;
    std::string note_;
};

}  // namespace pc
