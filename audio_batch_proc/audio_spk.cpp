#include "audio_spk.h"
#include "audio_mic.h"
#include <driver/i2s.h>

QueueHandle_t playQueue = NULL;
volatile bool txActive = false;

void spkInit() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 1024,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,
        .fixed_mclk           = 0
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[SPK] I2S driver install failed: %d\n", err);
        return;
    }

    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_SCK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_AMP_DOUT_PIN,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    i2s_set_pin(I2S_NUM_0, &pins);
    i2s_zero_dma_buffer(I2S_NUM_0);

    Serial.println("[SPK] I2S TX initialised");
}

void spkDeinit() {
    i2s_driver_uninstall(I2S_NUM_0);
    Serial.println("[SPK] I2S TX uninstalled");
}

void audioPlayTask(void *parameter) {
    pcm_block_t block;
    Serial.printf("[SPK] Play task running on Core %d\n", xPortGetCoreID());

    while (1) {
        if (xQueueReceive(playQueue, &block, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (!txActive) {
                micDeinit();
                spkInit();
                txActive = true;
                Serial.println("[SPK] Switched to TX mode");
            }
            size_t written = 0;
            i2s_write(I2S_NUM_0, block.data, block.length, &written, portMAX_DELAY);
            free(block.data);
        } else {
            if (txActive) {
                i2s_zero_dma_buffer(I2S_NUM_0);
                vTaskDelay(pdMS_TO_TICKS(100));
                spkDeinit();
                micInit();
                txActive = false;
                Serial.println("[SPK] Switched back to RX mode");
            }
        }
    }
}
