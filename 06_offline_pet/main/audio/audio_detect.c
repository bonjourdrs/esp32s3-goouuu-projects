#include "audio_detect.h"

#include <stdint.h>

#include "audio_player.h"
#include "demo_config.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "DETECT"

typedef enum {
    VOICE_SILENCE,
    VOICE_ACTIVE,
} voice_state_t;

static float s_noise;
static bool s_seeded;
static voice_state_t s_state;
static int64_t s_loud_since_us;  /* when the current above-threshold run began */
static int64_t s_quiet_since_us; /* when the current below-threshold run began */

void audio_detect_init(void)
{
    s_noise = VOICE_NOISE_SEED;
    s_seeded = false;
    s_state = VOICE_SILENCE;
    s_loud_since_us = 0;
    s_quiet_since_us = 0;
}

void audio_detect_reset(void)
{
    s_state = VOICE_SILENCE;
    s_loud_since_us = 0;
    s_quiet_since_us = 0;
}

static void clamp_noise(void)
{
    if (s_noise < VOICE_NOISE_MIN) {
        s_noise = VOICE_NOISE_MIN;
    } else if (s_noise > VOICE_NOISE_MAX) {
        s_noise = VOICE_NOISE_MAX;
    }
}

bool audio_detect_update(float rms, app_event_t *out)
{
    /* The pet cannot hear itself: while the speaker runs, the microphone is
     * ignored and the state machine is parked back at silence. */
    if (audio_player_is_active()) {
        audio_detect_reset();
        return false;
    }

    if (!s_seeded) {
        s_noise = rms;
        clamp_noise();
        s_seeded = true;
    }

    int64_t now = esp_timer_get_time();
    float start_th = s_noise * VOICE_START_MULTIPLIER;
    float end_th = s_noise * VOICE_END_MULTIPLIER;
    bool emitted = false;

    if (rms > start_th) {
        if (s_loud_since_us == 0) {
            s_loud_since_us = now;
        }
    } else {
        s_loud_since_us = 0;
    }

    if (rms < end_th) {
        if (s_quiet_since_us == 0) {
            s_quiet_since_us = now;
        }
    } else {
        s_quiet_since_us = 0;
    }

    switch (s_state) {
    case VOICE_SILENCE:
        if (s_loud_since_us != 0 &&
            (now - s_loud_since_us) >= (int64_t)VOICE_START_HOLD_MS * 1000) {
            s_state = VOICE_ACTIVE;
            s_quiet_since_us = 0;
        } else if (rms < start_th) {
            /* Only learn the room while it really is quiet, otherwise a long
             * sentence would slowly drag the floor up to speech level. */
            s_noise += VOICE_NOISE_ALPHA * (rms - s_noise);
            clamp_noise();
        }
        break;

    case VOICE_ACTIVE:
        if (s_quiet_since_us != 0 &&
            (now - s_quiet_since_us) >= (int64_t)VOICE_END_HOLD_MS * 1000) {
            s_state = VOICE_SILENCE;
            s_loud_since_us = 0;
            s_quiet_since_us = 0;

            out->type = EVENT_VOICE_END;
            out->value = rms;
            out->timestamp_ms = (uint32_t)(now / 1000);
            emitted = true;
        }
        break;
    }

    return emitted;
}
