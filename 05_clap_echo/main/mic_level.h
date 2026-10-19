#pragma once

#include "esp_err.h"

/* INMP441 I2S microphone on the GOOUUU baseboard:
 *   WS  -> GPIO4
 *   SCK -> GPIO5
 *   SD  -> GPIO6
 *   L/R -> GND (left channel selected)
 */

esp_err_t mic_level_init(void);

/* Normalised loudness of the last measurement window:
 * 0.0 = silence, 1.0 = loud. */
float mic_level_read(void);
