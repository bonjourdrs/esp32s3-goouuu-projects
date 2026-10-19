#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* MAX98357 I2S amplifier driver.
 *   DIN -> PIN_SPK_DATA,  BCLK -> PIN_SPK_BCLK,  LRC -> PIN_SPK_LRCLK
 */

esp_err_t speaker_init(void);

/* Stream `count` mono 16-bit frames, duplicated into both stereo slots.
 * Blocks until the I2S DMA has taken all of them. */
esp_err_t speaker_write(const int16_t *frames, size_t count);
