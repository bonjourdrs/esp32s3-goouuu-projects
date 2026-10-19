#include <math.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define SAMPLE_RATE 24000
#define FRAMES 240
#define TWO_PI 6.28318530718f
#define NOISE_GATE_G 0.08f
#define FULL_SHAKE_G 1.5f
#define MAX_AMPLITUDE 8000.0f
static const char *TAG = "SHAKE";
static i2c_master_dev_handle_t imu;
static QueueHandle_t levels;
typedef struct { float level; int64_t time_us; } sound_control_t;

static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t n)
{
    return i2c_master_transmit_receive(imu, &reg, 1, data, n, 30);
}
static esp_err_t reg_write(uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(imu, data, sizeof(data), 30);
}
static esp_err_t imu_init(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_NUM_8,
        .scl_io_num = GPIO_NUM_9, .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) return err;
    for (int addr = 0x68; addr <= 0x69; ++addr) {
        if (i2c_master_probe(bus, addr, 50) != ESP_OK) continue;
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr, .scl_speed_hz = 100000,
        };
        err = i2c_master_bus_add_device(bus, &dev, &imu);
        if (err != ESP_OK) return err;
        uint8_t id = 0;
        if (reg_read(0x75, &id, 1) == ESP_OK && (id == 0x68 || id == 0x70)) {
            ESP_LOGI(TAG, "%s found: SDA=8 SCL=9 addr=0x%02x WHO_AM_I=0x%02x", id == 0x70 ? "MPU6500" : "MPU6050", addr, id);
            err = reg_write(0x6b, 0x80); // Device reset.
            if (err != ESP_OK) return err;
            vTaskDelay(pdMS_TO_TICKS(100));
            const uint8_t setup[][2] = {
                {0x6b, 0x01}, // Wake; PLL using X gyro reference.
                {0x6c, 0x00}, // Enable axes.
                {0x1a, 0x03}, // DLPF; accelerometer bandwidth 44 Hz.
                {0x19, 0x09}, // 100 Hz sample rate.
                {0x1b, 0x00}, // Gyro +/-250 deg/s.
                {0x1c, 0x08}, // Accelerometer +/-4 g, 8192 LSB/g.
            };
            for (unsigned i = 0; i < sizeof(setup)/sizeof(setup[0]); ++i) {
                err = reg_write(setup[i][0], setup[i][1]);
                if (err != ESP_OK) return err;
            }
            if (id == 0x70) {
                // MPU6500 has a separate accelerometer filter register (41 Hz).
                err = reg_write(0x1d, 0x03);
                if (err != ESP_OK) return err;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Unexpected device at 0x%02x: id=0x%02x", addr, id);
        ESP_ERROR_CHECK(i2c_master_bus_rm_device(imu));
        imu = NULL;
    }
    ESP_ERROR_CHECK(i2c_del_master_bus(bus));
    return ESP_ERR_NOT_FOUND;
}

static void audio_task(void *arg)
{
    i2s_chan_handle_t tx;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    channel.dma_desc_num = 4;
    channel.dma_frame_num = FRAMES;
    ESP_ERROR_CHECK(i2s_new_channel(&channel, &tx, NULL));
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = GPIO_NUM_15,
                      .ws = GPIO_NUM_16, .dout = GPIO_NUM_7, .din = I2S_GPIO_UNUSED },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
    ESP_LOGI(TAG, "Audio ready: BCLK=15 LRC=16 DIN=7; shake to play");
    int16_t pcm[FRAMES * 2];
    float phase = 0, amplitude = 0, frequency = 220;
    sound_control_t control = {0};
    while (1) {
        sound_control_t latest;
        if (xQueueReceive(levels, &latest, 0) == pdTRUE) control = latest;
        // A stalled or disconnected sensor must not leave a sustained tone.
        float level = esp_timer_get_time() - control.time_us < 250000 ? control.level : 0;
        float target_amp = MAX_AMPLITUDE * level;
        float target_hz = 220.0f + 1540.0f * level;
        for (int i = 0; i < FRAMES; ++i) {
            amplitude += 0.003f * (target_amp - amplitude);
            frequency += 0.002f * (target_hz - frequency);
            int16_t sample = (int16_t)(sinf(phase) * amplitude);
            pcm[i*2] = pcm[i*2+1] = sample;
            phase += TWO_PI * frequency / SAMPLE_RATE;
            if (phase >= TWO_PI) phase -= TWO_PI;
        }
        size_t offset = 0;
        while (offset < sizeof(pcm)) {
            size_t written = 0;
            esp_err_t err = i2s_channel_write(tx, (uint8_t *)pcm + offset,
                                            sizeof(pcm) - offset, &written, 100);
            if (err != ESP_OK || written == 0) {
                ESP_LOGE(TAG, "Audio write failed: %s", esp_err_to_name(err));
                i2s_channel_disable(tx);
                vTaskDelete(NULL);
                return;
            }
            offset += written;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "04_mpu6050_sound; keep still briefly at startup");
    esp_err_t err = imu_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MPU6050 init failed: %s. Check 3V3/GND/SDA8/SCL9; reset after fixing.", esp_err_to_name(err));
        return;
    }
    levels = xQueueCreate(1, sizeof(sound_control_t));
    if (!levels || xTaskCreate(audio_task, "shake_audio", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot allocate audio task/queue");
        return;
    }
    float gravity[3] = {0};
    float envelope = 0;
    bool first = true;
    unsigned count = 0, errors = 0;
    TickType_t wake = xTaskGetTickCount();
    while (1) {
        uint8_t raw[6];
        err = reg_read(0x3b, raw, sizeof(raw));
        float energy = 0;
        if (err == ESP_OK) {
            errors = 0;
            for (int axis = 0; axis < 3; ++axis) {
                int16_t value = (int16_t)(((uint16_t)raw[2*axis] << 8) | raw[2*axis+1]);
                float accel = value / 8192.0f;
                if (first) gravity[axis] = accel;
                gravity[axis] += 0.04f * (accel - gravity[axis]);
                float dynamic = accel - gravity[axis];
                energy += dynamic * dynamic;
            }
            first = false;
            float target = fminf(1.0f, fmaxf(0.0f, (sqrtf(energy) - NOISE_GATE_G) / FULL_SHAKE_G));
            envelope += (target > envelope ? 0.55f : 0.10f) * (target - envelope);
            if (envelope < 0.005f) envelope = 0;
        } else {
            envelope = 0;
            first = true;
            if (++errors == 1 || errors % 50 == 0)
                ESP_LOGW(TAG, "Sensor read failed: %s; muted", esp_err_to_name(err));
        }
        sound_control_t control = { .level = envelope, .time_us = esp_timer_get_time() };
        xQueueOverwrite(levels, &control);
        if (++count % 25 == 0)
            ESP_LOGI(TAG, "motion=%.3fg level=%.2f tone=%.0fHz%s", sqrtf(energy), envelope,
                     220.0f + 1540.0f * envelope, envelope == 0 ? " (silent)" : "");
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
    }
}
