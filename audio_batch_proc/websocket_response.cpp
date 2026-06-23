#include "websocket_response.h"
#include "audio_spk.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ================================================================
//  Static globals
// ================================================================
static WiFiClient      respTcp;
static TaskHandle_t    respTaskHndl = NULL;
static volatile bool   respRunning  = false;

static char  respHost[64]  = {0};
static uint16_t respPort   = 80;
static char  respPath[128] = {0};
static char  respToken[96] = {0};

// ================================================================
//  URL parser
// ================================================================
static void parseUrl(const char *url, char *host, size_t hostSize,
                     uint16_t *port, char *path, size_t pathSize)
{
    const char *p = url;
    if (strncmp(p, "ws://", 5) == 0) p += 5;

    const char *hs = p;
    while (*p && *p != ':' && *p != '/') p++;
    size_t hLen = p - hs;
    if (hLen >= hostSize) hLen = hostSize - 1;
    memcpy(host, hs, hLen);
    host[hLen] = '\0';

    if (*p == ':') {
        p++;
        *port = (uint16_t)atoi(p);
        while (*p >= '0' && *p <= '9') p++;
    } else {
        *port = 80;
    }

    const char *ps = *p ? p : "/";
    size_t pLen = strlen(ps);
    if (pLen >= pathSize) pLen = pathSize - 1;
    memcpy(path, ps, pLen);
    path[pLen] = '\0';
}

// ================================================================
//  Base64 encoder (16 bytes → 24 chars)
// ================================================================
static void base64Encode16(const uint8_t src[16], char dst[25])
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";
    uint8_t buf[18] = {0};
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
//  WebSocket handshake
// ================================================================
static bool wsResponseHandshake()
{
    uint8_t rnd[16];
    for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)random(256);
    char key[25];
    base64Encode16(rnd, key);

    respTcp.printf(
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer %s\r\n"
        "\r\n",
        respPath, respHost, respPort, key, respToken
    );
    respTcp.flush();

    unsigned long deadline = millis() + 5000;
    String resp;
    while (millis() < deadline) {
        while (respTcp.available()) {
            char c = (char)respTcp.read();
            resp += c;
            size_t rl = resp.length();
            if (rl >= 4 && resp[rl-4]=='\r' && resp[rl-3]=='\n' && resp[rl-2]=='\r' && resp[rl-1]=='\n') {
                if (resp.indexOf("101") >= 0) {
                    Serial.println("[WS/resp] Handshake OK");
                    return true;
                }
                Serial.printf("[WS/resp] Handshake failed:\n%s\n", resp.c_str());
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    Serial.println("[WS/resp] Handshake timeout");
    return false;
}

// ================================================================
//  Read a WebSocket frame (server → client, NOT masked)
// ================================================================
static bool wsReadFrame(uint8_t **outData, size_t *outLen)
{
    uint8_t hdr[2];
    if (respTcp.read(hdr, 2) != 2) return false;

    uint8_t opcode = hdr[0] & 0x0F;
    uint64_t payloadLen = hdr[1] & 0x7F;

    if (payloadLen == 126) {
        uint8_t ext[2];
        if (respTcp.read(ext, 2) != 2) return false;
        payloadLen = (ext[0] << 8) | ext[1];
    } else if (payloadLen == 127) {
        uint8_t ext[8];
        if (respTcp.read(ext, 8) != 8) return false;
        payloadLen = 0;
        for (int i = 0; i < 8; i++) payloadLen = (payloadLen << 8) | ext[i];
    }

    if (payloadLen == 0) {
        *outData = NULL;
        *outLen = 0;
        return true;
    }

    uint8_t *data = (uint8_t *)malloc(payloadLen);
    if (!data) return false;

    size_t read = 0;
    while (read < payloadLen) {
        int r = respTcp.read(data + read, payloadLen - read);
        if (r <= 0) {
            free(data);
            return false;
        }
        read += r;
    }

    *outData = data;
    *outLen = payloadLen;
    return true;
}

// ================================================================
//  Task: receive audio frames → push to playQueue
// ================================================================
static void wsReceiveTask(void *parameter)
{
    Serial.printf("[WS/resp] Receiver task on Core %d\n", xPortGetCoreID());

    while (respRunning) {
        if (!respTcp.connected()) {
            Serial.println("[WS/resp] Reconnecting…");
            if (respTcp.connect(respHost, respPort)) {
                if (wsResponseHandshake()) {
                    Serial.println("[WS/resp] Connected");
                } else {
                    respTcp.stop();
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    continue;
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
        }

        uint8_t *data = NULL;
        size_t len = 0;
        if (wsReadFrame(&data, &len)) {
            if (len > 0 && data != NULL) {
                pcm_block_t block;
                block.data = data;
                block.length = len;
                if (xQueueSend(playQueue, &block, pdMS_TO_TICKS(10)) != pdTRUE) {
                    free(data);
                }
            }
        } else {
            Serial.println("[WS/resp] Frame read error — disconnecting");
            respTcp.stop();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    respTcp.stop();
    vTaskDelete(NULL);
}

// ================================================================
//  wsResponseInit
// ================================================================
void wsResponseInit(const char *url, const char *token)
{
    if (token) {
        strncpy(respToken, token, sizeof(respToken) - 1);
        respToken[sizeof(respToken) - 1] = '\0';
    }

    parseUrl(url, respHost, sizeof(respHost), &respPort, respPath, sizeof(respPath));
    Serial.printf("[WS/resp] Target: %s:%u%s\n", respHost, respPort, respPath);

    respRunning = true;
    xTaskCreatePinnedToCore(
        wsReceiveTask,
        "wsReceiver",
        8192,
        NULL,
        2,
        &respTaskHndl,
        0   // Core 0
    );
}

// ================================================================
//  wsResponseDeinit
// ================================================================
void wsResponseDeinit()
{
    respRunning = false;
    if (respTaskHndl != NULL) {
        vTaskDelete(respTaskHndl);
        respTaskHndl = NULL;
    }
    if (respTcp.connected()) {
        respTcp.stop();
    }
    Serial.println("[WS/resp] Deinitialised");
}
