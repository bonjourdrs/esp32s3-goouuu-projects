/*
 * dev_cfg —— 两条键盘通路（USB / 蓝牙）的开关，存在 NVS 里。
 * 默认都开启，和加开关之前的行为一致。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* 需在 pin_store_init() 之后调用（NVS 已初始化） */
esp_err_t dev_cfg_init(void);

bool dev_cfg_usb_enabled(void);
bool dev_cfg_ble_enabled(void);

esp_err_t dev_cfg_set_usb(bool enabled);
esp_err_t dev_cfg_set_ble(bool enabled);

/* 恢复出厂：两条通路都回到开启 */
void dev_cfg_reset(void);
