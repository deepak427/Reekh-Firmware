// wildlife_cam.ino — AI Scarecrow firmware
//
// Architecture:
//   setup()  — init hardware, WiFi, WebSocket, create FreeRTOS tasks
//   loop()   — empty (delay 10s), all logic lives in tasks
//
// Tasks (all on Core 1, Core 0 reserved for WiFi/lwIP SDK):
//   pirTask      (priority 5) — hardware interrupt → IDLE/ACTIVE state machine
//   cameraTask   (priority 4) — streams frames over WebSocket when ACTIVE
//   servoTask    (priority 3) — receives servo/action commands, drives hardware
//   wsManageTask (priority 2) — runs webSocket.loop(), sends heartbeats
//
// Libraries required (install via Arduino Library Manager):
//   - arduinoWebSockets  by Markus Sattler  (search: "WebSockets")
//   - ArduinoJson        by Benoit Blanchon (search: "ArduinoJson", install v6)
//
// Board: AI-Thinker ESP32-CAM
// Partition scheme: "Huge APP (3MB No OTA)" — required for WebSocket + camera

#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>

#include "camera_config.h"
#include "ws_client.h"

// ─── setup ────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(false);   // Disable ESP-IDF driver debug logs (silences FB-OVF spam)
    delay(300);
    Serial.println("\n[BOOT] Wildlife camera starting...");

    // Diagnostic: print exact reset cause
    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr = "OTHER";
    switch (reason) {
        case ESP_RST_POWERON:   reasonStr = "POWER_ON"; break;
        case ESP_RST_SW:        reasonStr = "SOFTWARE_RESET"; break;
        case ESP_RST_PANIC:     reasonStr = "CRASH_PANIC"; break;
        case ESP_RST_INT_WDT:   reasonStr = "INT_WATCHDOG"; break;
        case ESP_RST_TASK_WDT:  reasonStr = "TASK_WATCHDOG"; break;
        case ESP_RST_WDT:       reasonStr = "WATCHDOG"; break;
        case ESP_RST_BROWNOUT:  reasonStr = "BROWNOUT (Voltage drop on USB!)"; break;
        default: break;
    }
    Serial.printf("[BOOT] Reset reason: %s (%d)\n", reasonStr, (int)reason);

    // ── Pin setup ────────────────────────────────────────────────────────────
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, HIGH);  // Red LED off (active LOW)
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, LOW);   // High-power flash LED off
    pinMode(PIR_PIN, INPUT_PULLDOWN);   // pull-down so floating pin reads LOW

    // ── Camera init ──────────────────────────────────────────────────────────
    if (!initCamera()) {
        Serial.println("[BOOT] Camera init failed — halting");
        // Blink SOS pattern and restart
        while (true) {
            for (int i = 0; i < 3; i++) { blinkStatus(1, 150, 150); }
            for (int i = 0; i < 3; i++) { blinkStatus(1, 400, 150); }
            for (int i = 0; i < 3; i++) { blinkStatus(1, 150, 150); }
            delay(1000);
        }
    }

    // ── WiFi ─────────────────────────────────────────────────────────────────
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_17dBm);  // Reduce current spikes by ~80mA, prevents brownouts
    WiFi.setSleep(false);   // disable modem sleep — consistent low-latency WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (millis() - wifiStart > 20000) {
            Serial.println("\n[WiFi] Failed — restarting");
            ESP.restart();
        }
    }
    Serial.printf("\n[WiFi] Connected — IP: %s  RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());

    // ── WebSocket client init ─────────────────────────────────────────────────
    // Does not connect here — wsManageTask drives webSocket.loop() which
    // handles the actual connection and reconnection automatically.
    initWebSocket();

    // ── FreeRTOS queue for servo commands ─────────────────────────────────────
    // Depth 4: wsManageTask can queue up to 4 commands before servoTask drains them
    servoQueue = xQueueCreate(4, sizeof(ServoCommand_t));
    if (servoQueue == NULL) {
        Serial.println("[BOOT] Queue create failed — restarting");
        ESP.restart();
    }

    // ── Create tasks ──────────────────────────────────────────────────────────
    // pirTask must be created first — its handle is used by the ISR.

    BaseType_t ret;

    ret = xTaskCreatePinnedToCore(
        pirTask, "PirTask",
        PIR_TASK_STACK, NULL, PIR_TASK_PRIORITY,
        &pirTaskHandle, TASK_CORE
    );
    if (ret != pdPASS) {
        Serial.println("[BOOT] FATAL: pirTask create failed");
        ESP.restart();
    }

    // Attach PIR interrupt AFTER pirTaskHandle is valid.
    attachInterrupt(digitalPinToInterrupt(PIR_PIN), onPirRise, RISING);

    ret = xTaskCreatePinnedToCore(
        cameraTask, "CameraTask",
        CAM_TASK_STACK, NULL, CAM_TASK_PRIORITY,
        &cameraTaskHandle, TASK_CORE
    );
    if (ret != pdPASS) {
        Serial.printf("[BOOT] FATAL: cameraTask create failed (free heap: %lu)\n", ESP.getFreeHeap());
        ESP.restart();
    }

    ret = xTaskCreatePinnedToCore(
        servoTask, "ServoTask",
        SERVO_TASK_STACK, NULL, SERVO_TASK_PRIORITY,
        NULL, TASK_CORE
    );
    if (ret != pdPASS) {
        Serial.println("[BOOT] FATAL: servoTask create failed");
        ESP.restart();
    }

    ret = xTaskCreatePinnedToCore(
        wsManageTask, "WsManageTask",
        WS_TASK_STACK, NULL, WS_TASK_PRIORITY,
        NULL, TASK_CORE
    );
    if (ret != pdPASS) {
        Serial.println("[BOOT] FATAL: wsManageTask create failed");
        ESP.restart();
    }

    // Boot complete blink: 2 short pulses on safe red LED (zero brownout risk)
    blinkStatus(2, 60, 60);
    Serial.println("[BOOT] All tasks created — system running");
    Serial.printf("[BOOT] Free heap: %lu bytes\n", ESP.getFreeHeap());
}

// ─── loop ─────────────────────────────────────────────────────────────────────
// Intentionally empty. All logic is in FreeRTOS tasks above.
// This keeps Arduino's loopTask at the lowest priority so it never competes
// with our tasks for CPU time.

void loop() {
    delay(10000);
}
