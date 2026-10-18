# FPV Truck over LTE

Drive an RC truck from a Windows PC with an Xbox controller, with live video and battery
telemetry, over the cellular network. Built for **lowest possible latency**.

Truck: **ECX 1/10 AMP MT 2WD** (ECX03034I), 540 20T brushed motor, 7.2 V 6-cell NiMH pack.
The stock ESC/receiver is replaced by a standalone brushed ESC (see docs/wiring.md).

```
Xbox pad ──XInput 1 kHz──▶ fpv-pc (C++) ══UDP control ≥100 Hz══▶ phone app (C++) ──USB serial──▶ ESP32 ──PWM──▶ servo / ESC
                           fpv-pc      ◀══UDP H.264 video═══════ camera → HW encoder (zero copy)
                           fpv-pc      ◀══UDP telemetry 20 Hz═══ phone stats + ESP32 battery ADC
```

| Part | Where | Stack |
|---|---|---|
| Ground station | `pc/` | C++17, SDL3 (window/render), FFmpeg (D3D11VA decode), XInput, Winsock |
| Phone (Xiaomi Mi A3) | `android/` | NDK C++: Camera2 NDK → AMediaCodec, POSIX UDP, usbdevfs serial. Thin Java shim for permissions/service |
| Truck controller | `firmware/esp32/` | PlatformIO + Arduino-ESP32, LEDC PWM, ADC |
| Wire protocol | `common/fpv/` | Header-only C++ shared by all three |
| Tests | `tests/` | Protocol + firmware drive logic + PC mapping |
| Fake phone | `tools/fake_phone.cpp` | Test the PC side without the truck |

## Controls

| Controller | Keyboard | Action |
|---|---|---|
| **Menu (≡)** | **Enter** | Arm |
| **View (⧉)** or **B** | **Backspace** or **B** | Disarm |
| **RT** | **W** | Throttle in the current gear (keyboard: starts at 30 %, full after 0.3 s) |
| **LT** | **S** | Brake (in reverse gear: coast) |
| **LB** | **R** | Toggle gear F ↔ R (only while the throttle is released) |
| Left stick X | **A / D** | Steering (keyboard: ramps to full lock over 0.2 s) |
| D-pad ↑ / ↓ | ↑ / ↓ | Throttle limit ±10 % (default 60 %, saved to `fpv.ini`) |
| D-pad ← / → | ← / → | Steering trim, coarse (saved to `fpv.ini`) |
| − / = | − / = | Steering trim, fine |
| | **Tab** | Menu: controller ↔ keyboard, throttle limit, steering trim, HUD details, fullscreen, key help |
| | **F3** | HUD details (RTT, video, latency breakdown, ESP32 PWM, phone) |
| | F11 / Esc | Fullscreen / quit |

Start in keyboard mode with `input = keyboard` in `fpv.ini`, or switch in the Tab menu
(switching disarms). With the keyboard, the `fpv-pc` window must have focus: clicking away
while armed counts as losing the controller, so the truck brakes.

## Safety layers

* **ESP32:** no valid command for 150 ms, or a failsafe frame from the phone or PC → 50 %
  brake (`failsafeBrake`) and centred steering, held until the link is back. Before the first
  arm after boot it stays in neutral so the ESC can arm. After a link loss longer than 1 s,
  disarm, or boot, it ignores throttle until the trigger is released.
  It enforces hard throttle caps (100 % both ways; the PC app's throttle limit, default
  60 %, scales them) and a hardware watchdog.
* **Phone:** no command from the PC for 250 ms → tells the ESP32 to go to failsafe at once.
* **PC:** controller unplugged while armed → failsafe (brakes) until it's back, then
  disarmed. RTT over `rtt_limit_ms` → throttle halved.
* Every UDP packet is authenticated (SipHash-2-4 MAC with a key derived from a shared
  passphrase) and replay-checked, so traffic to the open port cannot drive the truck.
* LT tapped twice at a standstill will **not** reverse the truck. The firmware inserts a
  short forward guard pulse to drop the ESC out of its reverse-armed state (see docs/tuning.md).

**First power-up must be with the wheels off the ground.**

## Build

### Firmware (ESP32)
```
pip install platformio
cd firmware/esp32
pio run -t upload          # NodeMCU-32S on USB
```
Edit `firmware/esp32/src/config.h` for pins, caps, PWM rate, ESC mode and battery divider.

### PC app
Needs a MinGW-w64 GCC (MSYS2 UCRT64 works) or MSVC, CMake and Ninja.
```
cd pc
powershell -ExecutionPolicy Bypass -File fetch_deps.ps1     # SDL3 + FFmpeg into pc/third_party
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
copy build\fpv.ini.example build\fpv.ini                   # then set passphrase = ... in fpv.ini
build\fpv-pc.exe
```

### Phone app
Run `cd android && gradlew assembleRelease`, or open `android/` in Android Studio. Install
`app/build/outputs/apk/release/app-release.apk` (an optimized build) on the Mi A3. The app
asks for the PC address, the port, and the same passphrase as `fpv.ini`.

### Tests
```
cmake -S tests -B tests/build && cmake --build tests/build && tests/build/fpv-tests
```

## Bring-up order

1. **Bench, no phone:** flash the ESP32 and wire it up (docs/wiring.md). Set `mode = serial`
   and `serial_port = COMx` in `fpv.ini`, then run `fpv-pc`. Check steering, throttle, the
   caps, the LB gear toggle, the reverse sequence and the double-tap-brake guard with the
   wheels up. Unplug USB and the truck must stop.
2. **PC video path, no truck:** run `fpv-pc` in `udp` mode and
   `fpv-fake-phone --key "<passphrase>"` on the same PC. You should see a test pattern and a full OSD.
3. **Phone on home WiFi:** use the PC's LAN IP as the address. Plug the ESP32 into the phone
   with an OTG adapter.
4. **Phone on LTE:** follow docs/networking.md (NAT check, port forward, dynamic DNS).
5. **Tune:** see docs/tuning.md.

## Docs

* [docs/wiring.md](docs/wiring.md): pins, power, battery divider values
* [docs/networking.md](docs/networking.md): reaching the PC over LTE without a VPS
* [docs/tuning.md](docs/tuning.md): ESC setup, caps, PWM rate, video settings, measuring latency
* [docs/protocol.md](docs/protocol.md): wire formats
