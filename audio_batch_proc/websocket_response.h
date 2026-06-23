#ifndef WEBSOCKET_RESPONSE_H
#define WEBSOCKET_RESPONSE_H

#include <freertos/FreeRTOS.h>

// ---- Public API ----
void wsResponseInit(const char *url, const char *token);
void wsResponseDeinit();

#endif
