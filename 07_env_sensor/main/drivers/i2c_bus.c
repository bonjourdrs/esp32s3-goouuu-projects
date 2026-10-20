#include "i2c_bus.h"

#include "board_config.h"
#include "esp_log.h"

static const char *TAG = "I2C";
static i2c_master_bus_handle_t s_bus;

esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "master ready  SDA=GPIO%d SCL=GPIO%d %d Hz",
             PIN_I2C_SDA, PIN_I2C_SCL, I2C_CLOCK_HZ);
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_handle(void)
{
    return s_bus;
}

void i2c_bus_scan(void)
{
    ESP_LOGI(TAG, "scanning bus ...");
    int found = 0;

    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            const char *name = "unknown";
            switch (addr) {
            case 0x38: name = "AHT20";   break;
            case 0x68: name = "MPU6050"; break;
            case 0x69: name = "MPU6050 (alt)"; break;
            case 0x76: name = "BMP280";  break;
            case 0x77: name = "BMP280 (alt)";  break;
            default: break;
            }
            ESP_LOGI(TAG, "  0x%02x  %s", addr, name);
            ++found;
        }
    }

    if (found == 0) {
        ESP_LOGW(TAG, "  nothing answered - check 3V3 / GND / SDA=GPIO%d / SCL=GPIO%d",
                 PIN_I2C_SDA, PIN_I2C_SCL);
    } else {
        ESP_LOGI(TAG, "%d device(s) found", found);
    }
}
