#pragma once

#include "esp_err.h"

/* AHT20 temperature + humidity sensor, fixed I2C address 0x38.
 * On the GOOUUU baseboard it sits on the same bus as the BMP280 and MPU6050. */

esp_err_t aht20_init(void);

/* Triggers a conversion and blocks for the ~80 ms it takes, then decodes.
 * Returns temperature in degC and relative humidity in percent. */
esp_err_t aht20_read(float *temperature_c, float *humidity_pct);
