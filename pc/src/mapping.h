// Controller -> drive command mapping. Pure logic (unit-tested in tests/test_main.cpp).
//
//   RT          throttle in the current gear
//   LT          brake
//   LB          toggle gear F <-> R (only while RT is released)
//   Left X      steering
//   Menu/Start  arm           View/Back or B  disarm
//   D-pad up/dn throttle limit +/-10%   D-pad l/r  steering trim
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
    float throttleScale = 0.6f;  // throttle limit, fraction of the firmware cap
    int steerTrim = 0;           // steering units (1000 = full lock), saved in fpv.ini
};

// +/- half of full lock. Trim is added to the steering, so a big trim costs lock on the
// other side; past ~150, re-seat the servo horn instead (docs/tuning.md, "Steering").
constexpr int kTrimMax = 500;
constexpr float kScaleMin = 0.1f;

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
    explicit Mapper(const MappingConfig& c) : cfg_(c) {
        setScale(c.throttleScale);
        setTrim(c.steerTrim);
    }

    // limiter: extra 0..1 throttle factor from link quality (1 = no limit).
    DriveCommand update(const PadState& pad, float limiter = 1.0f) {
        DriveCommand cmd;
        if (!pad.connected) {
            // Lost while armed: the truck brakes (failsafe) until the controller is back,
            // then stays disarmed until Menu is pressed.
            if (armed_) {
                note("INPUT LOST - BRAKING");
                lostArmed_ = true;
            }
            armed_ = false;
            prev_ = 0;
            if (lostArmed_) cmd.flags = fpv::ser::kFailsafe;
            return cmd;
        }
        lostArmed_ = false;
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
        if (pressed & kDpadUp) setScale(scale_ + 0.1f);
        if (pressed & kDpadDown) setScale(scale_ - 0.1f);
        if (pressed & kDpadRight) setTrim(trim_ + 10);
        if (pressed & kDpadLeft) setTrim(trim_ - 10);

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

    // Switching input source: never carry the arm state over to the other device.
    void disarm(const char* why) {
        if (armed_) note(why);
        armed_ = false;
        lostArmed_ = false;
        prev_ = 0;
    }

    void setScale(float s) {
        s = std::round(s * 100.0f) / 100.0f;  // keep 0.6 + 0.1 at exactly 0.7
        scale_ = s < kScaleMin ? kScaleMin : s > 1.0f ? 1.0f : s;
    }
    void setTrim(int t) { trim_ = t < -kTrimMax ? -kTrimMax : t > kTrimMax ? kTrimMax : t; }

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
    float scale_ = 1.0f;
    int trim_ = 0;
    bool armed_ = false;
    bool lostArmed_ = false;  // controller dropped out while armed
    bool gearRev_ = false;
    uint16_t prev_ = 0;
    std::string note_;
};

}  // namespace pc
