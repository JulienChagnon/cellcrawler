# Tuning

Do all of this with the **wheels off the ground**, using `mode = serial` in `fpv.ini` (the
ESP32 plugged straight into the PC) unless noted.

## ESC

1. **Neutral and endpoints.** The firmware outputs 1500 µs neutral and 1000–2000 µs full
   range. If the ESC was calibrated to the stock transmitter it almost certainly matches. If
   the motor creeps at neutral, recalibrate the ESC (see its manual) or adjust
   `escNeutralUs`.
2. **Direction.** Press RT gently. If the wheels spin backwards, set `escInvert = true`.
3. **ESC mode.** Most RC trucks ship in *Forward/Brake/Reverse* (`EscMode::FwdBrakeRev`):
   * Gear **R** + RT → the firmware sends brake → neutral → reverse automatically. Reverse
     should start within about 0.2 s. If it only brakes, raise `commitMs` (the ESC needs to
     see each step for longer).
   * **Double-tap guard.** Put the gear in **F**, tap LT, release, tap LT again. The wheels
     must **not** spin backwards. You may see a tiny forward twitch from the 40 µs guard
     pulse; that is expected. If the truck does reverse, raise `guardUs` above the ESC's
     neutral deadband (try 60). If it lurches forward, lower it.
   * If your ESC is set to *Forward/Reverse* (crawler, no brake), use `EscMode::FwdRev`.
     LT then coasts.
   * If your ESC is set to *Forward/Brake* (reverse disabled), set `guardEnabled = false`.

## Throttle caps

`fwdCap` / `revCap` / `brakeCap` in `config.h` are **hard limits**, in permille of full
throttle. The PC can never exceed them. They're set to 1000 / 1000 / 1000, so the PC's
**throttle limit** (Tab menu, D-pad or arrow ↑ / ↓, default 60 %, saved in `fpv.ini` as
`throttle_scale`) is a plain percentage of full power forward. Lower the caps here if you
want a limit nobody can raise from the PC.

A brushed motor whines without moving at low throttle: the ESC is already pulsing current
through it, just not enough to turn the wheels. That's normal. Keyboard throttle starts at
30 % for this reason.

## PWM rate (latency)

At 50 Hz the servo and ESC see a new command only every 20 ms. That adds 0–20 ms (10 ms on
average) of control latency.

* **Digital** servo: try `kSteerPwmHz = 200` … `333`. **Never** go above 50 Hz on an analog
  servo, because it will overheat.
* Most brushless ESCs accept 200–400 Hz. Test `kEscPwmHz` at 200 and check that neutral is
  still neutral.
* After raising the rates you may lower `commitMs` (it must still cover at least 2 PWM
  periods).

## Steering

`steerCenterUs` sets mechanical centre (use the D-pad ← / → for live trim first, then bake the
value in). `steerRangeUs` sets the lock. Reduce it if the servo buzzes at full lock.
`steer_expo` in `fpv.ini` softens the centre.

## Video (phone app)

| Setting | Default | Notes |
|---|---|---|
| Resolution | 1280×720 | 720p is the latency/quality sweet spot on the SD665 encoder |
| FPS | 60 | 30 fps adds about 17 ms of frame interval latency |
| Max bitrate | 4000 kbps | The app adapts down automatically when LTE queues or loses packets. Lower the max if your upload is weak. |
| Camera id | main back | The app lists the cameras. On the Mi A3 the ultrawide may appear as another id, which gives a wider FOV, useful for FPV. |

Focus is locked at infinity and electronic stabilisation is off (it buffers frames).

## Reading the OSD

* `RTT`: PC ↔ phone round trip. The minimum is the network floor; anything above it is
  queueing.
* `LOSS`: phone → PC datagram loss over the last second.
* `VID … Q`: `Q` is the network queueing delay estimated from video frames. The bitrate
  controller backs off when this rises.
* `LAT ~X = cam+enc + net + pc`: estimated capture-to-display latency. The one-way split uses
  RTT/2, so it's approximate on asymmetric LTE links.
* `ESP32 USB`: phone ↔ ESP32 serial round trip (normally 1–3 ms).

## Measuring true glass-to-glass latency

Show a millisecond timer on the PC monitor (for example an online stopwatch). Point the truck
camera at it so that the FPV window also shows the timer. Film both with another phone in
slow motion (240 fps), then read the difference between the real timer and the one in the
FPV window. Expect roughly 60–100 ms on home WiFi and 100–160 ms on LTE, depending on the
cell.
