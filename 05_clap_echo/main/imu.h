#pragma once

#include "esp_err.h"

/* MPU6050 / MPU6500 six-axis sensor on the GOOUUU baseboard:
 *   SDA -> GPIO8
 *   SCL -> GPIO9
 *   address 0x68 or 0x69
 *
 * The board fitted here reports WHO_AM_I = 0x70, which is an MPU6500.
 * Both parts share the register map used by this module.
 */

esp_err_t imu_init(void);

/* Tilt of the board around its X axis, in degrees.
 * 0 means flat, +90 means the X axis points straight up. */
float imu_tilt_deg(void);
