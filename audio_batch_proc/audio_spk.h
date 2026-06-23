#ifndef AUDIO_SPK_H
#define AUDIO_SPK_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ---- FreeRTOS queue for playback blocks ----
#define PLAY_QUEUE_LENGTH 8

extern QueueHandle_t playQueue;
extern volatile bool g_isSpeaking;

// ---- Public API ----
void audioPlayTask(void *parameter);

#endif
