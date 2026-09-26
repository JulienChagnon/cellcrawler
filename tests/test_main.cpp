// Host-side unit tests for the shared protocol and the firmware drive logic.
// Build & run: see tests/CMakeLists.txt (or `g++ -std=c++17 -I../common -I../firmware/esp32/src test_main.cpp`).
#include <cstdio>
#include <cstring>
#include <vector>

#include "drive_logic.h"
#include "fpv/battery.h"
#include "fpv/cobs.h"
#include "fpv/crc16.h"
#include "fpv/net_proto.h"
#include "fpv/serial_proto.h"
#include "fpv/siphash.h"
#include "mapping.h"

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)
#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        long long va_ = (long long)(a), vb_ = (long long)(b);                              \
        if (va_ != vb_) {                                                                  \
            std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            ++failures;                                                                    \
        }                                                                                  \
    } while (0)

using namespace fpv;

static void testCrc() {
    const char* s = "123456789";
    CHECK_EQ(crc16(reinterpret_cast<const uint8_t*>(s), 9), 0x29B1);
}

static void testCobs() {
    std::vector<std::vector<uint8_t>> cases = {
        {}, {0}, {0, 0}, {1, 2, 3}, {1, 0, 2, 0}, {0, 1},
    };
    std::vector<uint8_t> big(600);
    for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i % 7 == 0 ? 0 : i);
    cases.push_back(big);
    std::vector<uint8_t> noZero(254, 0x11);  // exactly one full block
    cases.push_back(noZero);
    noZero.push_back(0x22);
    cases.push_back(noZero);

    for (auto& c : cases) {
        std::vector<uint8_t> enc(cobsMaxEncoded(c.size())), dec(c.size() + 1);
        size_t n = cobsEncode(c.data(), c.size(), enc.data());
        for (size_t i = 0; i < n; ++i) CHECK(enc[i] != 0);
        size_t m = cobsDecode(enc.data(), n, dec.data());
        CHECK_EQ(m, c.size());
        CHECK(std::memcmp(dec.data(), c.data(), c.size()) == 0);
    }
}

static void testSipHash() {
    SipKey k;
    CHECK(parseSipKey("000102030405060708090a0b0c0d0e0f", k));
    uint8_t msg[64];
    for (int i = 0; i < 64; ++i) msg[i] = uint8_t(i);
    // Reference vectors from the SipHash paper / reference implementation.
    CHECK(siphash24(k, msg, 0) == 0x726fdb47dd0e0e31ULL);
    CHECK(siphash24(k, msg, 15) == 0xa129ca6149be45e5ULL);
    CHECK(siphash24(k, msg, 8) == 0x93f5f5799a932462ULL);
    CHECK(!parseSipKey("0001", k));
    CHECK(!parseSipKey("zz0102030405060708090a0b0c0d0e0f", k));
}

static void testSerialFrames() {
    ser::Control c;
    c.seq = 0xBEEF;
    c.steer = -734;
    c.throttle = 1000;
    c.brake = 0;  // zeros exercise COBS
    c.flags = ser::kArmed | ser::kGearReverse;
    uint8_t buf[ser::kMaxFrame * 3];
    size_t n = ser::encodeControl(c, buf);
    CHECK(n > 0 && buf[n - 1] == 0);

    // Garbage before, two frames back to back, fed one byte at a time.
    std::vector<uint8_t> stream = {0x55, 0x12, 0x00};
    stream.insert(stream.end(), buf, buf + n);
    ser::Telemetry t;
    t.lastSeq = 7;
    t.battMv = 12345;
    t.cells = 3;
    t.flags = ser::kTelArmed;
    t.uptimeMs = 0x01020304;
    size_t m = ser::encodeTelemetry(t, buf);
    stream.insert(stream.end(), buf, buf + m);

    ser::FrameParser p;
    int controls = 0, telems = 0;
    for (uint8_t b : stream) {
        p.feed(&b, 1, [&](uint8_t type, const uint8_t* pl, size_t len) {
            if (type == ser::kControl) {
                ser::Control d;
                CHECK(ser::decodeControl(pl, len, d));
                CHECK_EQ(d.seq, 0xBEEF);
                CHECK_EQ(d.steer, -734);
                CHECK_EQ(d.throttle, 1000);
                CHECK_EQ(d.flags, ser::kArmed | ser::kGearReverse);
                ++controls;
            } else if (type == ser::kTelemetry) {
                ser::Telemetry d;
                CHECK(ser::decodeTelemetry(pl, len, d));
                CHECK_EQ(d.battMv, 12345);
                CHECK_EQ(d.cells, 3);
                CHECK_EQ(d.uptimeMs, 0x01020304);
                ++telems;
            }
        });
    }
    CHECK_EQ(controls, 1);
    CHECK_EQ(telems, 1);
    CHECK_EQ(p.errors(), 1);  // the garbage prefix

    // Corrupted byte -> rejected.
    n = ser::encodeControl(c, buf);
    buf[3] ^= 0x40;
    if (buf[3] == 0) buf[3] = 1;
    int got = 0;
    p.feed(buf, n, [&](uint8_t, const uint8_t*, size_t) { ++got; });
    CHECK_EQ(got, 0);
}

static void testNetPackets() {
    SipKey key;
    parseSipKey("00112233445566778899aabbccddeeff", key);
    uint8_t buf[net::kMaxDatagram];
    net::Header h{net::kControl, 0xCAFEBABE, 42};
    net::Control c;
    c.pcTimeUs = 0x1122334455667788ULL;
    c.steer = 500;
    c.throttle = 250;
    c.brake = 10;
    c.flags = ser::kArmed;
    ByteWriter w = net::beginPacket(buf, sizeof buf, h);
    net::write(w, c);
    size_t n = net::finishPacket(w, buf, key);
    CHECK(n == net::kHeaderSize + 15 + net::kMacSize);

    net::Header rh;
    const uint8_t* pl;
    size_t plen;
    CHECK(net::openPacket(buf, n, key, rh, pl, plen));
    CHECK_EQ(rh.type, net::kControl);
    CHECK(rh.session == 0xCAFEBABE);
    CHECK_EQ(rh.seq, 42);
    net::Control d;
    CHECK(net::read(ByteReader(pl, plen), d));
    CHECK(d.pcTimeUs == c.pcTimeUs);
    CHECK_EQ(d.steer, 500);

    // Tamper / wrong key -> rejected.
    buf[net::kHeaderSize + 9] ^= 1;
    CHECK(!net::openPacket(buf, n, key, rh, pl, plen));
    buf[net::kHeaderSize + 9] ^= 1;
    SipKey other;
    parseSipKey("ffffffffffffffffffffffffffffffff", other);
    CHECK(!net::openPacket(buf, n, other, rh, pl, plen));
    CHECK(!net::openPacket(buf, 10, key, rh, pl, plen));

    // Telemetry round trip.
    net::Telemetry t;
    t.echoPcTimeUs = 99;
    t.battMv = 11100;
    t.rsrpDbm = -97;
    t.phoneTempDeciC = -55;
    t.videoBitrate = 4000000;
    w = net::beginPacket(buf, sizeof buf, {net::kTelemetry, 1, 2});
    net::write(w, t);
    n = net::finishPacket(w, buf, key);
    CHECK(net::openPacket(buf, n, key, rh, pl, plen));
    net::Telemetry u;
    CHECK(net::read(ByteReader(pl, plen), u));
    CHECK_EQ(u.battMv, 11100);
    CHECK_EQ(u.rsrpDbm, -97);
    CHECK_EQ(u.phoneTempDeciC, -55);
    CHECK_EQ(u.videoBitrate, 4000000);

    // Largest video chunk fits the datagram budget.
    net::VideoHeader vh;
    vh.chunkCount = 1;
    w = net::beginPacket(buf, sizeof buf, {net::kVideo, 1, 3});
    net::write(w, vh);
    CHECK_EQ(w.size(), net::kHeaderSize + net::kVideoHeaderSize);
    std::vector<uint8_t> chunk(net::kMaxVideoChunk, 0xAB);
    w.bytes(chunk.data(), chunk.size());
    n = net::finishPacket(w, buf, key);
    CHECK_EQ(n, net::kMaxDatagram);
}

static void testReplayWindow() {
    net::ReplayWindow r;
    CHECK(r.accept(100));
    CHECK(!r.accept(100));
    CHECK(r.accept(102));
    CHECK(r.accept(101));  // reordered but new
    CHECK(!r.accept(101));
    CHECK(r.accept(200));
    CHECK(!r.accept(130));  // too old
    CHECK(r.accept(150));
    CHECK(!r.accept(150));
}

static void testBattery() {
    using namespace battery;
    CHECK_EQ(detectCells(8400, Chemistry::LiPo), 2);
    CHECK_EQ(detectCells(7000, Chemistry::LiPo), 2);
    CHECK_EQ(detectCells(12600, Chemistry::LiPo), 3);
    CHECK_EQ(detectCells(10500, Chemistry::LiPo), 3);
    CHECK_EQ(detectCells(16800, Chemistry::LiPo), 4);
    CHECK_EQ(detectCells(25200, Chemistry::LiPo), 6);
    CHECK_EQ(detectCells(500, Chemistry::LiPo), 0);
    CHECK_EQ(percent(8400, 2, Chemistry::LiPo), 100);
    CHECK_EQ(percent(7680, 2, Chemistry::LiPo), 50);
    CHECK_EQ(percent(6000, 2, Chemistry::LiPo), 0);
    int p = percent(11400, 3, Chemistry::LiPo);  // 3.80 V/cell
    CHECK_EQ(p, 40);
}

// ---------------------------------------------------------------------------------------
// Drive logic

namespace {
struct Sim {
    drive::Config cfg;
    drive::DriveLogic logic;
    uint32_t now = 0;
    uint16_t seq = 0;
    drive::Output out{};

    explicit Sim(drive::Config c = {}) : cfg(c), logic(c) {}

    void cmd(uint16_t throttle, uint16_t brake, bool rev, bool armed = true, int16_t steer = 0) {
        ser::Control c;
        c.seq = ++seq;
        c.steer = steer;
        c.throttle = throttle;
        c.brake = brake;
        c.flags = uint8_t((armed ? ser::kArmed : 0) | (rev ? ser::kGearReverse : 0));
        logic.onCommand(now, c);
        out = logic.update(now);
    }
    // Holds the same command for ms milliseconds, sending it every 10 ms like the PC does.
    void hold(uint32_t ms, uint16_t throttle, uint16_t brake, bool rev, bool armed = true) {
        for (uint32_t t = 0; t < ms; t += 10) {
            now += 10;
            cmd(throttle, brake, rev, armed);
        }
    }
    int thr() const { return int(out.throttleUs) - 1500; }
};
}  // namespace

static void testFailsafeAndArming() {
    Sim s;
    // No command yet -> neutral + failsafe flag.
    s.out = s.logic.update(0);
    CHECK_EQ(s.out.throttleUs, 1500);
    CHECK(s.out.flags & ser::kTelFailsafe);

    // Armed but trigger already held at connect -> ignored until released.
    s.hold(100, 1000, 0, false);
    CHECK_EQ(s.thr(), 0);
    CHECK(s.out.flags & ser::kTelNeedNeutral);
    s.hold(20, 0, 0, false);
    s.hold(20, 1000, 0, false);
    CHECK_EQ(s.thr(), 200);  // 1000 * 40% cap * 500us

    // Commands stop -> neutral within failsafeMs, steering centred.
    s.cmd(1000, 0, false, true, 800);
    CHECK(s.out.steerUs == 1500 + 320);
    s.now += 151;
    s.out = s.logic.update(s.now);
    CHECK_EQ(s.thr(), 0);
    CHECK_EQ(s.out.steerUs, 1500);
    CHECK(s.out.flags & ser::kTelFailsafe);

    // Short blip (< relatchMs): driving resumes without releasing the trigger.
    s.hold(50, 1000, 0, false);
    CHECK_EQ(s.thr(), 200);

    // Long outage: link returns with trigger held -> must release first.
    s.now += 1200;
    s.out = s.logic.update(s.now);
    CHECK_EQ(s.thr(), 0);
    s.hold(50, 1000, 0, false);
    CHECK_EQ(s.thr(), 0);
    CHECK(s.out.flags & ser::kTelNeedNeutral);
    s.hold(20, 0, 0, false);
    s.hold(20, 500, 0, false);
    CHECK_EQ(s.thr(), 100);

    // Disarm -> neutral immediately.
    s.cmd(500, 0, false, false);
    CHECK_EQ(s.thr(), 0);
}

static void testCapsCannotBeExceeded() {
    Sim s;
    s.hold(20, 0, 0, false);
    s.cmd(65535, 0, false);  // out-of-range input
    CHECK_EQ(s.thr(), 200);
    s.hold(20, 0, 65535, false);
    CHECK(s.thr() >= -500);
}

static void testGearChangeOnlyAtZeroThrottle() {
    Sim s;
    s.hold(20, 0, 0, false);
    s.hold(50, 800, 0, false);
    s.hold(50, 800, 0, true);  // asks for reverse while on throttle
    CHECK(!(s.out.flags & ser::kTelGearReverse));
    CHECK(s.thr() > 0);
    s.hold(20, 0, 0, true);
    CHECK(s.out.flags & ser::kTelGearReverse);
}

static void testReverseSequence() {
    Sim s;
    s.hold(20, 0, 0, true);  // arm, select reverse
    CHECK(s.out.flags & ser::kTelGearReverse);
    // RT pressed in reverse: brake pulse, then neutral, then reverse.
    std::vector<int> seen;
    for (int i = 0; i < 40; ++i) {
        s.hold(10, 1000, 0, true);
        int t = s.thr();
        if (seen.empty() || seen.back() != t) seen.push_back(t);
    }
    // Expect: arm brake (-150us = 30% of 500), neutral (0), reverse (-125us = 25% cap)
    CHECK_EQ(seen.size(), 3);
    if (seen.size() == 3) {
        CHECK_EQ(seen[0], -150);
        CHECK_EQ(seen[1], 0);
        CHECK_EQ(seen[2], -125);
    }
    CHECK(s.logic.escState() == drive::EscState::Reversing);

    // Release and press again: already armed, reverse immediately.
    s.hold(100, 0, 0, true);
    s.hold(10, 1000, 0, true);
    CHECK_EQ(s.thr(), -125);

    // LT in reverse gear coasts.
    s.hold(10, 0, 800, true);
    CHECK_EQ(s.thr(), 0);
}

static void testDoubleBrakeTapDoesNotReverse() {
    Sim s;
    s.hold(20, 0, 0, false);
    s.hold(200, 1000, 0, false);  // drive forward
    s.hold(100, 0, 1000, false);  // brake
    CHECK_EQ(s.thr(), -500);
    s.hold(100, 0, 0, false);  // release: ESC now reverse-armed
    CHECK(s.logic.mightReverse());
    // Brake again: forward guard pulse first, then brake.
    s.hold(10, 0, 1000, false);
    CHECK_EQ(s.thr(), 40);
    s.hold(80, 0, 1000, false);
    CHECK_EQ(s.thr(), -500);
    CHECK(!s.logic.mightReverse() || s.logic.escState() == drive::EscState::Braking);
}

static void testQuickBrakeTapStillGuarded() {
    // A 10 ms tap is shorter than commitMs, but the ESC may still have seen it.
    Sim s;
    s.hold(20, 0, 0, false);
    s.hold(10, 0, 1000, false);
    s.hold(10, 0, 0, false);
    CHECK(s.logic.mightReverse());
    s.hold(10, 0, 1000, false);
    CHECK_EQ(s.thr(), 40);
}

static void testFwdRevMode() {
    drive::Config c;
    c.escMode = drive::EscMode::FwdRev;
    Sim s(c);
    s.hold(20, 0, 0, true);
    s.hold(10, 1000, 0, true);
    CHECK_EQ(s.thr(), -125);
    s.hold(10, 0, 1000, true);
    CHECK_EQ(s.thr(), 0);
}

static void testInversion() {
    drive::Config c;
    c.escInvert = true;
    c.steerInvert = true;
    Sim s(c);
    s.hold(20, 0, 0, false);
    s.cmd(1000, 0, false, true, 1000);
    CHECK_EQ(s.thr(), -200);
    CHECK_EQ(s.out.steerUs, 1100);
}

// ---------------------------------------------------------------------------------------
// PC controller mapping

static void testMapping() {
    pc::MappingConfig mc;
    mc.steerExpo = 0;
    mc.steerDeadzone = 0;
    mc.triggerDeadzone = 0;
    pc::Mapper m(mc);
    pc::PadState p;
    p.connected = true;

    // Disarmed by default: triggers do nothing.
    p.rt = 1;
    pc::DriveCommand c = m.update(p);
    CHECK_EQ(c.throttle, 0);
    CHECK(!(c.flags & ser::kArmed));

    // Arm, full throttle.
    p.buttons = pc::kStart;
    c = m.update(p);
    p.buttons = 0;
    c = m.update(p);
    CHECK(c.flags & ser::kArmed);
    CHECK_EQ(c.throttle, 1000);

    // LB while on throttle: refused.
    p.buttons = pc::kLB;
    c = m.update(p);
    CHECK(!(c.flags & ser::kGearReverse));
    CHECK(m.takeNote() == "RELEASE THROTTLE TO SHIFT");
    p.buttons = 0;
    m.update(p);

    // LB with throttle released: toggles, and toggles back on the next press only.
    p.rt = 0;
    p.buttons = pc::kLB;
    c = m.update(p);
    CHECK(c.flags & ser::kGearReverse);
    c = m.update(p);  // held, no new edge
    CHECK(c.flags & ser::kGearReverse);
    p.buttons = 0;
    m.update(p);
    p.buttons = pc::kLB;
    c = m.update(p);
    CHECK(!(c.flags & ser::kGearReverse));
    p.buttons = 0;

    // Brake, throttle scale, link limiter.
    p.lt = 0.5f;
    p.rt = 1;
    p.buttons = pc::kDpadDown;
    c = m.update(p, 0.5f);
    CHECK_EQ(c.brake, 500);
    CHECK_EQ(c.throttle, 450);  // 1.0 * 0.9 scale * 0.5 limiter
    p.buttons = 0;

    // Steering: invert-free linear, clamped with trim.
    p.lx = -1;
    c = m.update(p);
    CHECK_EQ(c.steer, -1000);
    p.buttons = pc::kDpadRight;
    m.update(p);
    p.buttons = 0;
    p.lx = 1;
    c = m.update(p);
    CHECK_EQ(c.steer, 1000);
    p.lx = 0;
    c = m.update(p);
    CHECK_EQ(c.steer, 10);

    // B disarms immediately; controller loss disarms.
    p.buttons = pc::kB;
    c = m.update(p);
    CHECK(!(c.flags & ser::kArmed));
    CHECK_EQ(c.throttle, 0);
    p.buttons = pc::kStart;
    m.update(p);
    p.connected = false;
    c = m.update(p);
    CHECK(!(c.flags & ser::kArmed));
    p.connected = true;
    p.buttons = 0;
    c = m.update(p);
    CHECK(!(c.flags & ser::kArmed));
}

static void testSteerExpoDeadzone() {
    pc::MappingConfig mc;
    mc.steerDeadzone = 0.1f;
    mc.steerExpo = 0.5f;
    pc::Mapper m(mc);
    pc::PadState p;
    p.connected = true;
    p.lx = 0.05f;
    CHECK_EQ(m.update(p).steer, 0);
    p.lx = 1.0f;
    CHECK_EQ(m.update(p).steer, 1000);
    p.lx = 0.55f;  // halfway after the deadzone -> 0.5*0.5 + 0.5*0.125 = 0.3125
    CHECK_EQ(m.update(p).steer, 313);
    p.lx = -0.55f;
    CHECK_EQ(m.update(p).steer, -313);
}

int main() {
    testCrc();
    testCobs();
    testSipHash();
    testSerialFrames();
    testNetPackets();
    testReplayWindow();
    testBattery();
    testFailsafeAndArming();
    testCapsCannotBeExceeded();
    testGearChangeOnlyAtZeroThrottle();
    testReverseSequence();
    testDoubleBrakeTapDoesNotReverse();
    testQuickBrakeTapStillGuarded();
    testFwdRevMode();
    testInversion();
    testMapping();
    testSteerExpoDeadzone();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
