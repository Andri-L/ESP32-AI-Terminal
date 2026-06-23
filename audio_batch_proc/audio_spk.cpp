#include "audio_spk.h"
#include "audio_mic.h"
#include <driver/i2s.h>

QueueHandle_t playQueue = NULL;
volatile bool g_isSpeaking = false;

void audioPlayTask(void *parameter) {
    pcm_block_t block;
    Serial.printf("[SPK] Play task running on Core %d\n", xPortGetCoreID());

    while (1) {
        if (xQueueReceive(playQueue, &block, pdMS_TO_TICKS(500)) == pdTRUE) {
            g_isSpeaking = true;
            size_t written = 0;
            i2s_write(I2S_NUM_0, block.data, block.length, &written, portMAX_DELAY);
            free(block.data);
        } else {
            // Queue empty for 500ms → DMA drained, audio finished
            g_isSpeaking = false;
        }
    }
}
