/*
 * 01_blink_led - on-board WS2812 RGB LED blink demo.
 *
 * Cycles a set of colours on the addressable RGB LED and keeps a heartbeat
 * on the serial console. LED type / GPIO / period come from menuconfig
 * (menu "ESP32-S3 Blink Demo").
 */

#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if CONFIG_LED_TYPE_RGB
#include "ws2812.h"
#endif

static const char *TAG = "BLINK";

static void blink_task(void *arg)
{
    const uint32_t period_ms = CONFIG_LED_BLINK_PERIOD_MS;
    uint32_t step = 0;

#if CONFIG_LED_TYPE_RGB
    static const uint8_t colors[][3] = {
        { 32,  0,  0 },  /* red    */
        {  0, 32,  0 },  /* green  */
        {  0,  0, 32 },  /* blue   */
        { 32, 32,  0 },  /* yellow */
        {  0, 32, 32 },  /* cyan   */
        { 32,  0, 32 },  /* purple */
        { 32, 32, 32 },  /* white  */
        {  0,  0,  0 },  /* off    */
    };
    const uint32_t color_count = sizeof(colors) / sizeof(colors[0]);
#else
    bool level = false;
#endif

    while (1) {
#if CONFIG_LED_TYPE_RGB
        const uint8_t *c = colors[step % color_count];
        ws2812_set_rgb(c[0], c[1], c[2]);
#else
        level = !level;
        gpio_set_level(CONFIG_LED_GPIO_NUM, level ? 1 : 0);
#endif
        step++;

        /* heartbeat roughly every 5 seconds */
        if (step % 10 == 0) {
            ESP_LOGI(TAG, "heartbeat  uptime=%" PRIu64 "s  free_heap=%u B  led_step=%" PRIu32,
                     esp_timer_get_time() / 1000000ULL,
                     (unsigned)esp_get_free_heap_size(), step);
        }

        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "01_blink_led  built " __DATE__ " " __TIME__);

#if CONFIG_LED_TYPE_RGB
    ESP_LOGI(TAG, "LED: WS2812 RGB on GPIO%d", CONFIG_LED_GPIO_NUM);
    ESP_ERROR_CHECK(ws2812_init(CONFIG_LED_GPIO_NUM));
#else
    ESP_LOGI(TAG, "LED: plain GPIO on GPIO%d", CONFIG_LED_GPIO_NUM);
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_LED_GPIO_NUM,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));
#endif

    xTaskCreate(blink_task, "blink", 3072, NULL, 5, NULL);
}
