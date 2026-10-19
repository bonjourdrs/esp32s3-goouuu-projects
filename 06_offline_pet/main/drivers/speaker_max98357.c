#include "speaker_max98357.h"

#include "driver/i2s_std.h"
#include "esp_log.h"

#include "board_config.h"

#define TAG "SPK"

/* One DMA block; the clip player hands over frames in chunks of this size. */
#define SPK_BLOCK_FRAMES AUDIO_FRAME_SAMPLES

static i2s_chan_handle_t s_tx;
static int16_t s_stereo[SPK_BLOCK_FRAMES * 2];

esp_err_t speaker_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* idle output is silence, not a held sample */
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = SPK_BLOCK_FRAMES;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_SPK_BCLK,
            .ws = PIN_SPK_LRCLK,
            .dout = PIN_SPK_DATA,
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
             PIN_SPK_BCLK, PIN_SPK_LRCLK, PIN_SPK_DATA, AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t speaker_write(const int16_t *frames, size_t count)
{
    if (s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    while (count > 0) {
        size_t chunk = count < SPK_BLOCK_FRAMES ? count : SPK_BLOCK_FRAMES;

        for (size_t i = 0; i < chunk; ++i) {
            s_stereo[i * 2] = frames[i];
            s_stereo[i * 2 + 1] = frames[i];
        }

        size_t bytes = chunk * 2 * sizeof(int16_t);
        size_t offset = 0;
        while (offset < bytes) {
            size_t written = 0;
            esp_err_t err = i2s_channel_write(s_tx, (const uint8_t *)s_stereo + offset,
                                              bytes - offset, &written, 100);
            if (err != ESP_OK || written == 0) {
                ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(err));
                return err == ESP_OK ? ESP_FAIL : err;
            }
            offset += written;
        }

        frames += chunk;
        count -= chunk;
    }

    return ESP_OK;
}
