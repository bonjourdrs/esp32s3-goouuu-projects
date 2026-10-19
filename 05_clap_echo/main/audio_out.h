#pragma once

#include <stdint.h>
#include "esp_err.h"

/* MAX98357 I2S amplifier on the GOOUUU baseboard:
 *   BCLK -> GPIO15
 *   LRC  -> GPIO16
 *   DIN  -> GPIO7
 */

esp_err_t audio_out_init(void);

/* Start a tone that decays to silence over a quarter of a second.
 * Calling it again replaces whatever is still playing. */
void audio_out_trigger(uint32_t freq_hz);

/* Produce and block-write one 10 ms block of audio. Call in a tight loop. */
void audio_out_pump(void);
