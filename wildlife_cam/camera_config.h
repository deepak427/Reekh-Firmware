#pragma once

// ─── Board selection ──────────────────────────────────────────────────────────
// Your board is AI-Thinker ESP32-CAM. Do not change this.
#define CAMERA_MODEL_AI_THINKER

// ─── Camera pins (AI-Thinker) ─────────────────────────────────────────────────
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ─── Other hardware pins ──────────────────────────────────────────────────────
#define PIR_PIN           13   // PIR sensor DATA output
#define FLASH_LED_PIN      4   // Onboard flash LED (white high-power deterrent)
#define STATUS_LED_PIN    33   // Onboard small red LED (active LOW, safe low-power)
#define PAN_SERVO_PIN     12   // Pan servo PWM signal
#define TILT_SERVO_PIN    14   // Tilt servo PWM signal
#define RELAY_PIN         15   // Relay IN (spotlight)
#define DFPLAYER_TX_PIN   17   // ESP32 TX → DFPlayer RX
#define DFPLAYER_RX_PIN   16   // ESP32 RX → DFPlayer TX

// ─── Network — fill these in before flashing ─────────────────────────────────
// Copy this section into your own config and set your values.
// Never commit real credentials to version control.
#define WIFI_SSID         "YOUR_WIFI_SSID"
#define WIFI_PASSWORD     "YOUR_WIFI_PASSWORD"
#define WS_HOST           "3.108.11.185"         // reekh.deepaksingh.site (or EC2 IP: 3.108.11.185)
#define WS_PORT           8000
#define WS_PATH           "/ws/esp32-cam-01"
#define API_KEY           "YOUR_API_KEY"          // must match API_KEY in inference_service/.env
#define CAMERA_ID         "esp32-cam-01"

// ─── Timing ───────────────────────────────────────────────────────────────────
#define PIR_IDLE_TIMEOUT_MS    30000   // 30s no PIR → go IDLE
#define HEARTBEAT_INTERVAL_MS  10000   // 10s IDLE heartbeat
#define WS_LOOP_INTERVAL_MS       15   // wsManageTask tick
#define PIR_CHECK_INTERVAL_MS    100   // pirTask timeout-check interval

// ─── FreeRTOS task config ─────────────────────────────────────────────────────
// All tasks on Core 1. Core 0 is reserved for WiFi/lwIP (SDK-managed).
#define TASK_CORE              1

#define PIR_TASK_STACK      4096   // increased — ample stack for sendStatusMsg
#define PIR_TASK_PRIORITY      5   // highest — must respond instantly

#define CAM_TASK_STACK     10240   // increased — ample stack for frame transmission
#define CAM_TASK_PRIORITY      4

#define SERVO_TASK_STACK    3072
#define SERVO_TASK_PRIORITY    3

#define WS_TASK_STACK       6144   // WebSocket loop + JSON parsing
#define WS_TASK_PRIORITY       2

// ─── Camera quality ───────────────────────────────────────────────────────────
// With PSRAM: VGA, 2 buffers. Without: QVGA, 1 buffer.
#define CAM_XCLK_FREQ     10000000   // 10MHz rock-solid DMA FIFO timing
#define CAM_QUALITY_PSRAM        15   // 15 = standard crisp VGA, prevents FB-OVF
#define CAM_QUALITY_DRAM         15
