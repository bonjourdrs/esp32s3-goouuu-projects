#pragma once

/* Every tunable threshold lives here. Values are starting points measured on
 * a bare baseboard; re-tune once the board sits in its enclosure. */

/* ------------------------------------------------------------------ motion */

/* IMU output data rate. The polling loop cannot run faster than the RTOS tick
 * (10 ms at the default 100 Hz), so samples are actually taken at 100 Hz. */
#define MOTION_SAMPLE_HZ        200
#define MOTION_SAMPLE_PERIOD_MS (1000 / MOTION_SAMPLE_HZ)

/* Dynamic acceleration is what is left after the gravity estimate is
 * subtracted, so 0 g means perfectly still. */
#define MOTION_STILL_G          0.08f
#define MOTION_MOVE_G           0.12f

/* PICK_UP: quiet first, then a sustained burst of motion. */
#define PICKUP_STILL_HOLD_MS    400
#define PICKUP_MOVE_HOLD_MS     150

/* Re-arm threshold for the next PICK_UP. */
#define PUTDOWN_STILL_HOLD_MS   700

/* TAP: a short spike that starts from a board at rest. The width limit rejects
 * spikes that are held up, and the rest band rejects spikes that begin while
 * the board is already moving - a shake drives the accelerometer just as hard
 * as a finger does, but never from a resting baseline. */
#define TAP_THRESHOLD_G         2.5f
#define TAP_MAX_WIDTH_MS        40
#define TAP_REST_BAND_G         0.35f
#define TAP_DEBOUNCE_MS         250

/* SHAKE: peaks counted inside a sliding window. A wrist shake runs at roughly
 * 3 Hz and each cycle produces two acceleration peaks, so a window of 800 ms
 * with four peaks to clear is about 2.5 Hz of real shaking. */
#define SHAKE_WINDOW_MS         800
#define SHAKE_PEAK_G            0.6f
#define SHAKE_HARD_PEAK_G       1.0f
#define SHAKE_HARD_PEAK_COUNT   3
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
