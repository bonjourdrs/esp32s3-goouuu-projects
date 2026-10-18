#include "audio_tone.h"

#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "AUDIO"

/* MAX98357 pin map on the GOOUUU ESP32-S3 baseboard. */
#define I2S_BCLK_GPIO GPIO_NUM_15
#define I2S_LRC_GPIO  GPIO_NUM_16
#define I2S_DIN_GPIO  GPIO_NUM_7

#define SAMPLE_RATE   44100
#define AMPLITUDE     9000 /* ~27% of full scale */
#define TWO_PI        6.28318530718f
#define TONE_BUF_FRAME 256

static i2s_chan_handle_t s_tx;
static int16_t s_buf[TONE_BUF_FRAME * 2]; /* stereo interleaved */

esp_err_t audio_tone_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* output silence instead of repeating the last frame */
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

void audio_tone_play_note(uint32_t freq_hz, uint32_t duration_ms)
{
    if (s_tx == NULL) {
        return;
    }

    const float step = TWO_PI * (float)freq_hz / (float)SAMPLE_RATE;
    float phase = 0.0f;
    uint32_t total_frames = (uint32_t)((uint64_t)SAMPLE_RATE * duration_ms / 1000ULL);
    uint32_t done = 0;

    while (done < total_frames) {
        uint32_t n = total_frames - done;
        if (n > TONE_BUF_FRAME) {
            n = TONE_BUF_FRAME;
        }

        for (uint32_t i = 0; i < n; i++) {
            int16_t v = (int16_t)(sinf(phase) * (float)AMPLITUDE);
            s_buf[i * 2] = v;
            s_buf[i * 2 + 1] = v;
            phase += step;
            if (phase >= TWO_PI) {
                phase -= TWO_PI;
            }
        }

        size_t written = 0;
        if (i2s_channel_write(s_tx, s_buf, (size_t)n * 2 * sizeof(int16_t),
                              &written, portMAX_DELAY) != ESP_OK) {
            ESP_LOGW(TAG, "i2s write failed");
            return;
        }
        done += n;
    }
}

void audio_tone_play_scale(void)
{
    /* do re mi sol do */
    static const uint16_t notes[] = { 523, 587, 659, 784, 1047 };
    static const uint16_t dur_ms[] = { 260, 260, 260, 320, 520 };

    for (int i = 0; i < (int)(sizeof(notes) / sizeof(notes[0])); i++) {
        audio_tone_play_note(notes[i], dur_ms[i]);
        vTaskDelay(pdMS_TO_TICKS(70));
    }
}
