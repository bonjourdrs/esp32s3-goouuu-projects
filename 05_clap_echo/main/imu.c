#include "imu.h"

#include <math.h>
#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "IMU"

/* MPU6050 pin map on the GOOUUU ESP32-S3 baseboard. */
#define IMU_SDA_GPIO GPIO_NUM_8
#define IMU_SCL_GPIO GPIO_NUM_9

#define IMU_REG_WHO_AM_I     0x75
#define IMU_REG_ACCEL_XOUT_H 0x3b

/* Register 0x1c is set to +/-4 g, which is 8192 LSB per g. */
#define IMU_LSB_PER_G    8192.0f
#define IMU_GRAVITY_ALPHA 0.04f
#define DEG_PER_RAD      57.29577951f

static i2c_master_dev_handle_t s_imu;
static float s_gravity[3];
static bool  s_seeded;

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
        .sda_io_num = IMU_SDA_GPIO,
        .scl_io_num = IMU_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    for (int addr = 0x68; addr <= 0x69; ++addr) {
        if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
            continue;
        }

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(bus, &dev_cfg, &s_imu) != ESP_OK) {
            s_imu = NULL;
            break;
        }

        uint8_t who = 0;
        if (reg_read(IMU_REG_WHO_AM_I, &who, 1) == ESP_OK && (who == 0x68 || who == 0x70)) {
            ESP_LOGI(TAG, "%s ready  SDA=GPIO%d SCL=GPIO%d addr=0x%02x WHO_AM_I=0x%02x",
                     who == 0x70 ? "MPU6500" : "MPU6050",
                     IMU_SDA_GPIO, IMU_SCL_GPIO, addr, who);

            ESP_ERROR_CHECK(reg_write(0x6b, 0x80)); /* device reset */
            vTaskDelay(pdMS_TO_TICKS(100));

            static const uint8_t setup[][2] = {
                { 0x6b, 0x01 }, /* wake, PLL from X gyro reference */
                { 0x6c, 0x00 }, /* enable all axes */
                { 0x1a, 0x03 }, /* DLPF: accelerometer bandwidth 44 Hz */
                { 0x19, 0x09 }, /* 100 Hz sample rate */
                { 0x1b, 0x00 }, /* gyro +/-250 deg/s */
                { 0x1c, 0x08 }, /* accelerometer +/-4 g */
            };
            for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); ++i) {
                err = reg_write(setup[i][0], setup[i][1]);
                if (err != ESP_OK) {
                    return err;
                }
            }

            if (who == 0x70) {
                /* The MPU6500 keeps its accelerometer filter in a separate register. */
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

    i2c_del_master_bus(bus);
    ESP_LOGE(TAG, "no MPU6050/MPU6500 found; check 3V3 / GND / SDA=GPIO8 / SCL=GPIO9");
    return ESP_ERR_NOT_FOUND;
}

float imu_tilt_deg(void)
{
    if (s_imu == NULL) {
        return 0.0f;
    }

    uint8_t raw[6];
    if (reg_read(IMU_REG_ACCEL_XOUT_H, raw, sizeof(raw)) != ESP_OK) {
        return 0.0f;
    }

    float accel[3];
    for (int axis = 0; axis < 3; ++axis) {
        int16_t value = (int16_t)(((uint16_t)raw[2 * axis] << 8) | raw[2 * axis + 1]);
        accel[axis] = value / IMU_LSB_PER_G;
    }

    if (!s_seeded) {
        for (int axis = 0; axis < 3; ++axis) {
            s_gravity[axis] = accel[axis];
        }
        s_seeded = true;
    }
    for (int axis = 0; axis < 3; ++axis) {
        s_gravity[axis] += IMU_GRAVITY_ALPHA * (accel[axis] - s_gravity[axis]);
    }

    /* Low-passed gravity keeps the reading steady while the board is shaken. */
    return atan2f(s_gravity[0], s_gravity[2]) * DEG_PER_RAD;
}
