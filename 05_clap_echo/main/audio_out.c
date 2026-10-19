#include "audio_out.h"

#include <math.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "AUDIO"

/* MAX98357 pin map on the GOOUUU ESP32-S3 baseboard. */
#define I2S_BCLK_GPIO GPIO_NUM_15
#define I2S_LRC_GPIO  GPIO_NUM_16
#define I2S_DIN_GPIO  GPIO_NUM_7

#define SAMPLE_RATE  24000
#define BLOCK_FRAMES 240                          /* 10 ms per block */
#define TONE_FRAMES  (SAMPLE_RATE / 4)            /* 250 ms decay */
#define AMPLITUDE    9000.0f                      /* ~27% of full scale */
#define TWO_PI       6.28318530718f

static i2s_chan_handle_t s_tx;
static int16_t s_pcm[BLOCK_FRAMES * 2]; /* stereo interleaved */

static volatile uint32_t s_freq_hz;
static volatile int      s_tone_left;

esp_err_t audio_out_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* output silence instead of repeating the last frame */
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = BLOCK_FRAMES;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_LRC_GPIO,
            .dout = I2S_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "MAX98357 ready  BCLK=GPIO%d LRC=GPIO%d DIN=GPIO%d  fs=%d Hz",
             I2S_BCLK_GPIO, I2S_LRC_GPIO, I2S_DIN_GPIO, SAMPLE_RATE);
    return ESP_OK;
}

void audio_out_trigger(uint32_t freq_hz)
{
    s_freq_hz = freq_hz;
    s_tone_left = TONE_FRAMES;
}

void audio_out_pump(void)
{
    static float phase;

    uint32_t freq = s_freq_hz;
    int left = s_tone_left;

    for (int i = 0; i < BLOCK_FRAMES; ++i) {
        float sample = 0.0f;
        if (left > 0) {
            /* Squared linear ramp: a plucked decay rather than a hard cutoff. */
            float env = (float)left / (float)TONE_FRAMES;
            sample = sinf(phase) * AMPLITUDE * env * env;
            phase += TWO_PI * (float)freq / (float)SAMPLE_RATE;
            if (phase >= TWO_PI) {
                phase -= TWO_PI;
            }
            left--;
        }
        int16_t value = (int16_t)sample;
        s_pcm[i * 2] = value;
        s_pcm[i * 2 + 1] = value;
    }
    s_tone_left = left;

    size_t offset = 0;
    while (offset < sizeof(s_pcm)) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, (uint8_t *)s_pcm + offset,
                                         sizeof(s_pcm) - offset, &written, 100);
        if (err != ESP_OK || written == 0) {
            ESP_LOGW(TAG, "i2s write failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
            return;
        }
        offset += written;
    }
}
