#include "bmp280.h"

#include "board_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "esp_log.h"

static const char *TAG = "BMP280";

#define BMP280_ADDR_PRIMARY   0x76
#define BMP280_ADDR_SECONDARY 0x77

#define BMP280_REG_ID        0xD0
#define BMP280_REG_RESET     0xE0
#define BMP280_REG_STATUS    0xF3
#define BMP280_REG_CTRL_MEAS 0xF4
#define BMP280_REG_CONFIG    0xF5
#define BMP280_REG_PRESS     0xF7
#define BMP280_REG_CALIB     0x88

#define BMP280_CHIP_ID       0x58
#define BMP280_RESET_WORD    0xB6

/* osrs_t = x2, osrs_p = x16, mode = forced. One conversion on demand instead
 * of a free-running measurement, which matches a polled demo. */
#define BMP280_CTRL_FORCED   0x55
/* IIR filter x4. Standby time is ignored in forced mode. */
#define BMP280_CONFIG_FILTER 0x08

/* x16 pressure oversampling needs up to ~43 ms. */
#define BMP280_CONVERSION_MS 50

static i2c_master_dev_handle_t s_dev;

static uint16_t dig_T1;
static int16_t dig_T2, dig_T3;
static uint16_t dig_P1;
static int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;

/* Shared between the temperature and pressure steps: the pressure formula
 * needs the temperature result. */
static int32_t s_t_fine;

static esp_err_t read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static void load_calibration(const uint8_t *c)
{
    dig_T1 = (uint16_t)((c[1] << 8) | c[0]);
    dig_T2 = (int16_t)((c[3] << 8) | c[2]);
    dig_T3 = (int16_t)((c[5] << 8) | c[4]);

    dig_P1 = (uint16_t)((c[7] << 8) | c[6]);
    dig_P2 = (int16_t)((c[9] << 8) | c[8]);
    dig_P3 = (int16_t)((c[11] << 8) | c[10]);
    dig_P4 = (int16_t)((c[13] << 8) | c[12]);
    dig_P5 = (int16_t)((c[15] << 8) | c[14]);
    dig_P6 = (int16_t)((c[17] << 8) | c[16]);
    dig_P7 = (int16_t)((c[19] << 8) | c[18]);
    dig_P8 = (int16_t)((c[21] << 8) | c[20]);
    dig_P9 = (int16_t)((c[23] << 8) | c[22]);
}

esp_err_t bmp280_init(void)
{
    uint8_t addr = 0;
    if (i2c_master_probe(i2c_bus_handle(), BMP280_ADDR_PRIMARY, 50) == ESP_OK) {
        addr = BMP280_ADDR_PRIMARY;
    } else if (i2c_master_probe(i2c_bus_handle(), BMP280_ADDR_SECONDARY, 50) == ESP_OK) {
        addr = BMP280_ADDR_SECONDARY;
    } else {
        ESP_LOGE(TAG, "not found at 0x%02x or 0x%02x",
                 BMP280_ADDR_PRIMARY, BMP280_ADDR_SECONDARY);
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_CLOCK_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(i2c_bus_handle(), &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t id = 0;
    if (read_reg(BMP280_REG_ID, &id, 1) != ESP_OK || id != BMP280_CHIP_ID) {
        ESP_LOGE(TAG, "0x%02x is not a BMP280 (chip id 0x%02x, expected 0x%02x)",
                 addr, id, BMP280_CHIP_ID);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    /* The reset returns the part to its power-on defaults, so calibration has
     * to be re-read afterwards. */
    write_reg(BMP280_REG_RESET, BMP280_RESET_WORD);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t calib[26] = { 0 };
    err = read_reg(BMP280_REG_CALIB, calib, sizeof(calib));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "calibration read failed: %s", esp_err_to_name(err));
        return err;
    }
    load_calibration(calib);

    write_reg(BMP280_REG_CONFIG, BMP280_CONFIG_FILTER);
    write_reg(BMP280_REG_CTRL_MEAS, BMP280_CTRL_FORCED);

    ESP_LOGI(TAG, "ready at 0x%02x  chip id=0x%02x  dig_T1=%u dig_P1=%u",
             addr, id, dig_T1, dig_P1);
    return ESP_OK;
}

/* Integer compensation straight out of the BMP280 datasheet (section 3.11.3).
 * Kept in fixed point on purpose: it is exact and costs no float maths. */
static float compensate_temperature(int32_t adc_T)
{
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)dig_T1)) *
                      ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) * ((int32_t)dig_T3)) >> 14;

    s_t_fine = var1 + var2;
    return (float)((s_t_fine * 5 + 128) >> 8) / 100.0f;  /* the raw value is 0.01 degC */
}

static float compensate_pressure(int32_t adc_P)
{
    int64_t var1 = ((int64_t)s_t_fine) - 128000;
    int64_t var2 = var1 * var1 * (int64_t)dig_P6;
    var2 = var2 + ((var1 * (int64_t)dig_P5) << 17);
    var2 = var2 + (((int64_t)dig_P4) << 35);

    var1 = ((var1 * var1 * (int64_t)dig_P3) >> 8) + ((var1 * (int64_t)dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)dig_P1) >> 33;

    if (var1 == 0) {
        return 0.0f;  /* guards against a division by zero on a dead part */
    }

    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;

    var1 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)dig_P7) << 4);

    /* The result is in Q24.8 Pa: 256 counts per pascal. */
    return (float)p / 256.0f / 100.0f;
}

esp_err_t bmp280_read(float *temperature_c, float *pressure_hpa)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = write_reg(BMP280_REG_CTRL_MEAS, BMP280_CTRL_FORCED);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(BMP280_CONVERSION_MS));

    /* 0xF7..0xFC: pressure then temperature, 20 bits each, big endian with the
     * low nibble in the third byte. */
    uint8_t raw[6] = { 0 };
    err = read_reg(BMP280_REG_PRESS, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    int32_t adc_P = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | ((int32_t)raw[2] >> 4);
    int32_t adc_T = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) | ((int32_t)raw[5] >> 4);

    *temperature_c = compensate_temperature(adc_T);
    *pressure_hpa = compensate_pressure(adc_P);
    return ESP_OK;
}
