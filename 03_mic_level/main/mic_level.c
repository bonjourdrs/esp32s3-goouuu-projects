#include "mic_level.h"

#include <math.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "MIC"

/* INMP441 pin map on the GOOUUU ESP32-S3 baseboard. */
#define MIC_WS_GPIO  GPIO_NUM_4
#define MIC_SCK_GPIO GPIO_NUM_5
#define MIC_SD_GPIO  GPIO_NUM_6

#define MIC_SAMPLE_RATE 16000
#define MIC_WINDOW_FRAME 256 /* ~16 ms per measurement */

/* INMP441 delivers a 24-bit sample in the upper bits of the 32-bit slot. */
#define MIC_SAMPLE_SHIFT 8
#define MIC_FULL_SCALE   8388608.0f /* 2^23 */

/* Loudness window mapped to 0..1. Tuned so the room's idle noise floor
 * (~-47 dBFS on this board) lands near zero and speech peaks near full. */
#define MIC_FLOOR_DB (-55.0f)
#define MIC_CEIL_DB  (-5.0f)

static i2s_chan_handle_t s_rx;
static int32_t s_buf[MIC_WINDOW_FRAME];

esp_err_t mic_level_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Mono + Philips slots selects the left slot, which is where the
     * INMP441 transmits with its L/R pin tied to GND. */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_GPIO,
            .ws = MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_SD_GPIO,
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

    ESP_LOGI(TAG, "INMP441 ready  WS=GPIO%d SCK=GPIO%d SD=GPIO%d  fs=%d Hz",
             MIC_WS_GPIO, MIC_SCK_GPIO, MIC_SD_GPIO, MIC_SAMPLE_RATE);
    return ESP_OK;
}

float mic_level_read(void)
{
    if (s_rx == NULL) {
        return 0.0f;
    }

    size_t bytes = 0;
    if (i2s_channel_read(s_rx, s_buf, sizeof(s_buf), &bytes, pdMS_TO_TICKS(200)) != ESP_OK) {
        return 0.0f;
    }

    int frames = (int)(bytes / sizeof(int32_t));
    if (frames <= 0) {
        return 0.0f;
    }

    int64_t acc = 0;
    for (int i = 0; i < frames; i++) {
        int32_t v = s_buf[i] >> MIC_SAMPLE_SHIFT;
        acc += (int64_t)v * (int64_t)v;
    }

    float rms = sqrtf((float)acc / (float)frames);
    float dbfs = 20.0f * log10f(rms / MIC_FULL_SCALE + 1e-9f);

    float level = (dbfs - MIC_FLOOR_DB) / (MIC_CEIL_DB - MIC_FLOOR_DB);
    if (level < 0.0f) {
        level = 0.0f;
    } else if (level > 1.0f) {
        level = 1.0f;
    }
    return level;
}
