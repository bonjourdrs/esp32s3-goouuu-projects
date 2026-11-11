#include "dev_cfg.h"

#include "esp_log.h"
#include "nvs.h"

#define NVS_NAMESPACE   "unlock_cfg"
#define NVS_KEY_USB     "usb_on"
#define NVS_KEY_BLE     "ble_on"

static const char *TAG = "CFG";

static volatile bool s_usb_on = true;
static volatile bool s_ble_on = true;

esp_err_t dev_cfg_init(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t v = 1;
        if (nvs_get_u8(handle, NVS_KEY_USB, &v) == ESP_OK) {
            s_usb_on = (v != 0);
        }
        v = 1;
        if (nvs_get_u8(handle, NVS_KEY_BLE, &v) == ESP_OK) {
            s_ble_on = (v != 0);
        }
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "通路配置：USB %s，蓝牙 %s",
             s_usb_on ? "开" : "关", s_ble_on ? "开" : "关");
    return ESP_OK;
}

bool dev_cfg_usb_enabled(void)
{
    return s_usb_on;
}

bool dev_cfg_ble_enabled(void)
{
    return s_ble_on;
}

static esp_err_t set_flag(const char *key, bool enabled, volatile bool *dst)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 失败：%s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, key, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入 %s 失败：%s", key, esp_err_to_name(err));
        return err;
    }

    *dst = enabled;
    return ESP_OK;
}

esp_err_t dev_cfg_set_usb(bool enabled)
{
    return set_flag(NVS_KEY_USB, enabled, &s_usb_on);
}

esp_err_t dev_cfg_set_ble(bool enabled)
{
    return set_flag(NVS_KEY_BLE, enabled, &s_ble_on);
}

void dev_cfg_reset(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, NVS_KEY_USB);
        nvs_erase_key(handle, NVS_KEY_BLE);
        nvs_commit(handle);
        nvs_close(handle);
    }
    s_usb_on = true;
    s_ble_on = true;
    ESP_LOGI(TAG, "通路配置已恢复默认（都开启）");
}
