#include "pin_store.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE   "unlock_cfg"
#define NVS_KEY_PIN     "pin"
#define NVS_KEY_PWD     "pwd"
#define NVS_KEY_MODE    "mode"

static const char *TAG = "CRED";

static SemaphoreHandle_t s_lock;
static char s_pin[PIN_MAX_LEN + 1];
static char s_pwd[PWD_MAX_LEN + 1];
static unlock_mode_t s_mode = UNLOCK_MODE_NONE;

bool pin_store_pin_valid(const char *pin)
{
    if (pin == NULL) {
        return false;
    }
    size_t len = strlen(pin);
    if (len < PIN_MIN_LEN || len > PIN_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            return false;
        }
    }
    return true;
}

bool pin_store_pwd_valid(const char *pwd)
{
    if (pwd == NULL) {
        return false;
    }
    size_t len = strlen(pwd);
    if (len < PWD_MIN_LEN || len > PWD_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)pwd[i];
        if (c < 0x20 || c > 0x7E) {      /* 只收 ASCII 可打印字符 */
            return false;
        }
    }
    return true;
}

static void take_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void give_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/* 读字符串键；不存在或非法则返回空串 */
static void load_str(nvs_handle_t handle, const char *key, char *out,
                     size_t out_len, bool enforce, bool (*valid)(const char *))
{
    out[0] = '\0';
    size_t len = out_len;
    if (nvs_get_str(handle, key, out, &len) == ESP_OK) {
        if ((enforce && !valid(out)) || strlen(out) >= out_len) {
            out[0] = '\0';
        }
    } else {
        out[0] = '\0';
    }
}

esp_err_t pin_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新分区，擦除后重试");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败：%s", esp_err_to_name(err));
        return err;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        load_str(handle, NVS_KEY_PIN, s_pin, sizeof(s_pin), true, pin_store_pin_valid);
        load_str(handle, NVS_KEY_PWD, s_pwd, sizeof(s_pwd), true, pin_store_pwd_valid);

        uint8_t mode = UNLOCK_MODE_NONE;
        bool has_mode = (nvs_get_u8(handle, NVS_KEY_MODE, &mode) == ESP_OK);
        if (!has_mode || mode > UNLOCK_MODE_PASSWORD) {
            mode = UNLOCK_MODE_NONE;
        }
        nvs_close(handle);
        s_mode = (unlock_mode_t)mode;

        /* 旧固件只存 PIN、从不写 mode 键（含旧版内置的默认 PIN）。
         * 新规则要求先选解锁方式，旧数据留着只会让初始状态看起来“有 PIN”，直接清空。 */
        if (!has_mode && (s_pin[0] != '\0' || s_pwd[0] != '\0')) {
            nvs_handle_t rw;
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &rw) == ESP_OK) {
                nvs_erase_key(rw, NVS_KEY_PIN);
                nvs_erase_key(rw, NVS_KEY_PWD);
                nvs_commit(rw);
                nvs_close(rw);
            }
            s_pin[0] = '\0';
            s_pwd[0] = '\0';
            ESP_LOGW(TAG, "检测到旧版固件遗留的凭据，已清空，请到配置网页重新设置");
        }
    } else {
        s_pin[0] = '\0';
        s_pwd[0] = '\0';
        s_mode = UNLOCK_MODE_NONE;
    }

    /* 模式与内容必须一致，否则视为未设置 */
    if ((s_mode == UNLOCK_MODE_PIN && s_pin[0] == '\0') ||
        (s_mode == UNLOCK_MODE_PASSWORD && s_pwd[0] == '\0')) {
        ESP_LOGW(TAG, "解锁方式与已存凭据不匹配，重置为未设置");
        s_mode = UNLOCK_MODE_NONE;
    }

    ESP_LOGI(TAG, "解锁方式：%s（PIN %s，密码 %s）",
             s_mode == UNLOCK_MODE_PIN ? "电脑 PIN" :
             s_mode == UNLOCK_MODE_PASSWORD ? "电脑密码" : "未设置",
             s_pin[0] ? "已保存" : "空",
             s_pwd[0] ? "已保存" : "空");
    return ESP_OK;
}

unlock_mode_t pin_store_get_mode(void)
{
    return s_mode;
}

bool pin_store_has_pin(void)
{
    return s_pin[0] != '\0';
}

bool pin_store_has_password(void)
{
    return s_pwd[0] != '\0';
}

esp_err_t pin_store_get_pin(char *out, size_t out_len)
{
    if (out == NULL || out_len < PIN_MAX_LEN + 1) {
        return ESP_ERR_INVALID_ARG;
    }
    take_lock();
    strlcpy(out, s_pin, out_len);
    give_lock();
    return ESP_OK;
}

esp_err_t pin_store_get_password(char *out, size_t out_len)
{
    if (out == NULL || out_len < PWD_MAX_LEN + 1) {
        return ESP_ERR_INVALID_ARG;
    }
    take_lock();
    strlcpy(out, s_pwd, out_len);
    give_lock();
    return ESP_OK;
}

esp_err_t pin_store_set_credentials(const char *pin, const char *pwd, unlock_mode_t mode)
{
    if (pin == NULL) {
        pin = "";
    }
    if (pwd == NULL) {
        pwd = "";
    }
    if (pin[0] != '\0' && !pin_store_pin_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pwd[0] != '\0' && !pin_store_pwd_valid(pwd)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (mode == UNLOCK_MODE_PIN && pin[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (mode == UNLOCK_MODE_PASSWORD && pwd[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (mode != UNLOCK_MODE_PIN && mode != UNLOCK_MODE_PASSWORD) {
        mode = UNLOCK_MODE_NONE;
        pin = "";
        pwd = "";
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 失败：%s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_PIN, pin);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_PWD, pwd);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, NVS_KEY_MODE, (uint8_t)mode);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入凭据失败：%s", esp_err_to_name(err));
        return err;
    }

    take_lock();
    strlcpy(s_pin, pin, sizeof(s_pin));
    strlcpy(s_pwd, pwd, sizeof(s_pwd));
    s_mode = mode;
    give_lock();

    ESP_LOGI(TAG, "凭据已更新：方式=%s，PIN %s，密码 %s",
             mode == UNLOCK_MODE_PIN ? "电脑 PIN" :
             mode == UNLOCK_MODE_PASSWORD ? "电脑密码" : "未设置",
             pin[0] ? "有" : "空", pwd[0] ? "有" : "空");
    return ESP_OK;
}

void pin_store_reset(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, NVS_KEY_PIN);
        nvs_erase_key(handle, NVS_KEY_PWD);
        nvs_erase_key(handle, NVS_KEY_MODE);
        nvs_commit(handle);
        nvs_close(handle);
    }

    take_lock();
    s_pin[0] = '\0';
    s_pwd[0] = '\0';
    s_mode = UNLOCK_MODE_NONE;
    give_lock();

    ESP_LOGI(TAG, "凭据已清空");
}
