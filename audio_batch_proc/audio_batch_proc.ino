//
//  audio_batch_proc.ino  —  ESP32 Audio Batch Processor
//
//  Captures PCM audio from an INMP441 digital microphone via I2S/DMA,
//  then streams it as raw binary over a WebSocket tunnel to a remote
//  server.  A FreeRTOS event group drives a simple state machine that
//  handles WiFi provisioning, WebSocket connection, and error recovery.
//
//  Core layout:
//    Core 0 — Left completely free (reserved for future audio reception)
//    Core 1 — audioTask (I2S/DMA capture) + wsSendTask (WebSocket tx)
//
//  Dependencies (Arduino IDE / PlatformIO):
//    - Built‑in:  WiFi.h, driver/i2s.h, freertos/*
//    - Built‑in:  esp_websocket_client.h  (ESP‑IDF, accessible from Arduino)
//

#include <Arduino.h>
#include <WiFi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "audio_mic.h"
#include "websocket.h"

// ================================================================
//  User‑configurable settings
// ================================================================

// ---- WiFi credentials (hardcoded — WPA2‑PSK) ----
#define WIFI_SSID     "LINA SOFI"
#define WIFI_PASSWORD "Lina0429"

// ---- Remote WebSocket server ----
// Must be a ws:// URI (wss:// for TLS, but plain ws:// is used here).
// The server must accept raw binary WebSocket frames on this path.
#define WS_SERVER_URL "ws://192.168.1.7:8080/audio"

// ================================================================
//  FreeRTOS Event Group bits
// ================================================================
#define WIFI_CONNECTED_BIT    (1 << 0)
#define WIFI_DISCONNECTED_BIT (1 << 1)
// WS_CONNECTED_BIT is defined in websocket.h as (1 << 2)

// ================================================================
//  System state enumeration
// ================================================================
typedef enum {
    STATE_IDLE,
    STATE_WIFI_CONNECTING,
    STATE_WS_CONNECTING,
    STATE_STREAMING,
    STATE_DISCONNECTED
} sys_state_t;

// ================================================================
//  Globals
// ================================================================
static EventGroupHandle_t sysEventGroup = NULL;
static sys_state_t        state         = STATE_IDLE;

// ================================================================
//  WiFi event handler (updates the FreeRTOS event group)
// ================================================================
//
// The Arduino WiFi subsystem fires callbacks on the Arduino event
// loop (Core 0 by default).  This handler simply translates the
// Arduino‑layer events into FreeRTOS event‑group bits so that the
// state machine (running in loop()) and other tasks can react.
//
static void wifiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info)
{
    switch (event) {

        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            // An IPv4 address has been obtained →
            // signal that WiFi is operational.
            Serial.printf("[WiFi] Connected — IP: %s\n",
                          WiFi.localIP().toString().c_str());
            xEventGroupSetBits(sysEventGroup, WIFI_CONNECTED_BIT);
            xEventGroupClearBits(sysEventGroup, WIFI_DISCONNECTED_BIT);
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            // Link‑layer disconnection (AP lost, out of range, etc.)
            // Clear the connected bit and set the disconnected bit.
            Serial.println("[WiFi] Disconnected");
            xEventGroupSetBits(sysEventGroup, WIFI_DISCONNECTED_BIT);
            xEventGroupClearBits(sysEventGroup, WIFI_CONNECTED_BIT);
            break;

        default:
            break;
    }
}

// ================================================================
//  setup()  —  one‑time initialisation
// ================================================================
void setup()
{
    Serial.begin(115200);
    delay(1000);   // Let the USB/UART peripheral settle
    Serial.println("\n=== ESP32 Audio Batch Processor ===");

    // ---------- Create PCM queue (before any task starts) ----------
    pcmQueue = xQueueCreate(PCM_QUEUE_LENGTH, sizeof(pcm_block_t));
    if (pcmQueue == NULL) {
        Serial.println("[FATAL] Queue creation failed — halting");
        while (1) { delay(1000); }
    }

    // ---------- Create FreeRTOS event group ----------
    sysEventGroup = xEventGroupCreate();
    if (sysEventGroup == NULL) {
        Serial.println("[FATAL] Event group creation failed — halting");
        while (1) { delay(1000); }
    }
    xEventGroupSetBits(sysEventGroup, WIFI_DISCONNECTED_BIT);

    // ---------- Register WiFi event handler ----------
    WiFi.onEvent(wifiEventHandler);

    // ---------- Start WiFi in Station mode ----------
    // The physical Wi‑Fi driver is started and the STA begins
    // scanning/associating with the configured AP.
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[WiFi] Connecting to %s …\n", WIFI_SSID);

    state = STATE_WIFI_CONNECTING;
}

// ================================================================
//  loop()  —  simple state machine
// ================================================================
//
//  The loop() runs on Core 0 (Arduino task).  It polls the event
//  group to decide when to transition between states.
//
void loop()
{
    EventBits_t bits;

    switch (state) {

        // ----- Wait for WiFi association -----
        case STATE_WIFI_CONNECTING: {
            bits = xEventGroupWaitBits(sysEventGroup,
                                       WIFI_CONNECTED_BIT,
                                       pdFALSE,           // Don't clear
                                       pdTRUE,            // Wait for all
                                       pdMS_TO_TICKS(500));

            if (bits & WIFI_CONNECTED_BIT) {
                state = STATE_WS_CONNECTING;
                Serial.println("[State] WiFi up → connecting WebSocket");
            }
            break;
        }

        // ----- Bring up WebSocket client & wait for handshake -----
        case STATE_WS_CONNECTING: {
            // Start the WebSocket client (creates wsSendTask internally)
            wsInit(WS_SERVER_URL, sysEventGroup);

            // Wait up to 15 s for the WebSocket handshake to complete
            bits = xEventGroupWaitBits(sysEventGroup,
                                       WS_CONNECTED_BIT,
                                       pdFALSE,
                                       pdTRUE,
                                       pdMS_TO_TICKS(15000));

            if (bits & WS_CONNECTED_BIT) {
                // WebSocket tunnel is ready → spin up audio capture
                micInit();

                xTaskCreatePinnedToCore(
                    audioTask,
                    "audioTask",
                    8192,               // Stack size (bytes)
                    &pcmQueue,          // Parameter — pointer to queue handle
                    2,                  // Priority (same as wsSendTask)
                    &audioTaskHandle,   // Store handle for cleanup
                    1                   // Core 1 — leave Core 0 free
                );

                state = STATE_STREAMING;
                Serial.println("[State] Streaming audio → server");
            } else {
                // Handshake timed out — tear down and retry
                Serial.println("[State] WS handshake timeout — retrying");
                wsDeinit();
                delay(5000);
            }
            break;
        }

        // ----- Normal operation: monitor link health -----
        case STATE_STREAMING: {
            // Poll the event group (non‑blocking)
            bits = xEventGroupGetBits(sysEventGroup);

            // Only WiFi loss triggers a full restart.
            // The WebSocket may drop and auto‑reconnect; during that
            // window the sender task simply drains & discards blocks
            // so the DMA pipeline never stalls.
            if (bits & WIFI_DISCONNECTED_BIT) {
                state = STATE_DISCONNECTED;
                Serial.println("[State] WiFi lost → restarting");
            }

            delay(2000);   // Poll period
            break;
        }

        // ----- Clean‑up and retry -----
        case STATE_DISCONNECTED: {
            Serial.println("[State] Tearing down subsystems …");

            // 1. Stop audio capture & uninstall I2S driver
            micDeinit();

            // 2. Delete WebSocket sender and stop client
            wsDeinit();

            // 3. Destroy and recreate the PCM queue
            if (pcmQueue != NULL) {
                vQueueDelete(pcmQueue);
                pcmQueue = NULL;
            }
            pcmQueue = xQueueCreate(PCM_QUEUE_LENGTH, sizeof(pcm_block_t));

            // 4. Ask the WiFi stack to re‑associate
            WiFi.reconnect();

            state = STATE_WIFI_CONNECTING;
            break;
        }

        default:
            break;
    }
}
