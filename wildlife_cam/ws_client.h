#pragma once

#include <Arduino.h>
#include "camera_config.h"

// ─── Shared state (volatile — read by multiple tasks) ─────────────────────────

// Device state enum
typedef enum {
    STATE_IDLE = 0,
    STATE_ACTIVE
} DeviceState_t;

extern volatile DeviceState_t deviceState;
extern volatile uint32_t      lastPirTriggerMs;
extern volatile bool          pirTriggered;
extern volatile bool          wsConnected;

// Current servo angles (updated when backend sends command)
extern volatile float currentPan;
extern volatile float currentTilt;

// Task handles (needed for cross-task notifications)
extern TaskHandle_t pirTaskHandle;
extern TaskHandle_t cameraTaskHandle;

// Servo command queue handle
extern QueueHandle_t servoQueue;

// ─── Servo command struct ─────────────────────────────────────────────────────

typedef struct {
    float pan;
    float tilt;
    bool  skip;
    char  action[32];   // "none", "SOUND_HUMAN_SHOUT", "SOUND_DOG_BARK", etc.
    bool  flash;        // true if FLASH_LIGHT requested (primary or secondary)
} ServoCommand_t;

// ─── Public functions (called from wildlife_cam.ino setup) ───────────────────

// Initialise camera hardware. Returns true on success.
bool     initCamera();

// Initialise WebSocket client (does not connect — wsManageTask does that).
void     initWebSocket();

// PIR interrupt service routine — attach via attachInterrupt() in setup()
// after pirTaskHandle has been assigned.
void IRAM_ATTR onPirRise();

// FreeRTOS task functions — passed to xTaskCreatePinnedToCore
void     pirTask(void* parameter);
void     cameraTask(void* parameter);
void     servoTask(void* parameter);
void     wsManageTask(void* parameter);

// LED utility (used by multiple tasks)
void     blinkFlash(int times, int on_ms, int off_ms);
void     blinkStatus(int times, int on_ms, int off_ms);
