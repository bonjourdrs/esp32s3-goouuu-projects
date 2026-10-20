#include "board_config.h"

#include "aht20.h"
#include "bmp280.h"
#include "i2c_bus.h"
#include "ws2812.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";

/* Ends of the LED colour ramp, in degC. */
#define LED_COLD_C 15.0f
#define LED_WARM_C 32.0f

/* The LED mirrors the temperature so the reading is visible without a serial
 * monitor: blue when cold, red when warm. */
static void led_show_temperature(float t)
{
    float span = (t - LED_COLD_C) / (LED_WARM_C - LED_COLD_C);
    if (span < 0.0f) {
        span = 0.0f;
    } else if (span > 1.0f) {
        span = 1.0f;
    }

    ws2812_set_rgb((uint8_t)(255.0f * span), 0, (uint8_t)(255.0f * (1.0f - span)));
}

void app_main(void)
{
    ESP_LOGI(TAG, "07_env_sensor  AHT20 + BMP280 on the sensor I2C bus");

    ESP_ERROR_CHECK(ws2812_init(PIN_LED_RGB));
    ws2812_set_rgb(0, 0, 0);

    ESP_ERROR_CHECK(i2c_bus_init());
    i2c_bus_scan();

    bool aht_ok = (aht20_init() == ESP_OK);
    bool bmp_ok = (bmp280_init() == ESP_OK);

    if (!aht_ok) {
        ESP_LOGW(TAG, "AHT20 unavailable; humidity skipped");
    }
    if (!bmp_ok) {
        ESP_LOGW(TAG, "BMP280 unavailable; pressure skipped");
    }

    while (1) {
        bool got_any = false;

        if (aht_ok) {
            float t = 0.0f;
            float rh = 0.0f;
            if (aht20_read(&t, &rh) == ESP_OK) {
                ESP_LOGI(TAG, "AHT20   temp=%.2f C   humidity=%.1f %%", t, rh);
                led_show_temperature(t);
                got_any = true;
            } else {
                ESP_LOGW(TAG, "AHT20 read failed");
            }
        }

        if (bmp_ok) {
            float t = 0.0f;
            float p = 0.0f;
            if (bmp280_read(&t, &p) == ESP_OK) {
                ESP_LOGI(TAG, "BMP280  temp=%.2f C   pressure=%.2f hPa", t, p);
                led_show_temperature(t);
                got_any = true;
            } else {
                ESP_LOGW(TAG, "BMP280 read failed");
            }
        }

        if (!got_any) {
            ws2812_set_rgb(0, 0, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(ENV_READ_PERIOD_MS));
    }
}
