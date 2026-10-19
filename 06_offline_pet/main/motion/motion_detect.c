#include "motion_detect.h"

#include <math.h>
#include <stdint.h>

#include "demo_config.h"
#include "esp_timer.h"

/* Time constant of the gravity estimate. At 200 Hz this settles in about a
 * quarter of a second, slow enough to ignore shaking, fast enough to follow
 * the board being turned over. */
#define GRAVITY_ALPHA 0.02f

typedef enum {
    MOTION_FREE,
    MOTION_HELD,
} motion_state_t;

#define SHAKE_SLOTS 24

static float s_gravity[3];
static bool s_seeded;

/* pick up / put down */
static motion_state_t s_motion_state;
static int64_t s_still_since_us;
static int64_t s_move_since_us;
static int64_t s_still_before_burst_us;

/* tap */
static int64_t s_above_since_us;
static bool s_above_is_tap;
static int64_t s_last_tap_us;

/* shake */
static int64_t s_peak_us[SHAKE_SLOTS];
static float s_peak_g[SHAKE_SLOTS];
static int s_peak_head;
static bool s_peak_rising;
static float s_peak_max;
static int64_t s_shake_until_us;

/* upside down */
static int64_t s_upside_since_us;
static bool s_upside_armed;

void motion_detect_init(void)
{
    for (int i = 0; i < 3; ++i) {
        s_gravity[i] = 0.0f;
    }
    s_seeded = false;

    s_motion_state = MOTION_FREE;
    s_still_since_us = 0;
    s_move_since_us = 0;
    s_still_before_burst_us = 0;

    s_above_since_us = 0;
    s_above_is_tap = false;
    s_last_tap_us = 0;

    s_peak_head = 0;
    s_peak_rising = false;
    s_peak_max = 0.0f;
    s_shake_until_us = 0;

    s_upside_since_us = 0;
    s_upside_armed = true;
}

/* How many shake peaks fall inside the sliding window, and how big the
 * biggest of them was. */
static int peaks_in_window(int64_t now, float *max_g)
{
    int count = 0;
    *max_g = 0.0f;
    int64_t cutoff = now - (int64_t)SHAKE_WINDOW_MS * 1000;

    for (int i = 0; i < SHAKE_SLOTS; ++i) {
        if (s_peak_us[i] >= cutoff && s_peak_us[i] != 0) {
            ++count;
            if (s_peak_g[i] > *max_g) {
                *max_g = s_peak_g[i];
            }
        }
    }
    return count;
}

static void push_peak(int64_t now, float value)
{
    s_peak_us[s_peak_head] = now;
    s_peak_g[s_peak_head] = value;
    s_peak_head = (s_peak_head + 1) % SHAKE_SLOTS;
}

int motion_detect_update(const imu_sample_t *sample, app_event_t *events, int max_events)
{
    int n = 0;
    int64_t now = esp_timer_get_time();

    float accel[3] = { sample->ax, sample->ay, sample->az };
    if (!s_seeded) {
        for (int i = 0; i < 3; ++i) {
            s_gravity[i] = accel[i];
        }
        s_seeded = true;
    }

    float dynamic_sq = 0.0f;
    for (int i = 0; i < 3; ++i) {
        s_gravity[i] += GRAVITY_ALPHA * (accel[i] - s_gravity[i]);
        float residual = accel[i] - s_gravity[i];
        dynamic_sq += residual * residual;
    }
    float dynamic = sqrtf(dynamic_sq);

    bool still_now = dynamic < MOTION_STILL_G;
    bool moving_now = dynamic > MOTION_MOVE_G;

    /* ---------------------------------------------------------- pick up */

    if (moving_now) {
        if (s_move_since_us == 0) {
            s_move_since_us = now;
            /* Stillness has to be sampled before it is cleared below. */
            s_still_before_burst_us = s_still_since_us ? (now - s_still_since_us) : 0;
        }
    } else {
        s_move_since_us = 0;
    }

    if (still_now) {
        if (s_still_since_us == 0) {
            s_still_since_us = now;
        }
    } else {
        s_still_since_us = 0;
    }

    if (s_motion_state == MOTION_FREE && s_move_since_us != 0 &&
        (now - s_move_since_us) >= (int64_t)PICKUP_MOVE_HOLD_MS * 1000 &&
        s_still_before_burst_us >= (int64_t)PICKUP_STILL_HOLD_MS * 1000) {
        if (n < max_events) {
            events[n].type = EVENT_PICK_UP;
            events[n].value = dynamic;
            events[n].timestamp_ms = (uint32_t)(now / 1000);
            ++n;
        }
        s_motion_state = MOTION_HELD;
        s_move_since_us = 0;
    }

    if (s_motion_state == MOTION_HELD && s_still_since_us != 0 &&
        (now - s_still_since_us) >= (int64_t)PUTDOWN_STILL_HOLD_MS * 1000) {
        s_motion_state = MOTION_FREE;
    }

    /* -------------------------------------------------------------- tap */

    if (sample->accel_mag > TAP_THRESHOLD_G) {
        if (s_above_since_us == 0) {
            s_above_since_us = now;
            s_above_is_tap = true;
        } else if ((now - s_above_since_us) > (int64_t)TAP_MAX_WIDTH_MS * 1000) {
            /* Held above the threshold for too long: a shake, not a tap. */
            s_above_is_tap = false;
        }
    } else {
        if (s_above_since_us != 0 && s_above_is_tap &&
            (now - s_last_tap_us) >= (int64_t)TAP_DEBOUNCE_MS * 1000) {
            if (n < max_events) {
                events[n].type = EVENT_TAP;
                events[n].value = sample->accel_mag;
                events[n].timestamp_ms = (uint32_t)(now / 1000);
                ++n;
            }
            s_last_tap_us = now;
        }
        s_above_since_us = 0;
        s_above_is_tap = false;
    }

    /* ------------------------------------------------------------ shake */

    if (!s_peak_rising) {
        if (dynamic > SHAKE_PEAK_G) {
            s_peak_rising = true;
            s_peak_max = dynamic;
        }
    } else if (dynamic > s_peak_max) {
        s_peak_max = dynamic;
    } else if (dynamic < SHAKE_PEAK_G * 0.6f) {
        push_peak(now, s_peak_max);
        s_peak_rising = false;
    }

    float peak_max = 0.0f;
    int peaks = peaks_in_window(now, &peak_max);

    if (peaks > SHAKE_HARD_PEAK_COUNT && peak_max > SHAKE_HARD_PEAK_G &&
        now >= s_shake_until_us) {
        if (n < max_events) {
            events[n].type = EVENT_SHAKE_HARD;
            events[n].value = peak_max;
            events[n].timestamp_ms = (uint32_t)(now / 1000);
            ++n;
        }
        /* One shake, one event: the window ages out during the cooldown. */
        s_shake_until_us = now + (int64_t)SHAKE_COOLDOWN_MS * 1000;
    }

    /* ------------------------------------------------------ upside down */

    if (sample->az < UPSIDE_DOWN_AZ_G) {
        if (s_upside_since_us == 0) {
            s_upside_since_us = now;
        }
        if (s_upside_armed &&
            (now - s_upside_since_us) >= (int64_t)UPSIDE_DOWN_HOLD_MS * 1000) {
            if (n < max_events) {
                events[n].type = EVENT_UPSIDE_DOWN;
                events[n].value = sample->az;
                events[n].timestamp_ms = (uint32_t)(now / 1000);
                ++n;
            }
            s_upside_armed = false;
        }
    } else {
        s_upside_since_us = 0;
        if (sample->az > UPSIDE_DOWN_REARM_AZ_G) {
            s_upside_armed = true;
        }
    }

    return n;
}
