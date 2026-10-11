// Pure drive logic: turns control commands into servo/ESC pulse widths. No Arduino
// dependencies, so it is unit-tested on the PC (tests/test_drive_logic.cpp).
//
// Safety rules enforced here, regardless of what the host sends:
//   * No valid command for failsafeMs, or a command with kFailsafe (the phone or PC lost its
//     own input) -> centred steering and, if the truck was armed, failsafeBrake for as
//     long as it lasts (neutral in FwdRev mode, which has no brake). Never armed since
//     boot -> neutral, so the ESC still arms at power-up.
//   * Not armed                        -> neutral throttle.
//   * After boot, disarm, or a failsafe longer than relatchMs
//                                      -> throttle ignored until the host sends throttle 0.
//     (Short blips, e.g. LTE jitter, resume without re-latching.)
//   * Gear only changes while throttle input is 0.
//   * Throttle is scaled by hard caps (fwdCap / revCap / brakeCap).
//
// ESC handling (EscMode::FwdBrakeRev, the usual RC car mode): the ESC treats a
// below-neutral pulse as BRAKE, unless it previously saw brake -> neutral, in which case
// below-neutral means REVERSE. We therefore track what the ESC has seen:
//   * mightReverse_: pessimistic. Set as soon as any below-neutral output is followed by
//     neutral. If set when the driver brakes, we first emit a short forward "guard" pulse
//     so the ESC drops out of reverse-armed state; otherwise LT tapped twice at a standstill
//     would drive the truck backwards.
//   * esc_: optimistic-but-confirmed model, a pulse class only counts once it has been held
//     for commitMs (several PWM periods). Used to sequence brake -> neutral -> reverse when
//     the driver wants to reverse.
#pragma once
#include <cstdint>

#include "fpv/serial_proto.h"

namespace drive {

enum class EscMode : uint8_t {
    FwdBrakeRev,  // below neutral = brake, then reverse after brake->neutral->below
    FwdRev,       // below neutral = reverse immediately (crawler style), no ESC brake
};

struct Config {
    // Steering servo
    uint16_t steerCenterUs = 1500;
    uint16_t steerRangeUs = 400;  // +/- from centre at full lock
    bool steerInvert = false;

    // ESC
    EscMode escMode = EscMode::FwdBrakeRev;
    uint16_t escNeutralUs = 1500;
    uint16_t escFwdRangeUs = 500;  // neutral + range = full forward
    uint16_t escRevRangeUs = 500;  // neutral - range = full brake / reverse
    bool escInvert = false;        // set if the ESC drives forward on pulses < neutral
    uint16_t escDeadbandUs = 25;   // pulses within this of neutral are neutral to the ESC

    // Hard caps, permille of the full range. The host's throttle 0..1000 maps onto 0..cap.
    uint16_t fwdCap = 400;
    uint16_t revCap = 250;
    uint16_t brakeCap = 1000;

    uint16_t revArmBrake = 300;  // brake strength used for the automatic reverse-arm pulse
    uint16_t guardUs = 40;       // forward guard pulse beyond neutral (must exceed the ESC deadband)
    bool guardEnabled = true;    // disable if the ESC has reverse turned off
    uint16_t commitMs = 60;      // pulse must be held this long before the ESC is assumed to have seen it
    uint16_t revArmHoldMs = 100; // reverse sequence: brake and neutral each held this much longer

    uint16_t failsafeMs = 150;
    uint16_t failsafeBrake = 500;  // brake on link loss, permille of full brake. 0 = neutral
    uint16_t relatchMs = 1000;  // outage length after which throttle must be released again
};

enum class EscState : uint8_t { IdleFwd, Braking, IdleArmed, Reversing };

struct Output {
    uint16_t steerUs;
    uint16_t throttleUs;
    uint8_t flags;  // fpv::ser::TelemetryFlags (without kTelLowBattery)
};

class DriveLogic {
public:
    explicit DriveLogic(const Config& cfg) : cfg_(cfg) {}

    void onCommand(uint32_t nowMs, const fpv::ser::Control& c) {
        lastSeq_ = c.seq;
        if (c.flags & fpv::ser::kFailsafe) {
            // Not a command: the last real one stays in cmd_, and its age keeps counting
            // towards relatchMs.
            hostLost_ = true;
            return;
        }
        if (haveCmd_ && nowMs - lastCmdMs_ >= cfg_.relatchMs) needNeutral_ = true;
        cmd_ = c;
        lastCmdMs_ = nowMs;
        haveCmd_ = true;
        hostLost_ = false;
    }

    uint16_t lastSeq() const { return lastSeq_; }
    EscState escState() const { return esc_; }
    bool mightReverse() const { return mightReverse_; }

    Output update(uint32_t nowMs) {
        using namespace fpv::ser;
        const bool fresh = haveCmd_ && !hostLost_ && (nowMs - lastCmdMs_) <= cfg_.failsafeMs;
        const bool armed = fresh && (cmd_.flags & kArmed);
        // Lost while armed (the last real command was armed): brake.
        const bool failsafeBrake = haveCmd_ && !fresh && (cmd_.flags & kArmed) && cfg_.failsafeBrake > 0 &&
                                   cfg_.escMode == EscMode::FwdBrakeRev;
        if (fresh && !armed) needNeutral_ = true;
        if (haveCmd_ && nowMs - lastCmdMs_ >= cfg_.relatchMs) needNeutral_ = true;
        if (armed && needNeutral_ && cmd_.throttle == 0) needNeutral_ = false;

        const uint16_t throttleIn = (armed && !needNeutral_) ? clamp1000(cmd_.throttle) : 0;
        const uint16_t brakeIn = armed ? clamp1000(cmd_.brake) : 0;

        const bool wantRev = (cmd_.flags & kGearReverse) != 0;
        if (armed && wantRev != gearRev_ && cmd_.throttle == 0) gearRev_ = wantRev;

        Output out;
        out.steerUs = steerPulse(fresh ? cmd_.steer : 0);
        out.throttleUs = throttlePulse(nowMs, throttleIn, brakeIn, failsafeBrake);
        out.flags = 0;
        if (!fresh) out.flags |= kTelFailsafe;
        if (armed) out.flags |= kTelArmed;
        if (gearRev_) out.flags |= kTelGearReverse;
        if (esc_ == EscState::Reversing || esc_ == EscState::IdleArmed) out.flags |= kTelReverseEngaged;
        if (needNeutral_) out.flags |= kTelNeedNeutral;
        return out;
    }

private:
    enum class Pulse : uint8_t { Neutral, Fwd, Below };

    static uint16_t clamp1000(uint16_t v) { return v > 1000 ? 1000 : v; }

    uint16_t steerPulse(int16_t steer) const {
        int32_t s = steer < -1000 ? -1000 : steer > 1000 ? 1000 : steer;
        if (cfg_.steerInvert) s = -s;
        return static_cast<uint16_t>(cfg_.steerCenterUs + s * cfg_.steerRangeUs / 1000);
    }

    // signedUs > 0 = forward, < 0 = below neutral (brake/reverse).
    uint16_t escPulse(int32_t signedUs) const {
        if (cfg_.escInvert) signedUs = -signedUs;
        return static_cast<uint16_t>(cfg_.escNeutralUs + signedUs);
    }
    int32_t fwdUs(uint16_t v, uint16_t cap) const { return int32_t(v) * cap / 1000 * cfg_.escFwdRangeUs / 1000; }
    int32_t belowUs(uint16_t v, uint16_t cap) const { return -(int32_t(v) * cap / 1000 * cfg_.escRevRangeUs / 1000); }

    uint16_t throttlePulse(uint32_t nowMs, uint16_t throttle, uint16_t brake, bool failsafeBrake) {
        int32_t us = 0;
        if (failsafeBrake) {
            // Whatever the gear: if the ESC might be reverse-armed (or is reversing), the
            // guard pulse first, so the brake pulse is a brake and not reverse.
            if (cfg_.guardEnabled && mightReverse_) us = cfg_.guardUs;
            else us = belowUs(cfg_.failsafeBrake, 1000);
        } else if (cfg_.escMode == EscMode::FwdRev) {
            // No ESC brake: LT coasts, reverse is immediate.
            if (brake == 0 && throttle > 0) us = gearRev_ ? belowUs(throttle, cfg_.revCap) : fwdUs(throttle, cfg_.fwdCap);
        } else if (!gearRev_) {
            if (brake > 0) {
                if (cfg_.guardEnabled && mightReverse_) us = cfg_.guardUs;  // until the ESC has seen forward
                else us = belowUs(brake, cfg_.brakeCap);
            } else if (throttle > 0) {
                us = fwdUs(throttle, cfg_.fwdCap);
            }
        } else if (throttle > 0 && brake == 0) {
            // Reverse gear. LT coasts (the ESC has no brake while reversing).
            switch (esc_) {
                // brake -> neutral -> reverse, each step held revArmHoldMs past the point the
                // ESC has surely seen it: some ESCs want a firm brake and a clear neutral.
                case EscState::IdleFwd: us = belowUs(cfg_.revArmBrake, 1000); break;  // brake first
                case EscState::Braking:
                    us = nowMs - escSinceMs_ < cfg_.revArmHoldMs ? belowUs(cfg_.revArmBrake, 1000) : 0;
                    break;
                case EscState::IdleArmed:
                    // Straight after the arming brake: hold neutral a little longer. (After
                    // reversing and letting go, reverse again at once.)
                    us = escFrom_ == EscState::Braking && nowMs - escSinceMs_ < cfg_.revArmHoldMs
                             ? 0
                             : belowUs(throttle, cfg_.revCap);
                    break;
                case EscState::Reversing: us = belowUs(throttle, cfg_.revCap); break;
            }
        }
        trackEsc(nowMs, classify(us));
        return escPulse(us);
    }

    Pulse classify(int32_t us) const {
        if (us > cfg_.escDeadbandUs) return Pulse::Fwd;
        if (us < -int32_t(cfg_.escDeadbandUs)) return Pulse::Below;
        return Pulse::Neutral;
    }

    void trackEsc(uint32_t nowMs, Pulse p) {
        // Pessimistic reverse-armed tracking: any below->neutral may have been seen.
        if (p == Pulse::Below) sawBelow_ = true;
        if (p == Pulse::Neutral && sawBelow_) mightReverse_ = true;

        if (p != pending_) {
            pending_ = p;
            pendingSinceMs_ = nowMs;
        }
        if (nowMs - pendingSinceMs_ < cfg_.commitMs) return;

        // Confirmed: the ESC has seen this pulse class for several frames.
        const EscState before = esc_;
        switch (p) {
            case Pulse::Fwd:
                esc_ = EscState::IdleFwd;
                sawBelow_ = false;
                mightReverse_ = false;
                break;
            case Pulse::Below:
                if (esc_ == EscState::IdleFwd) esc_ = EscState::Braking;
                else if (esc_ == EscState::IdleArmed) esc_ = EscState::Reversing;
                break;
            case Pulse::Neutral:
                if (esc_ == EscState::Braking || esc_ == EscState::Reversing) esc_ = EscState::IdleArmed;
                break;
        }
        if (esc_ != before) {
            escFrom_ = before;
            escSinceMs_ = nowMs;
        }
    }

    Config cfg_;
    fpv::ser::Control cmd_{};
    uint32_t lastCmdMs_ = 0;
    bool haveCmd_ = false;
    bool hostLost_ = false;  // a kFailsafe frame came after the last real command
    uint16_t lastSeq_ = 0;
    bool needNeutral_ = true;
    bool gearRev_ = false;

    EscState esc_ = EscState::IdleFwd;
    EscState escFrom_ = EscState::IdleFwd;  // state before the last change
    uint32_t escSinceMs_ = 0;               // when esc_ last changed
    Pulse pending_ = Pulse::Neutral;
    uint32_t pendingSinceMs_ = 0;
    bool sawBelow_ = false;
    bool mightReverse_ = false;
};

}  // namespace drive
