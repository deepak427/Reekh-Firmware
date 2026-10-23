#include "ws_client.h"
#include "camera_config.h"
#include "esp_camera.h"
#include <WiFi.h>
#include <WebSocketsClient.h>   // arduinoWebSockets by Markus Sattler
#include <ArduinoJson.h>         // ArduinoJson v6

// ─── Shared volatile state ────────────────────────────────────────────────────

volatile DeviceState_t deviceState      = STATE_IDLE;
volatile uint32_t      lastPirTriggerMs = 0;
volatile bool          pirTriggered     = false;   // set by ISR/simulate, cleared by pirTask
volatile bool          wsConnected      = false;
volatile float         currentPan       = 0.0f;
volatile float         currentTilt      = 0.0f;

// ─── Task handles ─────────────────────────────────────────────────────────────

TaskHandle_t  pirTaskHandle    = NULL;
TaskHandle_t  cameraTaskHandle = NULL;
QueueHandle_t servoQueue       = NULL;

// ─── Module-private globals ───────────────────────────────────────────────────

static WebSocketsClient webSocket;
static SemaphoreHandle_t wsMutex         = NULL;
static uint32_t         frameSeq         = 0;
static uint32_t         lastHeartbeatMs  = 0;

static bool wsSendTXT(const char* text) {
    if (!wsConnected || wsMutex == NULL) return false;
    if (xSemaphoreTake(wsMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        bool ok = webSocket.sendTXT(text);
        xSemaphoreGive(wsMutex);
        return ok;
    }
    return false;
}

static bool wsSendBIN(const uint8_t* data, size_t len) {
    if (!wsConnected || wsMutex == NULL) return false;
    if (xSemaphoreTake(wsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        bool ok = webSocket.sendBIN(data, len);
        xSemaphoreGive(wsMutex);
        return ok;
    }
    return false;
}

// Forward declaration — defined later in this file
static void sendStatusMsg(const char* state, const char* message);

// ─── PIR interrupt service routine ───────────────────────────────────────────
// IRAM_ATTR: lives in fast internal RAM — never affected by flash cache stalls
// during WiFi operations. Runs in microseconds.
// Declared non-static so wildlife_cam.ino can pass it to attachInterrupt.

void IRAM_ATTR onPirRise() {
    lastPirTriggerMs = millis();
    pirTriggered     = true;

    if (pirTaskHandle == NULL) return;

    BaseType_t higherPriorityWoken = pdFALSE;
    xTaskNotifyFromISR(pirTaskHandle, 0, eNoAction, &higherPriorityWoken);
    portYIELD_FROM_ISR(higherPriorityWoken);
}

// ─── WebSocket event callback ─────────────────────────────────────────────────
// Called by wsManageTask during webSocket.loop().
// Runs in wsManageTask context — safe to use xQueueSend here.

static void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {

        case WStype_CONNECTED:
            wsConnected = true;
            Serial.printf("[WS] Connected to %s\n", WS_HOST);
            // NOTE: do NOT call blinkFlash() here — vTaskDelay is unsafe
            // in the WebSocket callback context. Blink is done in pirTask instead.
            {
                char msg[128];
                snprintf(msg, sizeof(msg),
                    "{\"type\":\"status\",\"state\":\"BOOT\","
                    "\"message\":\"Connected\","
                    "\"camera_id\":\"%s\","
                    "\"uptime_ms\":%lu,"
                    "\"wifi_rssi\":%d,"
                    "\"free_heap\":%lu}",
                    CAMERA_ID, millis(), WiFi.RSSI(), ESP.getFreeHeap());
                webSocket.sendTXT(msg);

                char resetMsg[96];
                snprintf(resetMsg, sizeof(resetMsg), "{\"type\":\"reset\",\"camera_id\":\"%s\"}", CAMERA_ID);
                webSocket.sendTXT(resetMsg);
            }
            break;

        case WStype_DISCONNECTED:
            wsConnected = false;
            Serial.println("[WS] Disconnected");
            break;

        case WStype_TEXT:
            if (length > 0) {
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, payload, length);
                if (err) {
                    Serial.printf("[WS] JSON parse error: %s\n", err.c_str());
                    break;
                }

                // Check message type (dashboard simulation or keepalive ping)
                const char* msgType = doc["type"].as<const char*>();
                if (msgType != NULL) {
                    if (strcmp(msgType, "ping") == 0) {
                        // Backend keepalive ping — silently ignore (not a servo command)
                        break;
                    }

                    if (strcmp(msgType, "simulate_pir") == 0) {
                        Serial.println("[SIM] PIR simulated from dashboard");
                        lastPirTriggerMs = millis();
                        pirTriggered     = true;
                        if (pirTaskHandle != NULL) {
                            xTaskNotify(pirTaskHandle, 0, eNoAction);
                        }
                        break;
                    }

                    if (strcmp(msgType, "simulate_idle") == 0) {
                        Serial.println("[SIM] Forced IDLE from dashboard");
                        deviceState = STATE_IDLE;
                        sendStatusMsg("IDLE", "Forced IDLE from dashboard");
                        break;
                    }
                }

                // Normal servo/action command — only parse if pan or tilt exists
                if (doc["pan"].is<float>() || doc["tilt"].is<float>()) {
                    ServoCommand_t cmd;
                    cmd.pan   = doc["pan"].as<float>();
                    cmd.tilt  = doc["tilt"].as<float>();
                    cmd.skip  = doc["skip"].as<bool>();
                    const char* action = doc["action"].as<const char*>();
                    if (action != NULL) {
                        strncpy(cmd.action, action, sizeof(cmd.action) - 1);
                        cmd.action[sizeof(cmd.action) - 1] = '\0';
                    } else {
                        strcpy(cmd.action, "none");
                    }

                    // Put into queue — servoTask will pick it up
                    // 0 timeout: if queue is full, drop the command (never block wsManageTask)
                    xQueueSend(servoQueue, &cmd, 0);
                }
            }
            break;

        case WStype_PING:
        case WStype_PONG:
            // Handled automatically by the library
            break;

        case WStype_ERROR:
            Serial.println("[WS] Error");
            break;

        default:
            break;
    }
}

// ─── Camera init ──────────────────────────────────────────────────────────────

bool initCamera() {
    Serial.println("[CAM] Initialising...");

    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;   // sccb (not sscb) for newer esp32-camera
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = CAM_XCLK_FREQ;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode    = CAMERA_GRAB_LATEST;
    config.jpeg_buffer_size = 64 * 1024;

    if (psramFound()) {
        Serial.println("[CAM] PSRAM found — VGA, 2 buffers");
        config.frame_size   = FRAMESIZE_VGA;
        config.jpeg_quality = CAM_QUALITY_PSRAM;
        config.fb_count     = 2;
        config.fb_location  = CAMERA_FB_IN_PSRAM;
    } else {
        Serial.println("[CAM] No PSRAM — QVGA, 1 buffer");
        config.frame_size   = FRAMESIZE_QVGA;
        config.jpeg_quality = CAM_QUALITY_DRAM;
        config.fb_count     = 1;
        config.fb_location  = CAMERA_FB_IN_DRAM;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM] Init FAILED: 0x%x\n", err);
        return false;
    }

    // Post-init sensor tuning
    sensor_t* s = esp_camera_sensor_get();
    if (s != NULL) {
        s->set_framesize(s, config.frame_size);
        s->set_quality(s, config.jpeg_quality);
        // Flip correction if image appears upside down — uncomment if needed:
        // s->set_vflip(s, 1);
        // s->set_hmirror(s, 1);
    }

    Serial.println("[CAM] Ready");
    return true;
}

// ─── WebSocket init ───────────────────────────────────────────────────────────

void initWebSocket() {
    if (wsMutex == NULL) {
        wsMutex = xSemaphoreCreateMutex();
    }
    webSocket.begin(WS_HOST, WS_PORT, WS_PATH);
    webSocket.onEvent(onWebSocketEvent);
    webSocket.setReconnectInterval(5000);   // retry every 5s if disconnected
    webSocket.enableHeartbeat(15000, 3000, 2);  // ping every 15s, timeout 3s, 2 retries
    // Pass API key as a header
    webSocket.setExtraHeaders("x-api-key: " API_KEY);
}

// ─── LED utility ──────────────────────────────────────────────────────────────
// Blocking — only call from tasks where brief blocking is acceptable
// (pirTask, setup). Never call from cameraTask during active streaming.

void blinkFlash(int times, int on_ms, int off_ms) {
    for (int i = 0; i < times; i++) {
        digitalWrite(FLASH_LED_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        digitalWrite(FLASH_LED_PIN, LOW);
        if (i < times - 1 && off_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(off_ms));
        }
    }
}

void blinkStatus(int times, int on_ms, int off_ms) {
    for (int i = 0; i < times; i++) {
        digitalWrite(STATUS_LED_PIN, LOW);   // Active LOW: LOW = ON
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        digitalWrite(STATUS_LED_PIN, HIGH);  // HIGH = OFF
        if (i < times - 1 && off_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(off_ms));
        }
    }
}

// ─── Helper: build and send JSON status message ───────────────────────────────

static void sendStatusMsg(const char* state, const char* message) {
    if (!wsConnected) return;
    char msg[192];
    snprintf(msg, sizeof(msg),
        "{\"type\":\"status\",\"state\":\"%s\","
        "\"message\":\"%s\","
        "\"camera_id\":\"%s\","
        "\"uptime_ms\":%lu,"
        "\"wifi_rssi\":%d,"
        "\"free_heap\":%lu}",
        state, message, CAMERA_ID,
        millis(), WiFi.RSSI(), ESP.getFreeHeap());
    wsSendTXT(msg);
}

// ─── pirTask ──────────────────────────────────────────────────────────────────
// Priority 5 — highest user task.
// Sleeps in xTaskNotifyWait most of the time.
// Woken by ISR (PIR fires) or by 100ms timer (to check timeout).

void pirTask(void* parameter) {
    Serial.println("[PIR] Task started");

    while (true) {
        // Block up to PIR_CHECK_INTERVAL_MS, or until notified by ISR/simulate
        xTaskNotifyWait(
            0, 0, NULL,
            pdMS_TO_TICKS(PIR_CHECK_INTERVAL_MS)
        );

        if (deviceState == STATE_IDLE) {
            // Wake if rising interrupt fired OR if pin is currently HIGH
            if (pirTriggered || digitalRead(PIR_PIN) == HIGH) {
                pirTriggered = false;
                lastPirTriggerMs = millis();
                deviceState  = STATE_ACTIVE;
                Serial.println("[PIR] Motion → ACTIVE");
                sendStatusMsg("ACTIVE", "Motion detected - streaming");
                blinkStatus(2, 50, 50);
                // Wake cameraTask — sleeping in xTaskNotifyWait(portMAX_DELAY)
                if (cameraTaskHandle != NULL) {
                    xTaskNotify(cameraTaskHandle, 0, eNoAction);
                }
            }
        } else {
            // STATE_ACTIVE: refresh timer if motion is ongoing (pin HIGH or interrupt)
            if (pirTriggered || digitalRead(PIR_PIN) == HIGH) {
                pirTriggered = false;
                lastPirTriggerMs = millis();
            }

            // Check 30s timeout
            uint32_t elapsed = millis() - lastPirTriggerMs;
            if (elapsed >= PIR_IDLE_TIMEOUT_MS) {
                deviceState = STATE_IDLE;
                pirTriggered = false;
                Serial.println("[PIR] Timeout → IDLE");

                // Center Pan on IDLE
                ServoCommand_t cmd = {0.0f, 0.0f, false, "none", false};
                xQueueSend(servoQueue, &cmd, 0);

                // Reset backend state
                char resetMsg[96];
                snprintf(resetMsg, sizeof(resetMsg), "{\"type\":\"reset\",\"camera_id\":\"%s\"}", CAMERA_ID);
                wsSendTXT(resetMsg);

                sendStatusMsg("IDLE", "PIR timeout - back to idle");
                blinkStatus(1, 50, 0);
                // cameraTask sees STATE_IDLE on next loop iteration and stops
            }
        }
    }
    vTaskDelete(NULL);
}

// ─── cameraTask ───────────────────────────────────────────────────────────────
// Priority 4.
// Sleeps when IDLE. Streams frames when ACTIVE.
// Never blocks on network — sendBIN returns quickly (lwIP buffers internally).

void cameraTask(void* parameter) {
    Serial.println("[CAM] Task started");

    while (true) {
        // Sleep here until pirTask notifies us (PIR triggered)
        xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);

        Serial.println("[CAM] Streaming started");

        // Drain any stale/overflow frame buffer accumulated while IDLE
        camera_fb_t* stale = esp_camera_fb_get();
        if (stale) {
            esp_camera_fb_return(stale);
        }

        // Stream until pirTask sets STATE_IDLE
        while (deviceState == STATE_ACTIVE) {

            if (!wsConnected) {
                // WebSocket not ready — wait briefly, pirTask keeps timeout fresh
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            camera_fb_t* fb = esp_camera_fb_get();
            if (!fb) {
                Serial.println("[CAM] Frame capture failed");
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            // Build metadata JSON — stack allocated, no heap
            char meta[128];
            snprintf(meta, sizeof(meta),
                "{\"type\":\"frame\","
                "\"seq\":%lu,"
                "\"pan\":%.2f,"
                "\"tilt\":0.00,"
                "\"w\":%u,"
                "\"h\":%u}",
                (unsigned long)frameSeq,
                (float)currentPan,
                fb->width,
                fb->height);

            // Send metadata text frame — tells backend what follows
            wsSendTXT(meta);

            // Send raw JPEG bytes — fb->buf directly, no malloc, no memcpy
            // lwIP copies in ~1460-byte chunks internally as it transmits
            wsSendBIN(fb->buf, fb->len);

            Serial.printf("[CAM] Frame #%lu — %u bytes\n", (unsigned long)frameSeq, fb->len);
            frameSeq++;

            // Return buffer to DMA pool — DMA can now overwrite it with next frame
            esp_camera_fb_return(fb);

            // Yield ~80ms (targets ~10-12 FPS). Gives lwIP time to flush TCP packet
            // and lets wsManageTask run to process incoming servo commands
            vTaskDelay(pdMS_TO_TICKS(80));
        }

        // STATE_IDLE — send heartbeat periodically while sleeping
        Serial.println("[CAM] Streaming stopped — IDLE");

        // Reset heartbeat timer when going idle
        lastHeartbeatMs = millis();
    }
    // Never reached
    vTaskDelete(NULL);
}

// ─── wsManageTask ─────────────────────────────────────────────────────────────
// Priority 2 — lowest active task, runs in cameraTask's vTaskDelay windows.
// Drives the WebSocket library: handles incoming data, pings, reconnection.
// When incoming data arrives, library calls onWebSocketEvent → handleCommand
// → xQueueSend(servoQueue) → servoTask wakes.

void wsManageTask(void* parameter) {
    Serial.println("[WS] Manage task started");

    // Send IDLE heartbeat every 10s when not streaming
    while (true) {
        if (wsMutex != NULL && xSemaphoreTake(wsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            webSocket.loop();   // check socket, call callbacks, handle pings
            xSemaphoreGive(wsMutex);
        }

        // Heartbeat while IDLE
        if (deviceState == STATE_IDLE) {
            uint32_t now = millis();
            if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
                lastHeartbeatMs = now;
                sendStatusMsg("IDLE", "Heartbeat - waiting for motion");
                blinkStatus(1, 30, 0);
            }
        }

        // Sleep 15ms — cameraTask runs freely for the rest of its time slice
        vTaskDelay(pdMS_TO_TICKS(WS_LOOP_INTERVAL_MS));
    }
    // Never reached
    vTaskDelete(NULL);
}

// ─── servoTask ────────────────────────────────────────────────────────────────
// Priority 3.
// Sleeps in xQueueReceive until wsManageTask delivers a command.
// Executes servo PWM and deterrent actions.
// Servo hardware: SG90, 50Hz PWM, pulse 500us (0°) to 2500us (180°).
// We map our -90 to +90 degree range to 500us to 2500us.

static uint32_t degreesToDuty(float degrees) {
    // SG90 pulse: 500us = -90deg, 1500us = 0deg, 2500us = +90deg
    // At 50Hz (20ms period), 16-bit resolution (65535 counts):
    // 1 count = 20ms/65535 ≈ 0.305us
    // 500us  = 500  / 0.305 ≈ 1638 counts
    // 2500us = 2500 / 0.305 ≈ 8192 counts
    float us = 1500.0f + (degrees * (1000.0f / 90.0f));
    us = constrain(us, 500.0f, 2500.0f);
    return (uint32_t)((us / 20000.0f) * 65535.0f);
}

// ─── DFPlayer Mini UART command helper ─────────────────────────────────────────
// Sends 10-byte standard checksummed packet to DFPlayer over Serial2 (GPIO 14)
static void dfSendCmd(uint8_t cmd, uint8_t param1, uint8_t param2) {
    uint16_t checksum = 0 - (0xFF + 0x06 + cmd + 0x00 + param1 + param2);
    uint8_t packet[10] = {
        0x7E, 0xFF, 0x06, cmd, 0x00, param1, param2,
        (uint8_t)(checksum >> 8), (uint8_t)(checksum & 0xFF),
        0xEF
    };
    Serial2.write(packet, 10);
}

void servoTask(void* parameter) {
    Serial.println("[SERVO] Task started");

    // Attach LEDC PWM channel for Pan servo (Tilt disabled in V1)
    ledcAttach(PAN_SERVO_PIN, 50, 16);

    // Configure relay and DFPlayer pins (Phase 2 hardware)
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);   // relay off by default

    // Center Pan at boot (no sweeps)
    ledcWrite(PAN_SERVO_PIN, degreesToDuty(0.0f));
    currentPan  = 0.0f;
    currentTilt = 0.0f;
    Serial.println("[SERVO] Pan centred at 0.0° (V1 Pan-only mode)");

    // Initialize DFPlayer Mini UART on GPIO 14 (TX only, baud 9600)
    // Direct 3.3V connection — no 1k resistor needed with ESP32!
    Serial2.begin(9600, SERIAL_8N1, -1, 14);
    vTaskDelay(pdMS_TO_TICKS(1000)); // wait 1s for DFPlayer + SD card boot

    dfSendCmd(0x06, 0, 30);          // Set volume to 30 (max)
    vTaskDelay(pdMS_TO_TICKS(200));

    // ── Boot sound test — plays all 5 tracks once in order ───────────────────
    // Track 1: SOUND_HUMAN_SHOUT   (apex predators: tiger, leopard)
    // Track 2: SOUND_DOG_BARK      (bear, boar, fox, raccoon dog, leopard cat)
    // Track 3: SOUND_WOLF_HOWL     (deer: sika, roe, musk)
    // Track 4: SOUND_RAPTOR_SCREECH (small mustelids + hare)
    // Track 5: SOUND_FRIENDLY_VOICE (dog, cow)
    const uint8_t TOTAL_SOUNDS = 5;
    const uint16_t SOUND_GAP_MS = 2000; // 2s gap between tracks

    Serial.println("[SERVO] DFPlayer boot test — playing all sounds...");
    for (uint8_t track = 1; track <= TOTAL_SOUNDS; track++) {
        Serial.printf("[SERVO] Boot test track %d/%d\n", track, TOTAL_SOUNDS);
        dfSendCmd(0x12, 0, track);
        vTaskDelay(pdMS_TO_TICKS(SOUND_GAP_MS));
    }
    Serial.println("[SERVO] Boot sound test complete");

    while (true) {
        ServoCommand_t cmd;

        // Block here forever until wsManageTask puts a command in the queue
        // portMAX_DELAY = sleep until item arrives, zero CPU used
        if (xQueueReceive(servoQueue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Update tracked position
        currentPan  = cmd.pan;
        currentTilt = 0.0f;

        if (!cmd.skip) {
            ledcWrite(PAN_SERVO_PIN, degreesToDuty(cmd.pan));
            Serial.printf("[SERVO] pan=%.1f\n", cmd.pan);
        }

        // ── Deterrent Sound Actions (Mapped to SD card /mp3/000X.mp3) ────────
        if (strcmp(cmd.action, "SOUND_HUMAN_SHOUT") == 0) {
            dfSendCmd(0x12, 0, 1); // Track 1: 0001.mp3
            Serial.println("[SERVO] Sound: SOUND_HUMAN_SHOUT -> Track 1");

        } else if (strcmp(cmd.action, "SOUND_DOG_BARK") == 0 || strcmp(cmd.action, "BARK_SOUND") == 0) {
            dfSendCmd(0x12, 0, 2); // Track 2: 0002.mp3
            Serial.println("[SERVO] Sound: SOUND_DOG_BARK -> Track 2");

        } else if (strcmp(cmd.action, "SOUND_WOLF_HOWL") == 0) {
            dfSendCmd(0x12, 0, 3); // Track 3: 0003.mp3
            Serial.println("[SERVO] Sound: SOUND_WOLF_HOWL -> Track 3");

        } else if (strcmp(cmd.action, "SOUND_RAPTOR_SCREECH") == 0) {
            dfSendCmd(0x12, 0, 4); // Track 4: 0004.mp3
            Serial.println("[SERVO] Sound: SOUND_RAPTOR_SCREECH -> Track 4");

        } else if (strcmp(cmd.action, "SOUND_FRIENDLY_VOICE") == 0) {
            dfSendCmd(0x12, 0, 5); // Track 5: 0005.mp3
            Serial.println("[SERVO] Sound: SOUND_FRIENDLY_VOICE -> Track 5");

        } else if (strcmp(cmd.action, "ALERT_OWNER") == 0) {
            Serial.println("[SERVO] Action: ALERT_OWNER");
        }

        // ── Visual Deterrent (Spotlight Relay) ────────────────────────────────
        // Triggers if primary action is FLASH_LIGHT OR if in secondary_actions
        if (cmd.flash || strcmp(cmd.action, "FLASH_LIGHT") == 0) {
            digitalWrite(RELAY_PIN, HIGH);
            vTaskDelay(pdMS_TO_TICKS(500));
            digitalWrite(RELAY_PIN, LOW);
            Serial.println("[SERVO] Action: FLASH_LIGHT (Spotlight)");
        }
        // "none" and "CONTINUE_SWEEP" require no hardware action
    }
    // Never reached
    vTaskDelete(NULL);
}
