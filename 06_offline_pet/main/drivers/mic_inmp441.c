#include "mic_inmp441.h"

#include <math.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "board_config.h"

#define TAG "MIC"

/* The INMP441 delivers 24 significant bits in the upper part of a 32-bit
 * slot, so the sample has to be shifted down before it means anything. */
#define MIC_SAMPLE_SHIFT 8
#define MIC_FULL_SCALE   8388608.0f /* 2^23 */

static i2s_chan_handle_t s_rx;
static int32_t s_buf[AUDIO_FRAME_SAMPLES];

esp_err_t mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Mono + Philips selects the left slot, which is where the INMP441
     * transmits with its L/R pin tied to GND. */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_BCLK,
            .ws = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_MIC_DATA,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "INMP441 ready  WS=GPIO%d SCK=GPIO%d SD=GPIO%d  fs=%d Hz  frame=%d",
             PIN_MIC_WS, PIN_MIC_BCLK, PIN_MIC_DATA, AUDIO_SAMPLE_RATE, AUDIO_FRAME_SAMPLES);
    return ESP_OK;
}

esp_err_t mic_read_frame(float *rms, float *peak)
{
    *rms = 0.0f;
    *peak = 0.0f;

    if (s_rx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes = 0;
    esp_err_t err = i2s_channel_read(s_rx, s_buf, sizeof(s_buf), &bytes, pdMS_TO_TICKS(200));
    if (err != ESP_OK) {
        return err;
    }

    int frames = (int)(bytes / sizeof(int32_t));
    if (frames <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    int64_t acc = 0;
    int32_t biggest = 0;
    for (int i = 0; i < frames; ++i) {
        int32_t v = s_buf[i] >> MIC_SAMPLE_SHIFT;
        acc += (int64_t)v * (int64_t)v;
        int32_t magnitude = v < 0 ? -v : v;
        if (magnitude > biggest) {
            biggest = magnitude;
        }
    }

    *rms = sqrtf((float)acc / (float)frames) / MIC_FULL_SCALE;
    *peak = (float)biggest / MIC_FULL_SCALE;
    return ESP_OK;
}
