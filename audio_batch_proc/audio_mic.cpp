#include "audio_mic.h"
#include <driver/i2s.h>

// ---- Global queue (definition) ----
// Initialised in micInit(), shared between audioTask (producer)
// and the WebSocket sender task (consumer).
QueueHandle_t pcmQueue = NULL;

// ---- Audio task handle (for controlled shutdown) ----
// Defined here, declared extern in audio_mic.h.
// Set by main.ino when the task is created; used by micDeinit().
TaskHandle_t audioTaskHandle = NULL;

// ---- Initialise I2S peripheral for INMP441 digital microphone ----
//
// The I2S driver uses DMA to copy audio samples directly from the
// INMP441 microphone into internal RAM ring‑buffers without any CPU
// involvement.  Eight DMA descriptors of 1024 samples each give a
// total hardware buffer of ~128 ms (16 kHz x 16‑bit), plenty of
// headroom for the task to consume data before an overrun occurs.
//
// Also creates the FreeRTOS queue that the audioTask will use to
// pass PCM blocks to the WebSocket sender task.
//
void micInit() {
    // PCM queue is created by main.ino setup() before any task runs.
    // If it doesn't exist, bail out.
    if (pcmQueue == NULL) {
        Serial.println("[MIC] PCM queue not initialised");
        return;
    }

    // ---------- Configure I2S peripheral ----------
    i2s_config_t i2s_config = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = DMA_BUF_COUNT,
        .dma_buf_len          = DMA_BUF_LEN,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,
        .fixed_mclk           = 0
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[MIC] I2S driver install failed: %d\n", err);
        return;
    }

    i2s_pin_config_t pin_config = {
        .bck_io_num   = I2S_SCK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_AMP_DOUT_PIN,    // MAX98357A DIN
        .data_in_num  = I2S_SD_PIN            // INMP441 SD pin (GPIO 34 input‑only)
    };

    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_zero_dma_buffer(I2S_NUM_0);

    Serial.printf("[MIC] Initialised — %d Hz, %d‑bit, DMA: %d x %d samples\n",
                  SAMPLE_RATE, BITS_PER_SAMPLE, DMA_BUF_COUNT, DMA_BUF_LEN);
}

// ---- De‑initialise I2S driver and audio task ----
//
// Deletes the audioTask first so it stops pushing new blocks,
// then uninstalls the I2S driver.  The queue is NOT destroyed
// here — the WebSocket sender task is still draining it.
//
void micDeinit() {
    // Stop the audio capture task
    if (audioTaskHandle != NULL) {
        vTaskDelete(audioTaskHandle);
        audioTaskHandle = NULL;
        Serial.println("[MIC] Audio task deleted");
    }

    // Uninstall the I2S peripheral driver
    i2s_driver_uninstall(I2S_NUM_0);
    Serial.println("[MIC] I2S driver uninstalled");
}

// ---- FreeRTOS task: capture audio via DMA -> push to queue ----
//
// This task is pinned to Core 1 so that Core 0 stays completely free
// for the WiFi / WebSocket / state‑machine loop.  The I2S DMA fills
// hardware ring‑buffers autonomously; the task simply pops complete
// blocks and forwards them through a FreeRTOS queue to the WebSocket
// sender.
//
// If the queue is full the block is dropped (its buffer is reused for
// the next I2S read) — missing one 128‑ms chunk is preferable to
// blocking the DMA pipeline.
//
// The task parameter is a pointer to the queue handle (QueueHandle_t*)
// so the task always has access even if the global changes.
//
void audioTask(void *parameter) {
    QueueHandle_t *pQueue = (QueueHandle_t *)parameter;
    const size_t bytesPerBlock = DMA_BUF_LEN * sizeof(int16_t);  // 2048

    int16_t     *currentBuffer = NULL;
    pcm_block_t  block;

    Serial.printf("[MIC] Audio task running on Core %d\n", xPortGetCoreID());

    while (1) {
        // Allocate a fresh buffer if we don't own one yet
        if (currentBuffer == NULL) {
            currentBuffer = (int16_t *)malloc(bytesPerBlock);
            if (currentBuffer == NULL) {
                Serial.println("[MIC] Buffer alloc failed — retrying");
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
        }

        // --- DMA read (blocking, zero CPU until data ready) ---
        // i2s_read() blocks until the DMA engine fills this buffer
        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_NUM_0,
                                  currentBuffer,
                                  bytesPerBlock,
                                  &bytesRead,
                                  portMAX_DELAY);

        if (err == ESP_OK && bytesRead > 0) {
            block.data   = (uint8_t *)currentBuffer;
            block.length = bytesRead;

            // Pass the buffer to the consumer via queue.
            // If the queue is full we drop the block and reuse
            // currentBuffer for the next DMA read.
            if (xQueueSend(*pQueue, &block, pdMS_TO_TICKS(5)) == pdTRUE) {
                // Buffer ownership passed to consumer — allocate a
                // fresh buffer next iteration
                currentBuffer = NULL;
            }
            // else: queue full → reuse currentBuffer (overwritten next i2s_read)
        }
    }

    // Cleanup (normally unreachable — task is deleted externally)
    free(currentBuffer);
    vTaskDelete(NULL);
}
