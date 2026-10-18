/*
 * 03_mic_level - INMP441 microphone drives the on-board RGB LED brightness.
 *
 * Every measurement window the RMS loudness of the microphone signal is
 * converted to a 0..1 level, smoothed, and mapped to the WS2812 brightness:
 * silence -> LED off, loud -> LED at full brightness.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_LED_TYPE_RGB
#include "ws2812.h"
#endif
#include "mic_level.h"

static const char *TAG = "MICLED";

static void meter_task(void *arg)
{
    if (mic_level_init() != ESP_OK) {
        ESP_LOGE(TAG, "mic init failed, meter task stopped");
        vTaskDelete(NULL);
        return;
    }

    float smoothed = 0.0f;
    uint32_t tick = 0;

    while (1) {
        float level = mic_level_read();
        /* Light smoothing so the LED follows the sound instead of flickering. */
        smoothed = 0.6f * smoothed + 0.4f * level;

        /* Perceptual (gamma) curve makes low levels visibly dimmer. */
        float shaped = smoothed * smoothed;

#if CONFIG_LED_TYPE_RGB
        uint8_t bright = (uint8_t)(shaped * 255.0f + 0.5f);
        ws2812_set_rgb(bright, bright, bright);
#else
        gpio_set_level(CONFIG_LED_GPIO_NUM, shaped > 0.05f ? 1 : 0);
        uint8_t bright = (uint8_t)(shaped * 255.0f + 0.5f);
#endif

        if (++tick % 25 == 0) {
            ESP_LOGI(TAG, "mic level=%.2f  brightness=%u", smoothed, bright);
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "03_mic_level  built " __DATE__ " " __TIME__);
    ESP_LOGI(TAG, "INMP441 I2S  WS=GPIO4 SCK=GPIO5 SD=GPIO6");

#if CONFIG_LED_TYPE_RGB
    ESP_ERROR_CHECK(ws2812_init(CONFIG_LED_GPIO_NUM));
#else
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_LED_GPIO_NUM,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));
#endif

    xTaskCreate(meter_task, "meter", 4096, NULL, 5, NULL);
}
