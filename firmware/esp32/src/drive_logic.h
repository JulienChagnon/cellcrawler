// Pure drive logic: turns control commands into servo/ESC pulse widths. No Arduino
// dependencies, so it is unit-tested on the PC (tests/test_drive_logic.cpp).
//
// Safety rules enforced here, regardless of what the host sends:
//   * No valid command for failsafeMs  -> neutral throttle, centred steering.
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

    uint16_t failsafeMs = 150;
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
        if (haveCmd_ && nowMs - lastCmdMs_ >= cfg_.relatchMs) needNeutral_ = true;
        cmd_ = c;
        lastCmdMs_ = nowMs;
        haveCmd_ = true;
    }

    uint16_t lastSeq() const { return cmd_.seq; }
    EscState escState() const { return esc_; }
    bool mightReverse() const { return mightReverse_; }

    Output update(uint32_t nowMs) {
        using namespace fpv::ser;
        const bool fresh = haveCmd_ && (nowMs - lastCmdMs_) <= cfg_.failsafeMs;
        const bool armed = fresh && (cmd_.flags & kArmed);
        if (fresh && !armed) needNeutral_ = true;
        if (haveCmd_ && nowMs - lastCmdMs_ >= cfg_.relatchMs) needNeutral_ = true;
        if (armed && needNeutral_ && cmd_.throttle == 0) needNeutral_ = false;

        const uint16_t throttleIn = (armed && !needNeutral_) ? clamp1000(cmd_.throttle) : 0;
        const uint16_t brakeIn = armed ? clamp1000(cmd_.brake) : 0;

        const bool wantRev = (cmd_.flags & kGearReverse) != 0;
        if (armed && wantRev != gearRev_ && cmd_.throttle == 0) gearRev_ = wantRev;

        Output out;
        out.steerUs = steerPulse(fresh ? cmd_.steer : 0);
        out.throttleUs = throttlePulse(nowMs, throttleIn, brakeIn);
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

    uint16_t throttlePulse(uint32_t nowMs, uint16_t throttle, uint16_t brake) {
        int32_t us = 0;
        if (cfg_.escMode == EscMode::FwdRev) {
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
                case EscState::IdleArmed:
                case EscState::Reversing: us = belowUs(throttle, cfg_.revCap); break;
                case EscState::Braking: us = 0; break;  // hold neutral to arm reverse
                case EscState::IdleFwd: us = belowUs(cfg_.revArmBrake, 1000); break;  // brake first
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
    }

    Config cfg_;
    fpv::ser::Control cmd_{};
    uint32_t lastCmdMs_ = 0;
    bool haveCmd_ = false;
    bool needNeutral_ = true;
    bool gearRev_ = false;

    EscState esc_ = EscState::IdleFwd;
    Pulse pending_ = Pulse::Neutral;
    uint32_t pendingSinceMs_ = 0;
    bool sawBelow_ = false;
    bool mightReverse_ = false;
};

}  // namespace drive
