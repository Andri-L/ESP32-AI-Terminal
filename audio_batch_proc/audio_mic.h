#ifndef AUDIO_MIC_H
#define AUDIO_MIC_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ---- Audio quality configuration ----
#define SAMPLE_RATE     16000
#define BITS_PER_SAMPLE 16
#define DMA_BUF_COUNT   8
#define DMA_BUF_LEN     1024   // Samples per DMA buffer (2048 bytes at 16-bit)

// ---- FreeRTOS queue sizing ----
#define PCM_QUEUE_LENGTH 4     // Number of PCM blocks the queue can hold

// ---- I2S pins for INMP441 microphone ----
// GPIO  25 (WS) & GPIO  26 (SCK) shared with MAX98357A speaker
// GPIO  34 is input‑only -> perfect for mic data
#define I2S_WS_PIN  25
#define I2S_SCK_PIN 26
#define I2S_SD_PIN  34

// ---- PCM block passed through FreeRTOS queue ----
typedef struct {
    uint8_t *data;
    size_t   length;
} pcm_block_t;

// ---- Global queue handle ----
// Created in micInit(), shared by audioTask and wsSendTask
extern QueueHandle_t pcmQueue;

// ---- Global task handle (set by main.ino, used by micDeinit) ----
extern TaskHandle_t audioTaskHandle;

// ---- Public API ----
void micInit();
void micDeinit();
void audioTask(void *parameter);

#endif
