# AI Scarecrow — Hardware Architecture & Wiring

Complete blueprint for the solder-free, field-deployable AI Scarecrow wildlife deterrent.

---

## System Block Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        POWER SUBSYSTEM                                  │
│                                                                         │
│  3× 18650 Li-Ion (11.1V)                                                │
│  [Battery Pack] ─────────┬──── LM2596 Buck Converter ────► 5.0V Rail   │
│                           └──── Relay high-voltage block ──► Spotlight  │
└─────────────────────────────────────────────────────────────────────────┘
                                      │
                               5.0V Clean Rail
                     ┌─────────────────┼──────────────────┐
                     │                 │                   │
              ┌──────▼──────┐  ┌──────▼──────┐   ┌───────▼──────┐
              │ ESP32-CAM   │  │  Pan Servo  │   │ Tilt Servo   │
              │ (GPIO + Cam)│  │   SG90      │   │   SG90       │
              └──────┬──────┘  └─────────────┘   └──────────────┘
                     │
          ┌──────────┼───────────────────────┐
          │          │                       │
    ┌─────▼────┐ ┌───▼──────┐    ┌──────────▼──────┐
    │  PIR     │ │DFPlayer  │    │   5V Relay       │
    │ (GPIO 13)│ │(GPIO16/17│    │   (GPIO 15)      │
    └──────────┘ └──┬───────┘    └──────────┬───────┘
                    │                        │
               ┌────▼────┐           ┌───────▼──────┐
               │ Speaker │           │ 12V Spotlight │
               └─────────┘           └──────────────┘

                     │ WiFi
                     ▼
          ┌────────────────────┐
          │  AWS EC2 Graviton  │
          │  Inference Service │
          │  (OpenCV 5 DNN)    │
          └────────────────────┘
```

---

## Pin Mapping

| Component | Signal | ESP32 GPIO | Wire Colour | Notes |
|-----------|--------|-----------|-------------|-------|
| Camera (OV2640) | SDA/SCL/Data | Multiple | — | AI-Thinker onboard, no wiring needed |
| PIR Sensor | Digital OUT | GPIO 13 | — | RISING edge interrupt; INPUT_PULLDOWN |
| Pan Servo | PWM Signal | GPIO 12 | Orange | Absolute angle 0–180° |
| Tilt Servo | PWM Signal | GPIO 14 | Orange | Absolute angle 0–180° (V1: locked 0°) |
| Spotlight Relay | Signal IN | GPIO 15 | — | 3.3V logic → 11.1V load |
| DFPlayer RX | Serial TX | GPIO 17 | — | ESP32 TX → DFPlayer RX; add 1kΩ if noisy |
| DFPlayer TX | Serial RX | GPIO 16 | — | DFPlayer TX → ESP32 RX |
| DFPlayer SPK+ | Audio | Speaker+ | — | Direct connection |
| DFPlayer SPK− | Audio | Speaker− | — | Direct connection |
| Status LED | Digital OUT | GPIO 33 | — | Onboard red LED, active LOW |
| Flash LED | Digital OUT | GPIO 4 | — | Onboard high-power white LED |

### Camera Module Pins (AI-Thinker, onboard — reference only)

```
PWDN=32  RESET=-1  XCLK=0   SIOD=26  SIOC=27
Y9=35    Y8=34     Y7=39    Y6=36    Y5=21
Y4=19    Y3=18     Y2=5
VSYNC=25 HREF=23   PCLK=22
```

---

## Power Distribution (3 Zones)

### Zone A — Raw Power (11.1V)

```
Battery Pack (+) ──┬──► LM2596 IN+
                   └──► Relay high-voltage switch block (spotlight)
Battery Pack (−) ──┬──► LM2596 IN−
                   └──► Spotlight GND
```

### Zone B — Clean Logic Power (5.0V)

All 5V loads share the LM2596 output terminals. Strip and twist multiple wires into each terminal clamp.

```
LM2596 OUT+ (5.0V) ──► ESP32-CAM 5V pin
                   ──► Pan Servo VCC (Red)
                   ──► Tilt Servo VCC (Red)
                   ──► DFPlayer VCC
                   ──► Relay Module VCC

LM2596 OUT− (GND) ──► ESP32-CAM GND pin
                  ──► Pan Servo GND (Brown/Black)
                  ──► Tilt Servo GND (Brown/Black)
                  ──► DFPlayer GND
                  ──► Relay Module GND
                  ──► PIR Sensor GND
```

> ⚠️ **Common ground is mandatory.** Without it, 3.3V PWM signals to servos will float and behave erratically.

### Zone C — 3.3V Logic

```
ESP32-CAM 3.3V pin ──► PIR Sensor VCC
```

---

## Critical Safety Rules

**Rule 1 — Calibrate before connecting any load.**
Attach only the battery to the LM2596. Measure `OUT+` with a multimeter. Adjust the trimmer screw until the reading is exactly **5.00V**. Only then connect the ESP32 and other devices.

**Rule 2 — Polarity is unforgiving.**
Reversed VCC/GND will instantly destroy the ESP32 and servo motors. Always verify Red → VCC, Black/Brown → GND before powering on.

**Rule 3 — Common ground.**
Every device on the 5V rail must share the same GND (LM2596 `OUT−`). A floating ground causes unpredictable PWM/logic behaviour.

**Rule 4 — Isolate 11.1V from 5V.**
The raw battery voltage (11.1V) must never contact the 5V logic rail. The only crossing point is the LM2596 converter itself.

**Rule 5 — Lock solder-free connections.**
Once a jumper wire connection is tested and confirmed, apply a small dab of hot-melt glue over the pin to prevent it popping out in the field.

**Rule 6 — Sequential bring-up.**
Test one zone at a time:
1. Calibrate LM2596 with only the battery connected
2. Connect ESP32 + PIR → verify boot blink sequence
3. Connect servos → verify PWM movement
4. Connect DFPlayer + relay/spotlight last

---

## Firmware Task Architecture

```
Core 0: WiFi / lwIP (ESP-IDF, do not schedule tasks here)

Core 1:
┌──────────────────────────────────────────────────────────────┐
│ pirTask       priority 5   GPIO 13 RISING → IDLE/ACTIVE SM   │
│ cameraTask    priority 4   Capture JPEG → WebSocket send      │
│ servoTask     priority 3   Drain servoQueue → GPIO PWM        │
│ wsManageTask  priority 2   webSocket.loop() + heartbeats      │
└──────────────────────────────────────────────────────────────┘

Inter-task communication:
  pirTask ──► deviceState (volatile) ──► cameraTask
  wsManageTask ──► xQueueSend(servoQueue) ──► servoTask
```

### Stack & Priority Budgets

| Task | Stack | Priority |
|------|-------|----------|
| pirTask | 4096 bytes | 5 (highest) |
| cameraTask | 10240 bytes | 4 |
| servoTask | 3072 bytes | 3 |
| wsManageTask | 6144 bytes | 2 (lowest) |

---

## Servo Control Design

Servos receive **absolute angles**, not relative deltas. This eliminates cumulative drift — the servo always moves to the exact target position the backend computed, regardless of prior position.

The backend sends:
```json
{"pan": 12.5, "tilt": 0.0, "skip": false, "action": "SOUND_DOG_BARK"}
```

The `servoTask` writes these values directly to the hardware PWM controller via `ledc` or the Servo library.

Pan range: −90° to +90° (centred at 0°)  
Tilt range: V1 locked at 0°; V2 will enable −30° to +30°
