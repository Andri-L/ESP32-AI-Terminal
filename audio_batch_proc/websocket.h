#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

// ---- WebSocket connection event bits ----
// These must match the bit definitions in main.ino
#define WS_CONNECTED_BIT  (1 << 2)

// ---- Public API ----
void wsInit(const char *url, const char *token, EventGroupHandle_t evGroup);
void wsDeinit();

#endif
