#pragma once

#include <stdbool.h>

#include "app_event.h"

/* Watches loudness and decides when somebody finished speaking.
 *
 * The threshold is not fixed: the detector tracks the room's noise floor while
 * it judges the room quiet, so it adapts to a fan or a TV in the background
 * without needing a hand-tuned constant. */

void audio_detect_init(void);

/* Feed one frame's RMS (normalised 0..1). Returns true and fills *out when
 * the frame completes a speech burst. */
bool audio_detect_update(float rms, app_event_t *out);

/* Forget the speech state machine. Called while the speaker is talking so the
 * pet's own voice cannot look like the start of a sentence. */
void audio_detect_reset(void);
