#include "web_auth.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define NVS_NAMESPACE   "unlock_cfg"
#define NVS_KEY_WEB_PWD "web_pwd"
#define NVS_KEY_WEB_CHG "web_chg"

static const char *TAG = "WEBAUTH";

static char s_pwd[WEB_PWD_MAX_LEN + 1] = WEB_PWD_DEFAULT;
static bool s_changed;

static bool pwd_valid(const char *pwd)
{
    if (pwd == NULL) {
        return false;
    }
    size_t len = strlen(pwd);
    if (len < WEB_PWD_MIN_LEN || len > WEB_PWD_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (pwd[i] < '0' || pwd[i] > '9') {
            return false;
        }
    }
    return true;
}

esp_err_t web_auth_init(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        char buf[WEB_PWD_MAX_LEN + 1] = {0};
        size_t len = sizeof(buf);
        if (nvs_get_str(handle, NVS_KEY_WEB_PWD, buf, &len) == ESP_OK && pwd_valid(buf)) {
            strlcpy(s_pwd, buf, sizeof(s_pwd));
        }

        uint8_t chg = 0;
        if (nvs_get_u8(handle, NVS_KEY_WEB_CHG, &chg) == ESP_OK) {
            s_changed = (chg != 0);
        }
        nvs_close(handle);
    }

    if (!s_changed) {
        ESP_LOGW(TAG, "网页密码仍是出厂默认 %s，首次登录后必须修改", WEB_PWD_DEFAULT);
    } else {
        ESP_LOGI(TAG, "网页密码已从 NVS 载入");
    }
    return ESP_OK;
}

bool web_auth_must_change(void)
{
    return !s_changed;
}

bool web_auth_check(const char *pwd)
{
    if (pwd == NULL) {
        return false;
    }
    return strcmp(pwd, s_pwd) == 0;
}

esp_err_t web_auth_set(const char *pwd)
{
    if (!pwd_valid(pwd)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 失败：%s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_WEB_PWD, pwd);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, NVS_KEY_WEB_CHG, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入网页密码失败：%s", esp_err_to_name(err));
        return err;
    }

    strlcpy(s_pwd, pwd, sizeof(s_pwd));
    s_changed = true;
    ESP_LOGI(TAG, "网页登录密码已更新");
    return ESP_OK;
}

void web_auth_reset(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, NVS_KEY_WEB_PWD);
        nvs_erase_key(handle, NVS_KEY_WEB_CHG);
        nvs_commit(handle);
        nvs_close(handle);
    }

    strlcpy(s_pwd, WEB_PWD_DEFAULT, sizeof(s_pwd));
    s_changed = false;
    ESP_LOGI(TAG, "网页登录密码已恢复为出厂默认");
}
