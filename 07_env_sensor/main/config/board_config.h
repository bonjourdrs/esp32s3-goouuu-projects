#pragma once

/* GOOUUU ESP32-S3 baseboard wiring. Every GPIO in this project comes from
 * here; business code must not contain raw pin numbers. */

/* Sensor I2C bus. AHT20, BMP280 and the MPU6050 all hang off this one.
 * The OLED header uses a different bus (GPIO41/42) - never mix them up. */
#define PIN_I2C_SDA 8
#define PIN_I2C_SCL 9

/* WS2812 addressable RGB LED. */
#define PIN_LED_RGB 48

/* I2C clock. The sensors handle 400 kHz, and the bus is short. */
#define I2C_CLOCK_HZ 400000

/* AHT20 needs ~80 ms per conversion, so 2 s is comfortable and keeps the
 * serial log readable. */
#define ENV_READ_PERIOD_MS 2000
