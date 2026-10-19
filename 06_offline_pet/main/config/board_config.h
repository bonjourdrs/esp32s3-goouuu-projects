#pragma once

/* GOOUUU ESP32-S3 baseboard wiring. Every GPIO in this project comes from
 * here; business code must not contain raw pin numbers. */

/* INMP441 digital microphone (L/R pin tied to GND -> left channel). */
#define PIN_MIC_WS    4
#define PIN_MIC_BCLK  5
#define PIN_MIC_DATA  6

/* MAX98357 I2S amplifier (SD tied to 3V3, GAIN tied to GND = 9 dB). */
#define PIN_SPK_DATA  7
#define PIN_SPK_BCLK  15
#define PIN_SPK_LRCLK 16

/* MPU6050 / MPU6500 on the sensor I2C bus (GPIO41/42 is the OLED bus). */
#define PIN_IMU_SDA   8
#define PIN_IMU_SCL   9

/* WS2812 addressable RGB LED. */
#define PIN_LED_RGB   48

/* Both capture and playback run at 16 kHz so the recorded clips play back
 * without resampling. The two I2S controllers have independent clocks, so
 * they may differ, but there is no reason to here. */
#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_FRAME_MS      20
#define AUDIO_FRAME_SAMPLES (AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS / 1000)
