/*
 * wifi_portal —— 长按 BOOT 打开的「配置热点」。
 *
 *  - 开放热点（无 WiFi 密码），SSID 为 ESP32-Unlock-xxxx
 *  - 别人一连上就由 DNS 把任意域名解析到本机，手机自动弹配置页
 *  - 网页登录密码出厂默认 0000（首次登录强制修改），登录后才能配置电脑 PIN / 密码
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t wifi_portal_start(void);
esp_err_t wifi_portal_stop(void);
bool wifi_portal_is_running(void);
