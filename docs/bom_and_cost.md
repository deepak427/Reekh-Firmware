# AI Scarecrow — Bill of Materials & Cost Estimate

All prices are exact Amazon.in retail prices (October 2026).  
Conversion rate: **1 USD = ₹94.94**

---

## Complete BOM

### Core Compute & Vision

| # | Component | Model / Variant | Qty | Unit Price (₹) | Total (₹) | Notes |
|---|-----------|-----------------|-----|----------------|-----------|-------|
| 1 | ESP32-CAM | AI-Thinker with OV3660 | 1 | ₹689 | ₹689 | Main compute + WiFi + camera |
| 2 | ESP32-CAM-MB | Micro USB programmer shield | 1 | ₹119 | ₹119 | Flashing only; removed for field use |

### Power System

| # | Component | Model / Variant | Qty | Unit Price (₹) | Total (₹) | Notes |
|---|-----------|-----------------|-----|----------------|-----------|-------|
| 3 | 18650 Li-Ion Battery | 3.7V 2500mAh | 3 | ₹102 | ₹306 | Wired in series for 11.1V |
| 4 | 3-Cell Battery Holder | 18650 × 3 series, no cover | 1 | ₹33 | ₹33 | |
| 5 | LM2596 Buck Converter | DC-DC adjustable step-down | 1 | ₹48 | ₹48 | Steps 11.1V → 5.0V regulated |

### Actuators & Deterrents

| # | Component | Model / Variant | Qty | Unit Price (₹) | Total (₹) | Notes |
|---|-----------|-----------------|-----|----------------|-----------|-------|
| 6 | Micro Servo | TowerPro SG90 9G 180° | 2 | ₹99 | ₹198 | Both physically mounted; tilt locked at 0° in firmware (V1 pan-only) |
| 7 | Pan/Tilt Bracket | ROBODUINO SG90 mount, servo not included | 1 | ₹169.75 | ₹169.75 | Camera + dual servo mechanical mount |
| 8 | LED Spotlight | AutoKraftZ Cone 9-LED | 1 | ₹104 | ₹104 | Visual deterrent |
| 9 | Relay Module | 5V 30A 1-channel | 1 | ₹145 | ₹145 | Switches spotlight |
| 10 | DFPlayer Mini | MP3-TF-16P | 1 | ₹95 | ₹95 | Audio driver |
| 11 | Speaker | SP Electron 2" 4Ω 3W round | 1 | ₹165 | ₹165 | Audio output |

### Sensors

| # | Component | Model / Variant | Qty | Unit Price (₹) | Total (₹) | Notes |
|---|-----------|-----------------|-----|----------------|-----------|-------|
| 12 | PIR Motion Sensor | SmartElex BX604 | 1 | ₹199 | ₹199 | Hardware interrupt wake trigger |

### Wiring & Assembly

| # | Component | Qty | Unit Price (₹) | Total (₹) | Notes |
|---|-----------|-----|----------------|-----------|-------|
| 13 | M-M Jumper Wires 20cm 40pcs | 1 | ₹55 | ₹55 | |
| 14 | M-F Jumper Wires 20cm 40pcs | 1 | ₹49 | ₹49 | |
| 15 | F-F Jumper Wires 20cm 40pcs | 1 | ₹55 | ₹55 | |
| 16 | Breadboard 400 tie-point | 1 | ₹35 | ₹35 | |
| 17 | USB-A to Micro-USB cable 1m | 1 | ₹42 | ₹42 | ESP32-CAM-MB programming |
| 18 | Double-sided Foam Tape | 1 | ₹75 | ₹75 | NUOVI 10m × 2mm |
| 19 | PVC Electrical Tape | 1 | ₹159.05 | ₹159.05 | On Blow 20m, insulation |
| 20 | Nylon Cable Ties 100mm × 100 | 1 | ₹159.05 | ₹159.05 | GLOBOMOTIVE, cable management |
| 21 | Hot Melt Glue Gun 20W | 1 | ₹159.05 | ₹159.05 | THEMISTO, lock jumper connections for field |

### Tools & Setup

| # | Component | Qty | Unit Price (₹) | Total (₹) | Notes |
|---|-----------|-----|----------------|-----------|-------|
| 22 | Digital Multimeter | SP Electron DT830D | 1 | ₹187 | ₹187 | Mandatory for LM2596 calibration |
| 23 | microSD Card | HIKVISION Extreme 8GB Class 10 V30 | 1 | ₹369 | ₹369 | DFPlayer audio files storage |
| 24 | USB Card Reader | Digtek DCR-007 USB-A 3.0 & Type-C | 1 | ₹199 | ₹199 | Copying audio files to SD card |

---

## Cost Summary

### Part 1 — Main Device Cost

Components permanently installed in the device. Does not include wires, tools, SD card, or card reader.

| Category | Total (₹) | Total (USD) |
|----------|-----------|-------------|
| Compute & Vision | ₹808 | $8.51 |
| Power System | ₹387 | $4.08 |
| Actuators & Deterrents | ₹876.75 | $9.24 |
| Sensors | ₹199 | $2.10 |
| **Main Device Total** | **₹2,270.75** | **$23.92** |

> The tilt servo (₹99) is physically mounted on the pan/tilt bracket but its PWM pin is not attached in firmware V1. Tilt is hardcoded to 0° — the bracket holds the camera steady. Both servos are included in cost as both are present in the hardware.

### Part 2 — Total Build Cost

Includes everything above plus wiring, consumables, tools, SD card, and card reader.

| Category | Total (₹) | Total (USD) |
|----------|-----------|-------------|
| Main Device | ₹2,270.75 | $23.92 |
| Wiring & Assembly | ₹733.15 | $7.72 |
| Tools & Setup | ₹755 | $7.95 |
| **Grand Total** | **₹3,758.90** | **$39.59** |

> Wiring, tools, SD card, and card reader are one-time purchases reusable across future builds. The incremental cost of a second device is approximately ₹2,270.75 ($23.92).

---

## Cloud Running Cost (AWS)

| Resource | Instance | Monthly Cost (USD) |
|----------|----------|-------------------|
| Inference Service | t4g.medium (ARM Graviton, 2 vCPU, 4 GB) | ~$25 |
| Data transfer | ~1 GB/day JPEG frames | ~$3 |
| **Total Cloud** | | **~$28/month** |

> The t4g.medium provides sufficient throughput for one ESP32-CAM at 10–12 FPS. For multi-camera deployments a t4g.large (~$50/month) is recommended.

### Multi-Device Cost Efficiency

A single backend instance handles multiple ESP32-CAM devices simultaneously. Each additional device adds only the hardware cost — the cloud cost is shared.

| Devices | Total Hardware | Cloud/Month | Cloud Cost Per Device/Month |
|---------|---------------|-------------|----------------------------|
| 1 | ~$24 | ~$28 | ~$28 |
| 3 | ~$72 | ~$28 | ~$9.30 |
| 5 | ~$120 | ~$30* | ~$6 |
| 10 | ~$240 | ~$50** | ~$5 |

\* Slightly higher data transfer at higher frame volumes.  
\** t4g.large recommended for 10+ simultaneous devices.

---

## Firmware Note — Tilt Servo

Both SG90 servos are physically mounted on the ROBODUINO pan/tilt bracket. However, the tilt servo PWM pin (`GPIO 14`) is **not attached** in firmware V1. The firmware hardcodes `tilt = 0.0°` in all outgoing frame metadata and always forces `currentTilt = 0.0f` in `servoTask` regardless of any command received. Only `PAN_SERVO_PIN` is driven via `ledcAttach`. Tilt will be enabled in V2.

---

## Cost vs. Commercial Alternative

| Solution | Capital Cost | Operating Cost |
|----------|-------------|----------------|
| AI Scarecrow (this project) — device only | ~$24 | ~$28/month cloud |
| AI Scarecrow (this project) — full build | ~$40 | ~$28/month cloud |
| Commercial trail camera + monitoring service | $150–$400 | $30–$100/month |
| Human guard patrol | — | $500–$1,500/month |
