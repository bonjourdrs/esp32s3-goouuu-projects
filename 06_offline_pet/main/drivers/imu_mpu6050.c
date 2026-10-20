#include "imu_mpu6050.h"

#include <math.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_config.h"

#define TAG "IMU"

#define IMU_REG_WHO_AM_I     0x75
#define IMU_REG_ACCEL_XOUT_H 0x3b

/* Registers 0x1c = 0x10 selects +/-8 g, which is 4096 LSB per g. The extra
 * headroom matters because a hard tap would clip a +/-4 g range. */
#define IMU_LSB_PER_G   4096.0f
/* 0x1b = 0x00 is +/-250 deg/s, or 131 LSB per deg/s. */
#define IMU_LSB_PER_DPS 131.0f
#define DEG_PER_RAD     57.29577951f

static i2c_master_dev_handle_t s_imu;

static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t n)
{
    return i2c_master_transmit_receive(s_imu, &reg, 1, data, n, 30);
}

static esp_err_t reg_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[] = { reg, value };
    return i2c_master_transmit(s_imu, buf, sizeof(buf), 30);
}

esp_err_t imu_init(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_IMU_SDA,
        .scl_io_num = PIN_IMU_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    /* The sensor shares the 3V3 rail with the amplifier, and the very first
     * probe after power-on sometimes comes back empty. A handful of retries
     * makes it reliable without hiding a genuinely missing part. */
    for (int attempt = 0; attempt < 5; ++attempt) {
        for (int addr = 0x68; addr <= 0x69; ++addr) {
            if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
                continue;
            }

            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = addr,
                /* The motion loop samples at 200 Hz; 400 kHz keeps the bus well
                 * below saturation. */
                .scl_speed_hz = 400000,
            };
            if (i2c_master_bus_add_device(bus, &dev_cfg, &s_imu) != ESP_OK) {
                s_imu = NULL;
                continue;
            }

            uint8_t who = 0;
            if (reg_read(IMU_REG_WHO_AM_I, &who, 1) == ESP_OK && (who == 0x68 || who == 0x70)) {
                ESP_LOGI(TAG, "%s ready  SDA=GPIO%d SCL=GPIO%d addr=0x%02x WHO_AM_I=0x%02x",
                         who == 0x70 ? "MPU6500" : "MPU6050",
                         PIN_IMU_SDA, PIN_IMU_SCL, addr, who);

                ESP_ERROR_CHECK(reg_write(0x6b, 0x80)); /* device reset */
                vTaskDelay(pdMS_TO_TICKS(100));

                static const uint8_t setup[][2] = {
                    { 0x6b, 0x01 }, /* wake, PLL from X gyro reference */
                    { 0x6c, 0x00 }, /* enable all axes */
                    { 0x1a, 0x03 }, /* DLPF: gyro bandwidth 44 Hz */
                    { 0x19, 0x04 }, /* 1 kHz / (4 + 1) = 200 Hz sample rate */
                    { 0x1b, 0x00 }, /* gyro +/-250 deg/s */
                    { 0x1c, 0x10 }, /* accelerometer +/-8 g */
                };
                for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); ++i) {
                    err = reg_write(setup[i][0], setup[i][1]);
                    if (err != ESP_OK) {
                        return err;
                    }
                }

                if (who == 0x70) {
                    /* The MPU6500 keeps its accelerometer filter in a register the
                     * MPU6050 does not have. */
                    err = reg_write(0x1d, 0x03);
                    if (err != ESP_OK) {
                        return err;
                    }
                }

                vTaskDelay(pdMS_TO_TICKS(100));
                return ESP_OK;
            }

            ESP_LOGW(TAG, "unexpected device at 0x%02x: WHO_AM_I=0x%02x", addr, who);
            i2c_master_bus_rm_device(s_imu);
            s_imu = NULL;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    i2c_del_master_bus(bus);
    ESP_LOGE(TAG, "no MPU6050/MPU6500 found; check 3V3 / GND / SDA=GPIO%d / SCL=GPIO%d",
             PIN_IMU_SDA, PIN_IMU_SCL);
    return ESP_ERR_NOT_FOUND;
}

static int16_t be16(const uint8_t *p)
{
    return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

esp_err_t imu_read(imu_sample_t *out)
{
    if (s_imu == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[14];
    esp_err_t err = reg_read(IMU_REG_ACCEL_XOUT_H, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    out->ax = be16(&raw[0]) / IMU_LSB_PER_G;
    out->ay = be16(&raw[2]) / IMU_LSB_PER_G;
    out->az = be16(&raw[4]) / IMU_LSB_PER_G;
    out->gx = be16(&raw[8]) / IMU_LSB_PER_DPS;
    out->gy = be16(&raw[10]) / IMU_LSB_PER_DPS;
    out->gz = be16(&raw[12]) / IMU_LSB_PER_DPS;

    out->accel_mag = sqrtf(out->ax * out->ax + out->ay * out->ay + out->az * out->az);

    out->roll = atan2f(out->ay, out->az) * DEG_PER_RAD;
    out->pitch = atan2f(-out->ax, sqrtf(out->ay * out->ay + out->az * out->az)) * DEG_PER_RAD;
    return ESP_OK;
}
