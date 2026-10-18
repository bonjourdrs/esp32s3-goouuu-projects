/*
 * 02_speaker_tone - tone playback through the on-board MAX98357 I2S amp.
 *
 * Every two seconds the amplifier plays a do-re-mi-sol-do scale so the
 * speaker path can be verified on its own.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "audio_tone.h"

static const char *TAG = "SPEAKER";

static void audio_task(void *arg)
{
    if (audio_tone_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed, task stopped");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        ESP_LOGI(TAG, "playing test scale");
        audio_tone_play_scale();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "02_speaker_tone  built " __DATE__ " " __TIME__);
    ESP_LOGI(TAG, "MAX98357 I2S  BCLK=GPIO15 LRC=GPIO16 DIN=GPIO7");

    xTaskCreate(audio_task, "audio", 4096, NULL, 4, NULL);
}
