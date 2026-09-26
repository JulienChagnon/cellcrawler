// ESP32 truck controller: USB serial commands in, servo/ESC PWM out, battery telemetry back.
#include <Arduino.h>
#include <driver/ledc.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "drive_logic.h"
#include "fpv/battery.h"
#include "fpv/serial_proto.h"

namespace {

using namespace fpv;

drive::DriveLogic logic(cfg::driveConfig());
ser::FrameParser parser;

// ---- PWM (ESP-IDF LEDC, 16-bit) ------------------------------------------------------
constexpr ledc_mode_t kMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_bit_t kRes = LEDC_TIMER_16_BIT;

struct PwmOut {
    ledc_channel_t channel;
    uint32_t hz;
    uint16_t lastUs;
};
PwmOut steerPwm{LEDC_CHANNEL_0, cfg::kSteerPwmHz, 0};
PwmOut escPwm{LEDC_CHANNEL_1, cfg::kEscPwmHz, 0};

void pwmInit(PwmOut& p, ledc_timer_t timer, int pin, uint16_t us) {
    ledc_timer_config_t t = {};
    t.speed_mode = kMode;
    t.duty_resolution = kRes;
    t.timer_num = timer;
    t.freq_hz = p.hz;
    t.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&t);

    ledc_channel_config_t c = {};
    c.gpio_num = pin;
    c.speed_mode = kMode;
    c.channel = p.channel;
    c.intr_type = LEDC_INTR_DISABLE;
    c.timer_sel = timer;
    c.duty = static_cast<uint32_t>(uint64_t(us) * p.hz * 65536ULL / 1000000ULL);
    c.hpoint = 0;
    ledc_channel_config(&c);
    p.lastUs = us;
}

// The new duty takes effect at the start of the next PWM period.
void pwmWrite(PwmOut& p, uint16_t us) {
    if (us == p.lastUs) return;
    p.lastUs = us;
    uint32_t duty = static_cast<uint32_t>(uint64_t(us) * p.hz * 65536ULL / 1000000ULL);
    ledc_set_duty(kMode, p.channel, duty);
    ledc_update_duty(kMode, p.channel);
}

drive::Output lastOut{};

void apply(uint32_t nowMs) {
    lastOut = logic.update(nowMs);
    pwmWrite(steerPwm, lastOut.steerUs);
    pwmWrite(escPwm, lastOut.throttleUs);
}

// ---- Battery ------------------------------------------------------------------------
float battSlowMv = 0;  // ~2 s time constant, reported
float battFastMv = 0;  // ~50 ms time constant, used for detection
uint8_t cells = cfg::kCells;
uint32_t stableSinceMs = 0;

void sampleBattery(uint32_t nowMs) {
    float pinMv = analogReadMilliVolts(cfg::kBattAdcPin);
    float mv = pinMv * float(cfg::kDividerR1 + cfg::kDividerR2) / float(cfg::kDividerR2) * cfg::kBattCalibration;

    float prevFast = battFastMv;
    battFastMv += (mv - battFastMv) * 0.2f;
    // Pack plugged in after boot: jump straight to the new level instead of a slow ramp.
    if (battSlowMv < 1000 && battFastMv > 2500) battSlowMv = battFastMv;
    battSlowMv += (mv - battSlowMv) * 0.005f;

    if (cfg::kCells == 0 && cells == 0) {
        bool stable = battFastMv > 2500 && fabsf(battFastMv - prevFast) < battFastMv * 0.002f;
        if (!stable) stableSinceMs = nowMs;
        else if (nowMs - stableSinceMs > 500) cells = battery::detectCells(uint32_t(battFastMv), cfg::kChemistry);
    }
}

// ---- Serial -------------------------------------------------------------------------
void sendTelemetry(uint32_t nowMs);

void pollSerial(uint32_t nowMs) {
    uint8_t buf[128];
    int avail;
    while ((avail = Serial.available()) > 0) {
        size_t n = Serial.read(buf, avail > int(sizeof buf) ? sizeof buf : size_t(avail));
        parser.feed(buf, n, [&](uint8_t type, const uint8_t* p, size_t len) {
            ser::Control c;
            if (type == ser::kControl && ser::decodeControl(p, len, c)) {
                logic.onCommand(nowMs, c);
                apply(nowMs);  // act immediately, don't wait for the next loop tick
                if (c.seq % ser::kEchoEvery == 0) sendTelemetry(nowMs);
            }
        });
    }
}

void sendTelemetry(uint32_t nowMs) {
    ser::Telemetry t;
    t.lastSeq = logic.lastSeq();
    t.battMv = uint16_t(battSlowMv < 0 ? 0 : battSlowMv > 65535 ? 65535 : battSlowMv);
    t.cells = cells;
    t.flags = lastOut.flags;
    if (cells > 0 && t.battMv < cells * battery::lowCellMv(cfg::kChemistry)) t.flags |= ser::kTelLowBattery;
    t.steerUs = lastOut.steerUs;
    t.throttleUs = lastOut.throttleUs;
    t.rxErrors = parser.errors();
    t.uptimeMs = nowMs;
    uint8_t frame[ser::kMaxFrame];
    size_t n = ser::encodeTelemetry(t, frame);
    if (Serial.availableForWrite() >= int(n)) Serial.write(frame, n);  // never block the control loop
}

// ---- LED: solid = armed, slow blink = link but disarmed, fast blink = failsafe --------
void updateLed(uint32_t nowMs) {
    bool on;
    if (lastOut.flags & ser::kTelFailsafe) on = (nowMs / 100) % 2;
    else if (lastOut.flags & ser::kTelArmed) on = true;
    else on = (nowMs / 500) % 2;
    digitalWrite(cfg::kLedPin, on);
}

void watchdogInit() {
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t c = {};
    c.timeout_ms = cfg::kWatchdogSeconds * 1000;
    c.idle_core_mask = 0;
    c.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&c) != ESP_OK) esp_task_wdt_init(&c);
#else
    esp_task_wdt_init(cfg::kWatchdogSeconds, true);
#endif
    esp_task_wdt_add(nullptr);
}

}  // namespace

void setup() {
    // Outputs go to neutral before anything else so the truck is inert while booting.
    const drive::Config dc = cfg::driveConfig();
    pwmInit(steerPwm, LEDC_TIMER_0, cfg::kSteerPin, dc.steerCenterUs);
    pwmInit(escPwm, LEDC_TIMER_1, cfg::kEscPin, dc.escNeutralUs);
    lastOut = {dc.steerCenterUs, dc.escNeutralUs, ser::kTelFailsafe};

    pinMode(cfg::kLedPin, OUTPUT);
    analogSetPinAttenuation(cfg::kBattAdcPin, ADC_11db);

    Serial.setRxBufferSize(1024);
    Serial.begin(ser::kBaud);
    watchdogInit();
}

void loop() {
    static uint32_t lastTelemetryMs = 0, lastBattMs = 0;
    const uint32_t now = millis();

    pollSerial(now);
    apply(now);  // failsafe timeout and reverse sequencing advance with time

    if (now - lastBattMs >= 10) {
        lastBattMs = now;
        sampleBattery(now);
    }
    if (now - lastTelemetryMs >= cfg::kTelemetryPeriodMs) {
        lastTelemetryMs = now;
        sendTelemetry(now);
    }
    updateLed(now);
    esp_task_wdt_reset();
    delayMicroseconds(200);
}
