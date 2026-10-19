#include "audio_player.h"

#include <stdint.h>

#include "board_config.h"
#include "demo_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "speaker_max98357.h"

#define TAG "PLAYER"

#define PLAYER_QUEUE_LEN 4

static QueueHandle_t s_cmd_q;
static volatile bool s_playing;
static volatile int64_t s_guard_until_us;

static void play_clip(const audio_clip_t *clip)
{
    int16_t scratch[AUDIO_FRAME_SAMPLES];
    size_t done = 0;

    while (done < clip->frames) {
        size_t chunk = clip->frames - done;
        if (chunk > AUDIO_FRAME_SAMPLES) {
            chunk = AUDIO_FRAME_SAMPLES;
        }

        const uint8_t *src = clip->pcm + done * 2;
        for (size_t i = 0; i < chunk; ++i) {
            scratch[i] = (int16_t)((uint16_t)src[i * 2] | ((uint16_t)src[i * 2 + 1] << 8));
        }

        if (speaker_write(scratch, chunk) != ESP_OK) {
            ESP_LOGE(TAG, "speaker write failed, dropping the rest of the clip");
            return;
        }
        done += chunk;
    }
}

static void player_task(void *arg)
{
    while (1) {
        audio_id_t id;
        if (xQueueReceive(s_cmd_q, &id, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const audio_clip_t *clip = audio_asset(id);
        if (clip == NULL) {
            ESP_LOGW(TAG, "clip %d is unavailable", (int)id);
            continue;
        }

        /* Raised before the first sample so the microphone is muted for the
         * whole clip, not just from the second block onwards. */
        s_playing = true;
        ESP_LOGI(TAG, "playing clip %d (%u frames, %u ms)",
                 (int)id, (unsigned)clip->frames,
                 (unsigned)(clip->frames * 1000 / AUDIO_SAMPLE_RATE));
        play_clip(clip);
        s_playing = false;
        s_guard_until_us = esp_timer_get_time() + (int64_t)SPEAKER_MIC_GUARD_MS * 1000;
    }
}

esp_err_t audio_player_init(void)
{
    s_cmd_q = xQueueCreate(PLAYER_QUEUE_LEN, sizeof(audio_id_t));
    if (s_cmd_q == NULL) {
        ESP_LOGE(TAG, "cannot create the player queue");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(player_task, "audio_player", 4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the player task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool audio_player_play(audio_id_t id)
{
    if (s_cmd_q == NULL) {
        return false;
    }
    return xQueueSend(s_cmd_q, &id, 0) == pdTRUE;
}

bool audio_player_is_active(void)
{
    if (s_playing) {
        return true;
    }
    return esp_timer_get_time() < s_guard_until_us;
}
