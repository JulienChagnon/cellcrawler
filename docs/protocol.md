# Wire protocol

Defined in `common/fpv/` (header-only, shared by all three programs). All integers are
little-endian.

## PC ↔ phone (UDP)

```
datagram = header(12) | payload | mac(8)
header   = 'F' 'V' version:u8 type:u8 session:u32 seq:u32
mac      = SipHash-2-4(key, header | payload)
```

* `session`: random per sender start-up. The PC accepts a new phone session only from a
  HELLO. The phone accepts a PC session only from a HELLO_ACK that echoes its own session.
* `seq`: +1 per datagram (shared across all types). A 64-entry sliding window rejects
  replays and duplicates.
* Datagrams are ≤ 1200 bytes, which fits the IPv6 minimum MTU and Tailscale.

| Type | Dir | Rate | Payload |
|---|---|---|---|
| 1 HELLO | phone → PC | 2 Hz until acked, then 0.5 Hz (NAT keepalive) | width u16, height u16, fps u8 |
| 2 HELLO_ACK | PC → phone | per HELLO | phoneSession u32 |
| 3 CONTROL | PC → phone | on change (≤ 500 Hz) + 100 Hz heartbeat | pcTimeUs u64, steer i16, throttle u16, brake u16, flags u8 |
| 4 TELEMETRY | phone → PC | 20 Hz | RTT echo, phone clock, ESP32 state, phone battery/temp/RSRP, video stats |
| 5 VIDEO | phone → PC | per frame | frameId u32, chunkIdx u16, chunkCount u16, flags u8, captureUs u64, phoneLatUs u32, data |
| 6 VIDEO_FEEDBACK | PC → phone | 5 Hz + immediate keyframe requests | requestKeyframe u8, lossPermille u16, queueDelayMs u16, recvKbps u32 |

Control values: steer −1000…1000, throttle and brake 0…1000. Flags: bit0 armed, bit1 reverse
gear, bit2 failsafe (the PC's controller is gone; the ESP32 brakes and ignores the rest),
bit7 record (network only: the phone records full-quality video while it's set, and strips
the bit before forwarding to the ESP32). Telemetry phone flag bit5 says the phone is
recording. Every CONTROL carries the full state, so a lost packet is simply superseded.

Video: one encoded H.264 frame (Annex B, with SPS/PPS before keyframes) is split into chunks
of exactly `kMaxVideoChunk` bytes; only the last chunk is shorter. The PC decodes a frame as
soon as all of its chunks have arrived. It abandons incomplete frames as soon as a newer frame
completes, and requests a keyframe when frames are lost.

RTT = PC now − echoed `pcTimeUs` − `echoHoldUs` (time the phone held it). The phone clock
offset is estimated from the lowest-RTT samples, which lets the PC turn the phone's camera
timestamp into a capture-to-display latency.

## Host ↔ ESP32 (USB serial, 921600 8N1)

```
frame = COBS( type:u8 | payload | crc16:u16 ) 0x00      crc = CRC-16/CCITT-FALSE
```

| Type | Dir | Payload |
|---|---|---|
| 0x01 CONTROL | host → ESP32 | seq u16, steer i16, throttle u16, brake u16, flags u8 |
| 0x81 TELEMETRY | ESP32 → host (20 Hz) | lastSeq u16, battMv u16, cells u8, flags u8, steerUs u16, throttleUs u16, rxErrors u16, uptimeMs u32 |

When the ESP32 receives a CONTROL with `seq % 16 == 0`, it replies with an immediate
TELEMETRY frame so the host can time the serial round trip.
Telemetry flags: failsafe, armed, gear reverse, reverse engaged, need-neutral, low battery.
