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

* **Power:** the ESP32 is powered by the phone over USB OTG (about 100 mA without WiFi). The
  servo keeps getting power from the ESC's BEC as it does now. **Do not** connect the BEC's
  red wire to the ESP32.
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
| 2S LiPo | 8.4 V | 47 kΩ | 15 kΩ | 2.03 V |
| 3S LiPo | 12.6 V | 68 kΩ | 12 kΩ | 1.89 V |
| 4S LiPo | 16.8 V | 100 kΩ | 15 kΩ | 2.19 V |
| 6S LiPo | 25.2 V | 100 kΩ | 10 kΩ | 2.29 V |

The firmware default (`kDividerR1/kDividerR2` in `config.h`) is **100 k / 15 k**, which is
safe for anything up to 4S. For 6S you **must** change it to 100 k / 10 k before connecting.
If you only know the truck model, look up its pack and pick the row.

**Calibration:** measure the pack with a multimeter, compare it with the `BAT` value on the
OSD, and set `kBattCalibration = measured / shown` in `config.h`.

The cell count is auto-detected at power-up (`kCells = 0`). Set it explicitly if you
sometimes plug in a very flat pack. Capacity % comes from a LiPo resting-voltage curve, so
it reads low under throttle and recovers when coasting.

## Parts

* NodeMCU-32S (have)
* USB-C OTG adapter or cable (phone → micro-USB on the NodeMCU)
* Resistors for the divider (above) + 100 nF capacitor
* 3 jumper wires with servo-style connectors
* Optional later: a 5 V / 3 A buck converter from the main pack to charge the phone. The Mi A3
  cannot normally charge and act as a USB host at the same time, so this also needs an
  "OTG + charge" Y-adapter. Test that on the bench first.
