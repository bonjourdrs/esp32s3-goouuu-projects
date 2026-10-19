/*
 * 05_clap_echo - clap at the board and it answers.
 *
 * The microphone watches for a sharp clap. When one is detected the amplifier
 * plays a short decaying note, and that note is chosen by how the board is
 * tilted. The RGB LED tracks ambient loudness and flashes white on each clap.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "audio_out.h"
#include "imu.h"
#include "mic_level.h"
#if CONFIG_LED_TYPE_RGB
#include "ws2812.h"
#endif

static const char *TAG = "CLAP";

/* A clap is a fast jump well above the room's noise floor. */
#define CLAP_LEVEL         0.65f
#define CLAP_RISE          0.20f
#define CLAP_REFRACTORY_MS 180

/* Pentatonic C5; tilting the board walks up the scale. */
#define NOTE_COUNT 5
static const uint16_t s_notes[NOTE_COUNT] = { 523, 587, 659, 784, 880 };
#define TILT_RANGE_DEG 40.0f

/* LED flash on a clap, decaying over roughly a quarter second. */
#define FLASH_DECAY 0.90f

static QueueHandle_t s_clap_q;
static volatile int   s_note_index;
static volatile float s_tilt_deg;

static void imu_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();

    while (1) {
        float tilt = imu_tilt_deg();
        s_tilt_deg = tilt;

        float clamped = tilt;
        if (clamped < -TILT_RANGE_DEG) {
            clamped = -TILT_RANGE_DEG;
        } else if (clamped > TILT_RANGE_DEG) {
            clamped = TILT_RANGE_DEG;
        }
        float pos = (clamped + TILT_RANGE_DEG) / (2.0f * TILT_RANGE_DEG);
        s_note_index = (int)lroundf(pos * (float)(NOTE_COUNT - 1));

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
    }
}

static void meter_task(void *arg)
{
    float smoothed = 0.0f;
    float previous = 0.0f;
    float flash = 0.0f;
    int64_t last_clap_us = 0;
    uint32_t tick = 0;

    while (1) {
        float level = mic_level_read();
        smoothed = 0.6f * smoothed + 0.4f * level;

        int64_t now_us = esp_timer_get_time();
        bool clap = level > CLAP_LEVEL &&
                    (level - previous) > CLAP_RISE &&
                    (now_us - last_clap_us) > (int64_t)CLAP_REFRACTORY_MS * 1000;

        if (clap) {
            last_clap_us = now_us;
            flash = 1.0f;
            int note = s_note_index;
            xQueueSend(s_clap_q, &note, 0);
            ESP_LOGI(TAG, "clap  level=%.2f  tilt=%.0f deg  note=%d  %u Hz",
                     level, s_tilt_deg, note, s_notes[note]);
        }
        previous = level;

        flash *= FLASH_DECAY;

        /* Ambient loudness drives the brightness; a clap pushes it to full. */
        float shaped = smoothed * smoothed;
        if (flash > shaped) {
            shaped = flash;
        }
        uint8_t bright = (uint8_t)(shaped * 255.0f + 0.5f);

#if CONFIG_LED_TYPE_RGB
        ws2812_set_rgb(bright, bright, bright);
#else
        gpio_set_level(CONFIG_LED_GPIO_NUM, shaped > 0.05f ? 1 : 0);
#endif

        if (++tick % 40 == 0) {
            ESP_LOGI(TAG, "level=%.2f  brightness=%u  tilt=%.0f deg  note=%d",
                     smoothed, bright, s_tilt_deg, s_note_index);
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void audio_task(void *arg)
{
    while (1) {
        int note = 0;
        if (xQueueReceive(s_clap_q, &note, 0) == pdTRUE) {
            if (note >= 0 && note < NOTE_COUNT) {
                audio_out_trigger(s_notes[note]);
            }
        }
        audio_out_pump();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "05_clap_echo  built " __DATE__ " " __TIME__);
    ESP_LOGI(TAG, "clap and the board answers; tilt it to pick the note");

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

    ESP_ERROR_CHECK(mic_level_init());
    ESP_ERROR_CHECK(audio_out_init());

    esp_err_t imu_err = imu_init();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "IMU unavailable; every clap answers with %u Hz", s_notes[0]);
    }

    s_clap_q = xQueueCreate(4, sizeof(int));
    if (s_clap_q == NULL) {
        ESP_LOGE(TAG, "cannot create the clap queue");
        return;
    }

    /* Audio sits above the meter so the I2S DMA never starves. */
    xTaskCreate(audio_task, "audio", 4096, NULL, 6, NULL);
    xTaskCreate(meter_task, "meter", 4096, NULL, 5, NULL);
    if (imu_err == ESP_OK) {
        xTaskCreate(imu_task, "imu", 3072, NULL, 4, NULL);
    }
}
