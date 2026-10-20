#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/* Shared I2C master bus for the on-board sensor cluster (GPIO8/GPIO9). */

esp_err_t i2c_bus_init(void);

i2c_master_bus_handle_t i2c_bus_handle(void);

/* Probe every 7-bit address and log what answers. Run once at boot: it tells
 * you immediately whether the sensors are actually populated. */
void i2c_bus_scan(void);
