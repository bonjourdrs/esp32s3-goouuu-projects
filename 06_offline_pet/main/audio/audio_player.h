#pragma once

#include <stdbool.h>

#include "audio_assets.h"
#include "esp_err.h"

/* Plays recorded clips on the speaker from its own task, so callers never
 * block on audio. While a clip is playing - and for SPEAKER_MIC_GUARD_MS
 * afterwards - audio_player_is_active() stays true, which is how the audio
 * detector avoids hearing the pet's own voice. */

esp_err_t audio_player_init(void);

/* Queue a clip. Never blocks; a full queue just drops the request. */
bool audio_player_play(audio_id_t id);

bool audio_player_is_active(void);
