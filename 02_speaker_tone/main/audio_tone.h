#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Tone output through the on-board MAX98357 I2S amplifier.
 * Wiring on the GOOUUU baseboard:
 *   BCLK -> GPIO15
 *   LRC  -> GPIO16
 *   DIN  -> GPIO7
 */

esp_err_t audio_tone_init(void);
void audio_tone_play_note(uint32_t freq_hz, uint32_t duration_ms);
void audio_tone_play_scale(void);
