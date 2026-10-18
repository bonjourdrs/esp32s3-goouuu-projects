#include "ws2812.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

#define WS2812_TAG "WS2812"

/* RMT runs at 10 MHz -> one tick is 0.1 us. */
#define WS2812_RESOLUTION_HZ 10000000
/* WS2812 bit timing, in ticks (0.1 us): 0 -> 0.3us high / 0.9us low
 *                                       1 -> 0.9us high / 0.3us low */
#define WS2812_T0H 3
#define WS2812_T0L 9
#define WS2812_T1H 9
#define WS2812_T1L 3

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;

esp_err_t ws2812_init(int gpio_num)
{
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = gpio_num,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = WS2812_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&chan_cfg, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(WS2812_TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = WS2812_T0H, .level1 = 0, .duration1 = WS2812_T0L },
        .bit1 = { .level0 = 1, .duration0 = WS2812_T1H, .level1 = 0, .duration1 = WS2812_T1L },
        .flags = { .msb_first = 1 },
    };
    err = rmt_new_bytes_encoder(&enc_cfg, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(WS2812_TAG, "rmt_new_bytes_encoder failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_enable(s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(WS2812_TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(WS2812_TAG, "ready on GPIO%d", gpio_num);
    return ESP_OK;
}

void ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_chan == NULL) {
        return;
    }

    /* WS2812 expects GRB byte order. */
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };

    rmt_transmit(s_chan, s_encoder, grb, sizeof(grb), &tx_cfg);
    rmt_tx_wait_all_done(s_chan, 100);
}
