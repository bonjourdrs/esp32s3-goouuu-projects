/*
 * bt_hid_kbd —— 把 ESP32-S3 变成一只 BLE（HID over GATT）键盘。
 *
 * 只做最少的两个动作：按下某个键（可带修饰键）、全部松开。
 * 按下 / 松开的间隔由调用方控制，方便和 USB HID 通路共用同一套时序。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 初始化 BLE 控制器 + NimBLE + HID 服务，并开始广播。
 * 失败不影响 USB 通路。 */
esp_err_t bt_kbd_init(void);

/* 电脑（主机）是否已经通过蓝牙连上本设备 */
bool bt_kbd_connected(void);

/* 按下：modifier 用 HID_KEYBOARD_MODIFIER_* 或 0，keycode 为 HID_KEY_* */
esp_err_t bt_kbd_press(uint8_t modifier, uint8_t keycode);

/* 松开所有按键 */
esp_err_t bt_kbd_release(void);

typedef struct {
    bool enabled;       /* 软件开关是否打开 */
    bool connected;     /* 电脑是否已连接 */
    bool advertising;   /* 是否正在广播（可被发现 / 可回连） */
    char address[18];   /* 本机蓝牙地址，形如 AA:BB:CC:DD:EE:FF */
} bt_kbd_status_t;

/* 开 / 关蓝牙通路：关 = 停止广播 + 断开当前连接；开 = 重新广播。
 * 可在任意任务调用（比如网页接口），内部串行化。 */
void bt_kbd_set_enabled(bool enabled);

bool bt_kbd_is_enabled(void);

/* 清除所有配对记录（bond），并重新开始广播。
 * 换电脑前用：Windows 端也要「删除设备」，两边都清才干净。 */
void bt_kbd_forget_all(void);

void bt_kbd_get_status(bt_kbd_status_t *out);
