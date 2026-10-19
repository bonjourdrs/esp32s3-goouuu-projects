#pragma once

#include "esp_err.h"

/* INMP441 I2S microphone driver.
 *   WS  -> PIN_MIC_WS,  SCK -> PIN_MIC_BCLK,  SD -> PIN_MIC_DATA
 *   L/R -> GND (left channel selected)
 */

esp_err_t mic_init(void);

/* Capture one AUDIO_FRAME_MS window and report its loudness.
 * Both values are normalised against the 24-bit full scale the INMP441
 * delivers, so 0.0 is silence and 1.0 is clipping. */
esp_err_t mic_read_frame(float *rms, float *peak);
