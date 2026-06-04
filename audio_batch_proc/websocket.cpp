#include "websocket.h"
#include "audio_mic.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

// ================================================================
//  Static globals
// ================================================================
static WiFiClient      wsTcp;         // TCP socket for WebSocket
static EventGroupHandle_t wsEventGrp = NULL;
static TaskHandle_t       wsTaskHndl = NULL;
static volatile bool      wsRunning  = false;  // Shutdown flag

// Parsed server info
static char  wsHost[64] = {0};
static uint16_t wsPort  = 80;
static char  wsPath[128] = {0};

// Auth token (passed from main sketch)
static char  wsToken[96] = {0};

// ================================================================
//  URL parser  —  extracts host, port, path from a ws:// URI
// ================================================================
static void parseUrl(const char *url, char *host, size_t hostSize,
                     uint16_t *port, char *path, size_t pathSize)
{
    const char *p = url;
    if (strncmp(p, "ws://", 5) == 0) p += 5;

    // Host
    const char *hs = p;
    while (*p && *p != ':' && *p != '/') p++;
    size_t hLen = p - hs;
    if (hLen >= hostSize) hLen = hostSize - 1;
    memcpy(host, hs, hLen);
    host[hLen] = '\0';

    // Port
    if (*p == ':') {
        p++;
        *port = (uint16_t)atoi(p);
        while (*p >= '0' && *p <= '9') p++;
    } else {
        *port = 80;
    }

    // Path
    const char *ps = *p ? p : "/";
    size_t pLen = strlen(ps);
    if (pLen >= pathSize) pLen = pathSize - 1;
    memcpy(path, ps, pLen);
    path[pLen] = '\0';
}

// ================================================================
//  Base64 encoder  —  encodes 16 raw bytes into 24 chars + null
//  (RFC 6455 Sec‑WebSocket‑Key is exactly 16 bytes raw → 24 char)
// ================================================================
static void base64Encode16(const uint8_t src[16], char dst[25])
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    uint8_t  buf[18] = {0};
    memcpy(buf, src, 16);

    dst[ 0] = T[ buf[ 0] >> 2 ];
    dst[ 1] = T[((buf[ 0] & 0x03) << 4) | (buf[ 1] >> 4)];
    dst[ 2] = T[((buf[ 1] & 0x0F) << 2) | (buf[ 2] >> 6)];
    dst[ 3] = T[ buf[ 2] & 0x3F ];

    dst[ 4] = T[ buf[ 3] >> 2 ];
    dst[ 5] = T[((buf[ 3] & 0x03) << 4) | (buf[ 4] >> 4)];
    dst[ 6] = T[((buf[ 4] & 0x0F) << 2) | (buf[ 5] >> 6)];
    dst[ 7] = T[ buf[ 5] & 0x3F ];

    dst[ 8] = T[ buf[ 6] >> 2 ];
    dst[ 9] = T[((buf[ 6] & 0x03) << 4) | (buf[ 7] >> 4)];
    dst[10] = T[((buf[ 7] & 0x0F) << 2) | (buf[ 8] >> 6)];
    dst[11] = T[ buf[ 8] & 0x3F ];

    dst[12] = T[ buf[ 9] >> 2 ];
    dst[13] = T[((buf[ 9] & 0x03) << 4) | (buf[10] >> 4)];
    dst[14] = T[((buf[10] & 0x0F) << 2) | (buf[11] >> 6)];
    dst[15] = T[ buf[11] & 0x3F ];

    dst[16] = T[ buf[12] >> 2 ];
    dst[17] = T[((buf[12] & 0x03) << 4) | (buf[13] >> 4)];
    dst[18] = T[((buf[13] & 0x0F) << 2) | (buf[14] >> 6)];
    dst[19] = T[ buf[14] & 0x3F ];

    dst[20] = T[ buf[15] >> 2 ];
    dst[21] = T[((buf[15] & 0x03) << 4) | 0 ];
    dst[22] = '=';
    dst[23] = '=';
    dst[24] = '\0';
}

// ================================================================
//  WebSocket handshake  (RFC 6455 section 4)
// ================================================================
static bool wsHandshake()
{
    // Generate 16 random bytes → base64 → Sec‑WebSocket‑Key
    uint8_t rnd[16];
    for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)random(256);
    char key[25];
    base64Encode16(rnd, key);

    // Build the HTTP Upgrade request (include Authorization if a token is set)
    wsTcp.printf(
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer %s\r\n"
        "\r\n",
        wsPath, wsHost, wsPort, key, wsToken
    );
    wsTcp.flush();

    // Read response headers until blank line (up to 5 s timeout)
    unsigned long deadline = millis() + 5000;
    String resp;
    while (millis() < deadline) {
        while (wsTcp.available()) {
            char c = (char)wsTcp.read();
            resp += c;

            size_t rl = resp.length();
            if (rl >= 4 &&
                resp[rl - 4] == '\r' && resp[rl - 3] == '\n' &&
                resp[rl - 2] == '\r' && resp[rl - 1] == '\n')
            {
                // Double CRLF found → headers complete
                if (resp.indexOf("101") >= 0) {
                    Serial.println("[WS] Handshake OK — 101 Switching Protocols");
                    return true;
                }
                Serial.printf("[WS] Handshake failed — response:\n%s\n", resp.c_str());
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    Serial.println("[WS] Handshake timeout");
    return false;
}

// ================================================================
//  Send a binary WebSocket frame  (RFC 6455, opcode 0x2, masked)
// ================================================================
//
//  Frame layout (client → server, MASK bit = 1):
//
//   Byte 0:  FIN=1, RSV=0, opcode=2 (binary)  →  0x82
//   Byte 1:  MASK=1 + payload‑len
//   [extended length: 2 or 8 bytes if len >= 126]
//   [masking key: 4 bytes]
//   [payload: len bytes, XOR'd with masking key]
//
static int wsSendBinary(uint8_t *data, size_t len)
{
    // ---- Build header ----
    uint8_t hdr[14];           // max header: 2 + 8 + 4 = 14 bytes
    size_t  hl = 0;

    // Byte 0: FIN + BINARY opcode
    hdr[hl++] = 0x82;

    // Byte 1 + extended length
    if (len < 126) {
        hdr[hl++] = (uint8_t)(0x80 | len);
    } else if (len <= 0xFFFF) {
        hdr[hl++] = (uint8_t)(0x80 | 126);
        hdr[hl++] = (uint8_t)((len >> 8) & 0xFF);
        hdr[hl++] = (uint8_t)(len & 0xFF);
    } else {
        hdr[hl++] = (uint8_t)(0x80 | 127);
        for (int i = 7; i >= 0; i--) {
            hdr[hl++] = (uint8_t)((len >> (i * 8)) & 0xFF);
        }
    }

    // ---- Masking key (4 random bytes) ----
    uint8_t mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)random(256);

    // ---- Send header + mask ----
    size_t written  = wsTcp.write(hdr, hl);
           written += wsTcp.write(mask, 4);

    // ---- Send masked payload (mask in‑place, buffer is ours) ----
    // The caller will free() the buffer right after we return, so
    // mutating it in‑place is safe.
    for (size_t i = 0; i < len; i++) {
        data[i] ^= mask[i & 3];
    }
    written += wsTcp.write(data, len);

    return (int)written;
}

// ================================================================
//  FreeRTOS task: drain PCM queue → send binary over WebSocket
// ================================================================
//
//  Pinned to Core 1 together with audioTask so that Core 0 stays
//  completely free for the Arduino event loop and future features.
//
static void wsSendTask(void *parameter)
{
    pcm_block_t block;

    Serial.printf("[WS] Sender task running on Core %d\n", xPortGetCoreID());

    while (wsRunning) {
        // Block until a PCM block arrives from the audioTask
        if (xQueueReceive(pcmQueue, &block, pdMS_TO_TICKS(200)) == pdTRUE) {

            // Check: are we still connected?
            if (wsTcp.connected()) {
                int sent = wsSendBinary(block.data, block.length);

                if (sent < 0) {
                    Serial.printf("[WS] Send failed (err=%d)\n", sent);
                    xEventGroupClearBits(wsEventGrp, WS_CONNECTED_BIT);
                    // Try to reconnect
                    wsTcp.stop();
                }
            } else {
                // TCP gone — clear connected flag, attempt reconnect
                xEventGroupClearBits(wsEventGrp, WS_CONNECTED_BIT);
            }

            // Free the audio buffer (ownership transferred via queue)
            free(block.data);

            // If we lost connection, attempt reconnect
            if (!wsTcp.connected()) {
                Serial.println("[WS] Reconnecting TCP…");
                if (wsTcp.connect(wsHost, wsPort)) {
                    if (wsHandshake()) {
                        xEventGroupSetBits(wsEventGrp, WS_CONNECTED_BIT);
                        Serial.println("[WS] Reconnected");
                    } else {
                        wsTcp.stop();
                        vTaskDelay(pdMS_TO_TICKS(3000));
                    }
                } else {
                    vTaskDelay(pdMS_TO_TICKS(3000));
                }
            }
        }
    }

    wsTcp.stop();
    vTaskDelete(NULL);
}

// ================================================================
//  wsInit  —  connect TCP, perform handshake, spawn sender task
// ================================================================
void wsInit(const char *url, const char *token, EventGroupHandle_t evGroup)
{
    wsEventGrp = evGroup;

    // Store auth token for handshake and reconnects
    if (token) {
        strncpy(wsToken, token, sizeof(wsToken) - 1);
        wsToken[sizeof(wsToken) - 1] = '\0';
    } else {
        wsToken[0] = '\0';
    }

    // Parse the ws:// URL
    parseUrl(url, wsHost, sizeof(wsHost), &wsPort, wsPath, sizeof(wsPath));
    Serial.printf("[WS] Target: %s:%u%s\n", wsHost, wsPort, wsPath);

    // Open TCP connection
    if (!wsTcp.connect(wsHost, wsPort)) {
        Serial.println("[WS] TCP connect failed");
        return;
    }
    Serial.printf("[WS] TCP connected to %s:%u\n", wsHost, wsPort);

    // WebSocket handshake
    if (!wsHandshake()) {
        wsTcp.stop();
        Serial.println("[WS] Handshake failed");
        return;
    }

    // Signal main loop that the tunnel is ready
    xEventGroupSetBits(wsEventGrp, WS_CONNECTED_BIT);

    // Launch sender task — pinned to Core 1
    wsRunning = true;
    xTaskCreatePinnedToCore(
        wsSendTask,
        "wsSender",
        8192,
        NULL,
        2,
        &wsTaskHndl,
        1                           // Core 1 — leave Core 0 free
    );

    Serial.println("[WS] Sender task launched");
}

// ================================================================
//  wsDeinit  —  tear down sender task and TCP connection
// ================================================================
void wsDeinit()
{
    wsRunning = false;

    if (wsTaskHndl != NULL) {
        vTaskDelete(wsTaskHndl);
        wsTaskHndl = NULL;
    }

    if (wsTcp.connected()) {
        wsTcp.stop();
    }

    xEventGroupClearBits(wsEventGrp, WS_CONNECTED_BIT);
    Serial.println("[WS] Deinitialised");
}
