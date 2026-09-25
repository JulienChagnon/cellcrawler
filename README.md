# FPV Truck over LTE

Drive an RC truck from a Windows PC with an Xbox controller, with live video and battery
telemetry, over the cellular network. Built for **lowest possible latency**.

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

| Input | Action |
|---|---|
| **Menu (≡)** | Arm |
| **View (⧉)** or **B** | Disarm |
| **RT** | Throttle in the current gear |
| **LT** | Brake (in reverse gear: coast) |
| **LB** | Toggle gear F ↔ R (only while RT is released) |
| Left stick X | Steering |
| D-pad ↑ / ↓ | Throttle scale ±10 % (of the firmware cap) |
| D-pad ← / → | Steering trim |
| F11 / Esc | Fullscreen / quit |

## Safety layers

* **ESP32:** no valid command for 150 ms → neutral throttle and centred steering. After a
  link loss longer than 1 s, disarm, or boot, it ignores throttle until the trigger is released.
  It enforces hard throttle caps (default 40 % forward, 25 % reverse) and a hardware watchdog.
* **Phone:** no command from the PC for 250 ms → sends explicit neutral to the ESP32.
* **PC:** controller unplugged → disarmed. RTT over `rtt_limit_ms` → throttle halved.
* Every UDP packet is authenticated (SipHash-2-4 MAC with a shared key) and replay-checked,
  so traffic to the open port cannot drive the truck.
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
build\fpv-pc.exe --genkey                                  # prints a new shared key
copy build\fpv.ini.example build\fpv.ini                   # then paste the key into fpv.ini
build\fpv-pc.exe
```

### Phone app
Open `android/` in Android Studio, then build and run on the Mi A3. From the command line
you can also run `cd android && gradlew assembleDebug` and install
`app/build/outputs/apk/debug/app-debug.apk`. The app asks for the PC address, the port, and
the same key as `fpv.ini`.

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
   `fpv-fake-phone --key <key>` on the same PC. You should see a test pattern and a full OSD.
3. **Phone on home WiFi:** use the PC's LAN IP as the address. Plug the ESP32 into the phone
   with an OTG adapter.
4. **Phone on LTE:** follow docs/networking.md (NAT check, port forward, dynamic DNS).
5. **Tune:** see docs/tuning.md.

## Docs

* [docs/wiring.md](docs/wiring.md): pins, power, battery divider values
* [docs/networking.md](docs/networking.md): reaching the PC over LTE without a VPS
* [docs/tuning.md](docs/tuning.md): ESC setup, caps, PWM rate, video settings, measuring latency
* [docs/protocol.md](docs/protocol.md): wire formats
