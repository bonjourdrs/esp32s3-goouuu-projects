#include "pet_app.h"

#include <stdint.h>

#include "app_event.h"
#include "audio_player.h"
#include "board_config.h"
#include "demo_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ws2812.h"

#define TAG "PET"

/* Roughly how fast the response flash fades, per PET_TICK_MS tick. */
#define FLASH_DECAY 0.85f

static float s_mood = 50.0f;
static float s_anger;
static float s_dizzy;
static float s_flash;
static int64_t s_cooldown_until_us;

static void clamp_values(void)
{
    if (s_mood < 0.0f) {
        s_mood = 0.0f;
    } else if (s_mood > PET_VALUE_MAX) {
        s_mood = PET_VALUE_MAX;
    }
    if (s_anger < 0.0f) {
        s_anger = 0.0f;
    } else if (s_anger > PET_VALUE_MAX) {
        s_anger = PET_VALUE_MAX;
    }
    if (s_dizzy < 0.0f) {
        s_dizzy = 0.0f;
    } else if (s_dizzy > PET_VALUE_MAX) {
        s_dizzy = PET_VALUE_MAX;
    }
}

/* An action changes how the pet feels even when it is too busy to answer. */
static void apply_emotion(app_event_type_t type)
{
    switch (type) {
    case EVENT_VOICE_END:
        s_mood += 2.0f;
        break;
    case EVENT_PICK_UP:
        s_mood += 3.0f;
        break;
    case EVENT_TAP:
        s_anger += 10.0f;
        break;
    case EVENT_SHAKE_HARD:
        s_dizzy += 30.0f;
        s_anger += 5.0f;
        break;
    default:
        break;
    }
    clamp_values();
}

static audio_id_t clip_for(app_event_type_t type)
{
    switch (type) {
    case EVENT_VOICE_END:
        return AUDIO_HELLO;
    case EVENT_PICK_UP:
        return AUDIO_PICKUP;
    case EVENT_TAP:
        return AUDIO_OUCH;
    case EVENT_SHAKE_HARD:
        return AUDIO_SHAKE_HARD;
    case EVENT_UPSIDE_DOWN:
        return AUDIO_UPSIDE_DOWN;
    default:
        return AUDIO_ID_COUNT;
    }
}

static void update_led(void)
{
    uint8_t r = (uint8_t)(s_anger * 255.0f / PET_VALUE_MAX);
    uint8_t g = (uint8_t)(s_mood * 255.0f / PET_VALUE_MAX);
    uint8_t b = (uint8_t)(s_dizzy * 255.0f / PET_VALUE_MAX);

    /* Every reply adds a white pop on top of the mood colour. */
    uint8_t flash = (uint8_t)(s_flash * 255.0f);
    if (flash > 0) {
        int rr = r + flash;
        int gg = g + flash;
        int bb = b + flash;
        r = rr > 255 ? 255 : (uint8_t)rr;
        g = gg > 255 ? 255 : (uint8_t)gg;
        b = bb > 255 ? 255 : (uint8_t)bb;
    }

    ws2812_set_rgb(r, g, b);
}

static void pet_task(void *arg)
{
    int64_t last_us = esp_timer_get_time();

    while (1) {
        app_event_t event;
        if (app_event_receive(&event, pdMS_TO_TICKS(PET_TICK_MS))) {
            int64_t now = esp_timer_get_time();
            bool busy = audio_player_is_active();

            apply_emotion(event.type);

            if (busy || now < s_cooldown_until_us) {
                /* Too soon, or still talking: remember the feeling, stay quiet. */
                ESP_LOGI(TAG, "heard %d (%.2f) while busy; mood=%.0f anger=%.0f dizzy=%.0f",
                         (int)event.type, event.value, s_mood, s_anger, s_dizzy);
            } else {
                audio_id_t clip = clip_for(event.type);
                if (clip != AUDIO_ID_COUNT && audio_player_play(clip)) {
                    s_flash = 1.0f;
                    s_cooldown_until_us = now + (int64_t)PET_RESPONSE_COOLDOWN_MS * 1000;
                    ESP_LOGI(TAG, "event=%d (%.2f) -> clip %d   mood=%.0f anger=%.0f dizzy=%.0f",
                             (int)event.type, event.value, (int)clip, s_mood, s_anger, s_dizzy);
                }
            }
        }

        int64_t now = esp_timer_get_time();
        float dt = (float)(now - last_us) / 1000000.0f;
        last_us = now;

        s_anger -= PET_ANGER_DECAY_PER_S * dt;
        s_dizzy -= PET_DIZZY_DECAY_PER_S * dt;
        s_mood -= PET_MOOD_DECAY_PER_S * dt;
        clamp_values();

        s_flash *= FLASH_DECAY;
        if (s_flash < 0.02f) {
            s_flash = 0.0f;
        }
        update_led();
    }
}

void pet_app_start(void)
{
    if (xTaskCreate(pet_task, "pet", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the pet task");
    }
}
