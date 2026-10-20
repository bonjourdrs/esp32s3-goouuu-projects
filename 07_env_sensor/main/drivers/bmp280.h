#pragma once

#include "esp_err.h"

/* BMP280 barometric pressure + temperature sensor.
 * Address is 0x76 or 0x77 depending on the SDO pin; both are probed. */

esp_err_t bmp280_init(void);

/* Triggers one forced-mode conversion, waits for it, and decodes.
 * Returns temperature in degC and pressure in hPa. */
esp_err_t bmp280_read(float *temperature_c, float *pressure_hpa);
