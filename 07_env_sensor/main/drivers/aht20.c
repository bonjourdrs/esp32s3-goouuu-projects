#include "aht20.h"

#include <math.h>

#include "board_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "esp_log.h"

static const char *TAG = "AHT20";

#define AHT20_ADDR           0x38
#define AHT20_CMD_CALIBRATE  0xBE  /* followed by 0x08 0x00 */
#define AHT20_CMD_TRIGGER    0xAC  /* followed by 0x33 0x00 */
#define AHT20_CMD_SOFT_RESET 0xBA
#define AHT20_CMD_STATUS     0x71

#define AHT20_STATUS_BUSY     0x80
#define AHT20_STATUS_CALIBRAT 0x08

/* The datasheet asks for 80 ms; a little margin costs nothing at a 2 s rate. */
#define AHT20_CONVERSION_MS   90

#define AHT20_RAW_SCALE       1048576.0f  /* 2^20 */

static i2c_master_dev_handle_t s_dev;

static esp_err_t write_bytes(const uint8_t *buf, size_t len)
{
    return i2c_master_transmit(s_dev, buf, len, 100);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

esp_err_t aht20_init(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AHT20_ADDR,
        .scl_speed_hz = I2C_CLOCK_HZ,
    };

    esp_err_t err = i2c_master_bus_add_device(i2c_bus_handle(), &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
        return err;
    }

    /* The part needs a moment after power-on before it accepts commands. */
    vTaskDelay(pdMS_TO_TICKS(40));

    const uint8_t reset[] = { AHT20_CMD_SOFT_RESET };
    write_bytes(reset, sizeof(reset));
    vTaskDelay(pdMS_TO_TICKS(20));

    const uint8_t calibrate[] = { AHT20_CMD_CALIBRATE, 0x08, 0x00 };
    err = write_bytes(calibrate, sizeof(calibrate));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no answer at 0x%02x: %s", AHT20_ADDR, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t status = 0;
    if (read_reg(AHT20_CMD_STATUS, &status, 1) == ESP_OK) {
        ESP_LOGI(TAG, "status=0x%02x calibrated=%d",
                 status, (status & AHT20_STATUS_CALIBRAT) ? 1 : 0);
        if ((status & AHT20_STATUS_CALIBRAT) == 0) {
            ESP_LOGW(TAG, "not calibrated - the first readings may drift");
        }
    }

    ESP_LOGI(TAG, "ready at 0x%02x", AHT20_ADDR);
    return ESP_OK;
}

esp_err_t aht20_read(float *temperature_c, float *humidity_pct)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t trigger[] = { AHT20_CMD_TRIGGER, 0x33, 0x00 };
    esp_err_t err = write_bytes(trigger, sizeof(trigger));
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(AHT20_CONVERSION_MS));

    /* 7 bytes: status, then 20-bit humidity and 20-bit temperature packed
     * across the remaining 6 bytes. */
    uint8_t raw[7] = { 0 };
    err = read_reg(0x00, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    if (raw[0] & AHT20_STATUS_BUSY) {
        ESP_LOGW(TAG, "still busy, dropping this sample");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t raw_hum = ((uint32_t)raw[1] << 12) |
                       ((uint32_t)raw[2] << 4) |
                       ((uint32_t)raw[3] >> 4);

    uint32_t raw_temp = ((uint32_t)(raw[3] & 0x0F) << 16) |
                        ((uint32_t)raw[4] << 8) |
                        raw[5];

    *humidity_pct = (float)raw_hum * 100.0f / AHT20_RAW_SCALE;
    *temperature_c = (float)raw_temp * 200.0f / AHT20_RAW_SCALE - 50.0f;

    if (*humidity_pct < 0.0f || *humidity_pct > 100.0f) {
        ESP_LOGW(TAG, "humidity out of range: %.1f %%", *humidity_pct);
    }

    return ESP_OK;
}
