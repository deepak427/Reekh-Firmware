# Autonomous Wildlife Deterrent — Firmware Service

Edge firmware for the **AI Scarecrow** wildlife deterrent system, running on the **AI-Thinker ESP32-CAM** with a custom FreeRTOS multi-task architecture.

---

## What This Does

The firmware is the physical edge node of the system. It:

1. Wakes on a PIR motion interrupt (zero idle power draw while waiting)
2. Streams JPEG frames to the cloud inference service over a persistent WebSocket
3. Receives absolute pan/tilt servo angles and deterrent action commands back
4. Drives two SG90 servos to physically track the detected animal
5. Actuates deterrents — flash LED, DFPlayer audio, 12V spotlight relay

---

## Hardware

| Component | Model | Role |
|-----------|-------|------|
| Microcontroller | AI-Thinker ESP32-CAM | Compute + camera + WiFi |
| Camera | OV2640 / OV3660 | 640×480 JPEG capture |
| Motion sensor | SmartElex BX604 PIR | Low-power wake trigger |
| Pan servo | TowerPro SG90 | Horizontal animal tracking |
| Tilt servo | TowerPro SG90 | Physically mounted on bracket; PWM pin not attached in V1 — tilt hardcoded to 0° in firmware. Will be enabled in V2. |
| Audio | Mini DFPlayer MP3 + 3W speaker | Bark / siren deterrent |
| Spotlight | AutoKraftZ 12V 9-LED + relay | Visual deterrent |
| Power | 3× 18650 Li-Ion → LM2596 buck → 5V | Field deployment |

See [`docs/hardware_architecture.md`](docs/hardware_architecture.md) for full wiring, pin mapping, and safety rules.
See [`docs/bom_and_cost.md`](docs/bom_and_cost.md) for part numbers and estimated cost.

---

## Firmware Architecture

All logic runs in **four FreeRTOS tasks on Core 1**. Core 0 is reserved for the WiFi/lwIP SDK stack.

```
Core 0: WiFi / lwIP (SDK-managed, do not use)
Core 1:
  ┌─────────────────────────────────────────────────────────────────┐
  │  pirTask      (priority 5)  GPIO 13 interrupt → IDLE/ACTIVE SM  │
  │  cameraTask   (priority 4)  Capture + WebSocket JPEG transmit   │
  │  servoTask    (priority 3)  Drain servoQueue → GPIO PWM write   │
  │  wsManageTask (priority 2)  webSocket.loop() + heartbeats       │
  └─────────────────────────────────────────────────────────────────┘
```

### State Machine

```
[IDLE]  ←── PIR_IDLE_TIMEOUT_MS (30s) ───┐
   │                                     │
   │  PIR RISING interrupt               │
   ▼                                     │
[ACTIVE] ─── stream frames ────────────►[backend] ──► servo cmds + actions
```

### Communication Protocol

**Primary — WebSocket** `/ws/esp32-cam-01` (persistent, bidirectional):
- **Send (two-message frame):**
  1. Text: `{"type":"frame","seq":42,"camera_id":"esp32-cam-01","pan":0.0,"tilt":0.0}`
  2. Binary: raw JPEG bytes
- **Receive:**
  `{"pan":12.5,"tilt":0.0,"action":"SOUND_DOG_BARK","phase":"TRACKING","skip":false}`

**Fallback — HTTP** (if WebSocket unavailable):
- `POST /frame` — multipart JPEG upload
- `GET /servo` — poll latest pan/tilt
- `GET /action` — poll latest deterrent action

### LED Blink Diagnostic Codes

No serial monitor needed in field deployment. All state is communicated via GPIO 33 (red LED, active LOW):

| Pattern | Meaning |
|---------|---------|
| 3 slow blinks (500ms) | Boot started |
| Rapid flicker | WiFi connecting |
| 5 fast blinks (100ms) | WiFi connected ✓ |
| SOS loop | Camera init failed or WiFi timeout |
| 1 blink every 3s | IDLE — alive, waiting for PIR |
| 4 fast blinks (80ms) | PIR triggered → ACTIVE |
| 1 very short blink (30ms) | Frame sent, 202 received ✓ |
| 2 medium blinks (200ms) | Frame HTTP error |
| 3 medium blinks (200ms) | Camera capture failed |
| 2 slow blinks (400ms) | PIR timeout → returning to IDLE |

---

## File Structure

```
firmware_service/
├── README.md                        # This file
├── docs/
│   ├── hardware_architecture.md     # Wiring, pin map, power zones, safety rules
│   └── bom_and_cost.md              # Full BOM with part numbers and estimated cost
└── wildlife_cam/
    ├── wildlife_cam.ino             # Main sketch — setup() + FreeRTOS task creation
    ├── camera_config.h              # All pin defines, WiFi credentials, timing constants
    ├── ws_client.h                  # Shared state declarations, task + queue prototypes
    └── ws_client.cpp                # WebSocket client, JSON parsing, servo command queue
```

---

## Setup & Flash

### Dependencies (Arduino IDE Library Manager)

| Library | Author | Version |
|---------|--------|---------|
| WebSockets | Markus Sattler | Latest |
| ArduinoJson | Benoit Blanchon | v6 |

### Board Settings

| Setting | Value |
|---------|-------|
| Board | AI Thinker ESP32-CAM |
| Partition Scheme | Huge APP (3MB No OTA) |
| Upload Speed | 115200 |

### Configuration (edit `camera_config.h`)

```cpp
#define WIFI_SSID     "your_network"
#define WIFI_PASSWORD "your_password"
#define WS_HOST       "your-ec2-ip"   // e.g. 13.235.66.226
#define WS_PORT       8000
#define API_KEY       "your-api-key"
```

### Flash Steps

1. Attach the ESP32-CAM-MB programming shield
2. Open `wildlife_cam/wildlife_cam.ino` in Arduino IDE
3. Set board + partition scheme as above
4. Upload
5. Remove the MB shield for field deployment (power from battery pack)
6. Follow the **sequential bring-up** steps in [`docs/hardware_architecture.md`](docs/hardware_architecture.md)

---

## Integration with Inference Service

The firmware is the physical client of the [Inference Service](../inference_service/README.md). Ensure:
- The inference service is running and accessible at `WS_HOST:WS_PORT`
- The `API_KEY` in `camera_config.h` matches `API_KEY` in `inference_service/.env`
- The `CAMERA_ID` value (`esp32-cam-01`) matches the WebSocket path on the backend

### Multi-Device Cloud Cost Scaling

A single backend instance handles multiple ESP32-CAM devices simultaneously. Each additional device only adds the ~$24 hardware cost — the ~$28/month cloud cost is shared across all devices.

| Devices | Hardware Cost | Cloud Cost/Month | Cost Per Device/Month |
|---------|--------------|------------------|-----------------------|
| 1 | ~$24 | ~$28 | ~$28 |
| 3 | ~$72 | ~$28 | ~$9.30 |
| 5 | ~$120 | ~$30* | ~$6 |
| 10 | ~$240 | ~$50** | ~$5 |

\* Slightly higher data transfer cost at higher frame volumes.  
\** A t4g.large (~$50/month) is recommended for 10+ simultaneous devices.
