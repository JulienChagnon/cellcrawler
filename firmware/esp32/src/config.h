// Build-time firmware configuration. Everything that depends on the truck lives here.
// See docs/tuning.md before changing PWM rates or caps.
#pragma once
#include "drive_logic.h"
#include "fpv/battery.h"

namespace cfg {

// ---- Pins (NodeMCU-32S) --------------------------------------------------------------
constexpr int kSteerPin = 25;
constexpr int kEscPin = 26;
constexpr int kBattAdcPin = 34;  // ADC1, input only. ADC2 pins cannot be used.
constexpr int kLedPin = 2;       // onboard blue LED

// ---- PWM ---------------------------------------------------------------------------
// 50 Hz is safe for every servo/ESC. Digital servos usually accept 200-333 Hz, which cuts
// up to ~17 ms of latency. NEVER run an analog servo above 50 Hz (it will overheat).
constexpr uint32_t kSteerPwmHz = 50;
constexpr uint32_t kEscPwmHz = 50;

// ---- Drive -------------------------------------------------------------------------
inline drive::Config driveConfig() {
    drive::Config c;
    c.steerCenterUs = 1500;
    c.steerRangeUs = 500;  // full lock = 1000 / 2000 us. Lower it if the servo buzzes at full lock.
    c.steerInvert = false;

    c.escMode = drive::EscMode::FwdBrakeRev;
    c.escNeutralUs = 1500;
    c.escFwdRangeUs = 500;
    c.escRevRangeUs = 500;
    c.escInvert = false;
    c.escDeadbandUs = 25;

    // Hard limits (permille of full throttle). Start low, raise after testing.
    c.fwdCap = 400;
    c.revCap = 250;
    c.brakeCap = 1000;

    c.revArmBrake = 300;
    c.guardUs = 40;
    c.guardEnabled = true;
    // Must cover >= 2 PWM periods at the ESC rate (20 ms each at 50 Hz).
    c.commitMs = 60;
    c.failsafeMs = 150;
    // Brake strength when the link drops while armed (permille of full brake; 0 = coast
    // in neutral like before). Held until the link comes back.
    c.failsafeBrake = 500;
    c.relatchMs = 1000;
    return c;
}

// ---- Battery -----------------------------------------------------------------------
// Divider: battery+ -> R1 -> GPIO34 -> R2 -> GND, 100 nF from GPIO34 to GND.
// Pick R1/R2 for your pack (docs/wiring.md) so the pin never exceeds ~2.5 V.
constexpr uint32_t kDividerR1 = 47000;
constexpr uint32_t kDividerR2 = 15000;  // 6-cell NiMH: ~9.0 V off the charger -> 2.18 V
// Multiply by measured/reported after checking with a multimeter (1.000 = no correction).
constexpr float kBattCalibration = 1.000f;
constexpr fpv::battery::Chemistry kChemistry = fpv::battery::Chemistry::NiMH;  // stock 7.2 V 6-cell 1800 mAh pack
constexpr uint8_t kCells = 6;  // 0 = auto-detect (unreliable for NiMH: a flat 6-cell reads as 5)

// ---- Link ----------------------------------------------------------------------------
constexpr uint32_t kTelemetryPeriodMs = 50;
constexpr uint32_t kWatchdogSeconds = 1;

}  // namespace cfg
