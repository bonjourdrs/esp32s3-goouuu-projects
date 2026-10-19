#pragma once

#include "esp_err.h"

/* MPU6050 / MPU6500 driver.
 *   SDA -> PIN_IMU_SDA,  SCL -> PIN_IMU_SCL
 *
 * The board carries an MPU6500 (WHO_AM_I = 0x70) even though the silkscreen
 * and the schematic say MPU6050, so both parts are accepted. */

typedef struct {
    float ax, ay, az;  /* g */
    float gx, gy, gz;  /* deg/s */
    float accel_mag;   /* g, magnitude of the raw accelerometer vector */
    float pitch, roll; /* deg, derived from the accelerometer only */
} imu_sample_t;

esp_err_t imu_init(void);

/* Read acceleration and rotation in one burst. */
esp_err_t imu_read(imu_sample_t *out);
