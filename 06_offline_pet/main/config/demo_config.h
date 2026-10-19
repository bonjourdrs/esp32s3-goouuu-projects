#pragma once

/* Every tunable threshold lives here. Values are starting points measured on
 * a bare baseboard; re-tune once the board sits in its enclosure. */

/* ------------------------------------------------------------------ motion */

#define MOTION_SAMPLE_HZ        200
#define MOTION_SAMPLE_PERIOD_MS (1000 / MOTION_SAMPLE_HZ)

/* Dynamic acceleration is what is left after the gravity estimate is
 * subtracted, so 0 g means perfectly still. */
#define MOTION_STILL_G          0.08f
#define MOTION_MOVE_G           0.15f

/* PICK_UP: quiet first, then a sustained burst of motion. */
#define PICKUP_STILL_HOLD_MS    1000
#define PICKUP_MOVE_HOLD_MS     200

/* Re-arm threshold for the next PICK_UP. */
#define PUTDOWN_STILL_HOLD_MS   700

/* TAP: a short spike, not a sustained one. The width limit is what separates
 * a tap from a shake, which also drives the accelerometer past the threshold
 * but holds it there for far longer. */
#define TAP_THRESHOLD_G         2.5f
#define TAP_MAX_WIDTH_MS        40
#define TAP_DEBOUNCE_MS         250

/* SHAKE: peaks counted inside a sliding window. */
#define SHAKE_WINDOW_MS         500
#define SHAKE_PEAK_G            0.8f
#define SHAKE_HARD_PEAK_G       1.5f
#define SHAKE_HARD_PEAK_COUNT   6
#define SHAKE_COOLDOWN_MS       700

/* UPSIDE_DOWN: gravity flips to -1 g on Z and stays there. */
#define UPSIDE_DOWN_AZ_G        (-0.7f)
#define UPSIDE_DOWN_HOLD_MS     800
#define UPSIDE_DOWN_REARM_AZ_G  0.5f

/* ------------------------------------------------------------------- audio */

/* Noise floor tracks the room only while it is judged quiet. */
#define VOICE_NOISE_ALPHA       0.02f
#define VOICE_NOISE_SEED        0.005f
#define VOICE_NOISE_MIN         0.0005f
#define VOICE_NOISE_MAX         0.05f

#define VOICE_START_MULTIPLIER  3.0f
#define VOICE_END_MULTIPLIER    1.8f
#define VOICE_START_HOLD_MS     300
#define VOICE_END_HOLD_MS       500

/* While the speaker is talking the microphone is ignored, plus this much
 * trailing time so the last echo cannot trip the detector. */
#define SPEAKER_MIC_GUARD_MS    300

/* --------------------------------------------------------------------- pet */

/* After answering, ignore further responses for a moment so the pet does not
 * talk over itself. */
#define PET_RESPONSE_COOLDOWN_MS 800
#define PET_TICK_MS              50

#define PET_ANGER_DECAY_PER_S    1.0f
#define PET_DIZZY_DECAY_PER_S    1.0f
#define PET_MOOD_DECAY_PER_S     0.5f

#define PET_VALUE_MAX            100
