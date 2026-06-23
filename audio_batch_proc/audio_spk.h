#ifndef AUDIO_SPK_H
#define AUDIO_SPK_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ---- I2S pins for MAX98357A speaker ----
// WS (GPIO 25) and BCLK (GPIO 26) shared with INMP441 microphone
#define I2S_AMP_DOUT_PIN 22

// ---- FreeRTOS queue for playback blocks ----
#define PLAY_QUEUE_LENGTH 16

extern QueueHandle_t playQueue;
extern volatile bool txActive;

// ---- Public API ----
void spkInit();
void spkDeinit();
void audioPlayTask(void *parameter);

#endif
