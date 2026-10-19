/*
 * 06_offline_pet - a desk pet that runs entirely on the board.
 *
 * It hears you speak and answers, knows when it is picked up, tapped, shaken
 * hard or turned over, and its mood shifts with how it has been treated.
 * No Wi-Fi, no cloud, no speech recognition: the sensors produce events, the
 * pet decides, and the recorded clips reply.
 */

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_event.h"
#include "audio_detect.h"
#include "audio_player.h"
#include "board_config.h"
#include "demo_config.h"
#include "imu_mpu6050.h"
#include "mic_inmp441.h"
#include "motion_detect.h"
#include "pet_app.h"
#include "speaker_max98357.h"
#include "ws2812.h"

static const char *TAG = "MAIN";
static const char *AUDIO_TAG = "AUDIO";
static const char *MOTION_TAG = "MOTION";

/* A single sample can trip more than one detector at once. */
#define MOTION_EVENT_MAX 4

static void audio_capture_task(void *arg)
{
    unsigned tick = 0;

    while (1) {
        float rms = 0.0f;
        float peak = 0.0f;

        if (mic_read_frame(&rms, &peak) != ESP_OK) {
            ESP_LOGW(AUDIO_TAG, "capture failed");
            continue;
        }

        app_event_t event;
        if (audio_detect_update(rms, &event)) {
            app_event_send(&event);
        }

        if (++tick % 10 == 0) {
            ESP_LOGI(AUDIO_TAG, "rms=%.4f peak=%.4f", rms, peak);
        }
    }
}

static void motion_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    unsigned tick = 0;

    while (1) {
        imu_sample_t sample;
        if (imu_read(&sample) == ESP_OK) {
            app_event_t events[MOTION_EVENT_MAX];
            int count = motion_detect_update(&sample, events, MOTION_EVENT_MAX);
            for (int i = 0; i < count; ++i) {
                app_event_send(&events[i]);
            }

            if (++tick % 40 == 0) {
                ESP_LOGI(MOTION_TAG, "mag=%.2fg pitch=%.0f roll=%.0f az=%.2fg",
                         sample.accel_mag, sample.pitch, sample.roll, sample.az);
            }
        }

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(MOTION_SAMPLE_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "06_offline_pet  built " __DATE__ " " __TIME__);
    ESP_LOGI(TAG, "speak, pick up, tap, shake or turn the board over");

    ESP_ERROR_CHECK(ws2812_init(PIN_LED_RGB));
    ESP_ERROR_CHECK(app_event_init());
    ESP_ERROR_CHECK(mic_init());
    ESP_ERROR_CHECK(speaker_init());
    ESP_ERROR_CHECK(audio_player_init());

    audio_detect_init();
    motion_detect_init();

    esp_err_t imu_err = imu_init();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "IMU unavailable; motion actions are disabled");
    }

    xTaskCreate(audio_capture_task, "audio_capture", 4096, NULL, 6, NULL);
    if (imu_err == ESP_OK) {
        xTaskCreate(motion_task, "motion", 4096, NULL, 5, NULL);
    }
    pet_app_start();
}
