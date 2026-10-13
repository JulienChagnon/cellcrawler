// Keyboard driving: WASD and friends turned into a PadState, so the Mapper treats the
// keyboard exactly like the controller (arming, gear, caps, failsafe). Keys are on/off, so
// throttle, brake and steering ramp instead of jumping. Pure logic (tests/test_main.cpp).
//
//   W           throttle (starts at 30%, ramps up; instant off)   S  brake
//   A / D       steer (ramps; recentres faster)    R  toggle gear F <-> R
//   Enter       arm                                Backspace or B  disarm
//   Up / Down   throttle limit +/-10%              Left / Right or - / =  steering trim
//
// The window losing focus counts as the controller being unplugged: if armed, the truck
// brakes (failsafe) and stays disarmed afterwards.
#pragma once
#include <cstdint>

#include "mapping.h"

namespace pc {

enum Key : uint16_t {
    kKeyW = 1 << 0,
    kKeyA = 1 << 1,
    kKeyS = 1 << 2,
    kKeyD = 1 << 3,
    kKeyArm = 1 << 4,
    kKeyDisarm = 1 << 5,
    kKeyGear = 1 << 6,
    kKeyUp = 1 << 7,
    kKeyDown = 1 << 8,
    kKeyLeft = 1 << 9,
    kKeyRight = 1 << 10,
};

class KeyboardPad {
public:
    // W jumps straight to kThrottleStart: below that a brushed motor only whines. Then it
    // ramps to full in kThrottleRampS. Seconds from nothing to full for the rest.
    static constexpr float kThrottleStart = 0.3f;
    static constexpr float kThrottleRampS = 0.3f;
    static constexpr float kBrakeRampS = 0.15f;
    static constexpr float kSteerRampS = 0.2f;
    static constexpr float kSteerCentreS = 0.1f;  // full lock back to centre

    PadState update(bool focused, uint16_t keys, uint64_t nowUs) {
        const float dt = lastUs_ ? float(nowUs - lastUs_) * 1e-6f : 0.0f;
        lastUs_ = nowUs;
        PadState p;
        if (!focused) {
            rt_ = lt_ = lx_ = 0;
            return p;  // connected = false
        }
        p.connected = true;

        // Throttle: ramps up, drops to 0 the moment W is released (or S is pressed).
        if ((keys & kKeyW) && !(keys & kKeyS))
            rt_ = step(rt_ < kThrottleStart ? kThrottleStart : rt_, 1, dt * (1 - kThrottleStart) / kThrottleRampS);
        else rt_ = 0;
        lt_ = (keys & kKeyS) ? step(lt_, 1, dt / kBrakeRampS) : 0;

        float target = 0;
        if (keys & kKeyA) target -= 1;
        if (keys & kKeyD) target += 1;
        const bool toCentre = target == 0 || (target > 0) != (lx_ > 0);
        lx_ = step(lx_, target, dt / (toCentre && lx_ != 0 ? kSteerCentreS : kSteerRampS));

        p.rt = rt_;
        p.lt = lt_;
        p.lx = lx_;
        if (keys & kKeyArm) p.buttons |= kStart;
        if (keys & kKeyDisarm) p.buttons |= kBack;
        if (keys & kKeyGear) p.buttons |= kLB;
        if (keys & kKeyUp) p.buttons |= kDpadUp;
        if (keys & kKeyDown) p.buttons |= kDpadDown;
        if (keys & kKeyLeft) p.buttons |= kDpadLeft;
        if (keys & kKeyRight) p.buttons |= kDpadRight;
        return p;
    }

private:
    // Moves v towards target by at most maxStep.
    static float step(float v, float target, float maxStep) {
        if (v < target) return v + maxStep >= target ? target : v + maxStep;
        if (v > target) return v - maxStep <= target ? target : v - maxStep;
        return v;
    }

    uint64_t lastUs_ = 0;
    float rt_ = 0, lt_ = 0, lx_ = 0;
};

}  // namespace pc
