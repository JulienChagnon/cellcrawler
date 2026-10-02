# Wiring

```
                         ┌──────────── Mi A3 ────────────┐
                         │  USB-C (OTG adapter)           │
                         └──────────────┬─────────────────┘
                                        │ USB (5 V + data)
                               ┌────────┴────────┐
                               │  NodeMCU-32S    │
   Steering servo signal ◀──── │ GPIO25          │
   ESC signal            ◀──── │ GPIO26          │
                               │ GPIO34 ◀── divider tap ◀── Battery +
                               │ GND  ────────────┬──────── ESC / servo GND (−)
                               └──────────────────┘         Battery −
```

* **ESC:** the stock ECX13006 is a combined ESC + receiver with no throttle signal input, so
  the ESP32 cannot drive it. Replace it with a standalone brushed ESC that has a BEC (for
  example a Hobbywing QuicRun 1060, with its battery jumper set to NiMH).
* **Power:** the ESP32 is powered by the phone over USB OTG (about 100 mA without WiFi). The
  servo is powered by the ESC's BEC: join the red and black wires of the ESC signal lead to
  the servo's red and black wires, for example with a servo Y-lead. Only the white/yellow
  signal wires go to the ESP32. **Do not** connect the BEC's red wire to the ESP32.
* **Ground:** ESP32 GND must be connected to the ESC/servo ground. Without it the PWM signals
  have no reference.
* **Signals:** 3.3 V PWM from GPIO25/26 is accepted by nearly every servo and ESC. Unplug
  the stock receiver.
* **LED (GPIO2):** fast blink = failsafe (no commands), slow blink = link up but disarmed,
  solid = armed.

## Battery voltage divider

`Battery+ → R1 → GPIO34 → R2 → GND`, plus a **100 nF** capacitor from GPIO34 to GND. Keep the
pin at or below about 2.5 V at full charge, which keeps the ESP32 ADC in its linear range.

| Pack | Full charge | R1 | R2 | Pin at full |
|---|---|---|---|---|
| 6-cell NiMH (7.2 V, stock) | ~9.0 V off the charger | 47 kΩ | 15 kΩ | 2.18 V |
| 2S LiPo | 8.4 V | 47 kΩ | 15 kΩ | 2.03 V |
| 3S LiPo | 12.6 V | 68 kΩ | 12 kΩ | 1.89 V |
| 4S LiPo | 16.8 V | 100 kΩ | 15 kΩ | 2.19 V |
| 6S LiPo | 25.2 V | 100 kΩ | 10 kΩ | 2.29 V |

The firmware is set (`kDividerR1/kDividerR2` in `config.h`) to **47 k / 15 k** for the stock
6-cell NiMH pack. That is also correct for 2S LiPo. For 3S or more you **must** change it to
the matching row before connecting, or the pin will go over 3.3 V.

**Calibration:** measure the pack with a multimeter, compare it with the `BAT` value on the
OSD, and set `kBattCalibration = measured / shown` in `config.h`.

The firmware is set to `kChemistry = NiMH` and `kCells = 6`. If you switch to LiPo, change
`kChemistry` and set `kCells` to the cell count, or to 0 to auto-detect it at power-up. Do not
auto-detect NiMH, because a flat 6-cell pack reads as 5 cells. Capacity % comes from a
resting-voltage curve, so it reads low under throttle and recovers when coasting. NiMH
voltage is flat for most of the discharge, so expect the % to be rough.

## Parts

* NodeMCU-32S (have)
* Standalone brushed ESC with a BEC, rated for 6-cell NiMH and for the stock motor (for
  example a Hobbywing QuicRun 1060). It replaces the stock ECX13006 ESC/receiver.
* USB-C OTG adapter or cable (phone → micro-USB on the NodeMCU)
* Resistors for the divider (above) + 100 nF capacitor
* 3 jumper wires with servo-style connectors
* Optional later: a 5 V / 3 A buck converter from the main pack to charge the phone. The Mi A3
  cannot normally charge and act as a USB host at the same time, so this also needs an
  "OTG + charge" Y-adapter. Test that on the bench first.
