/*
 * 15_usb_hid_unlock
 *
 * 短按板载 BOOT 键（GPIO0）：ESP32-S3 以键盘身份向 Windows 锁屏界面
 * 输入电脑 PIN 或电脑密码并回车，完成解锁。
 * 两条通路：USB 已挂载时走 USB HID（最稳）；拔了 USB 且蓝牙已配对连接时
 * 走 BLE HID（Windows 登录后有效，BIOS 不支持）。
 * 长按 BOOT 键 3 秒：开 / 关配置热点，手机连上后自动弹出配置网页；
 *   网页登录密码出厂为 0000，首次登录必须修改；
 *   登录后可设置电脑 PIN / 电脑密码，并选定其中一种作为解锁方式。
 * 长按 BOOT 键 10 秒：恢复出厂（网页密码回 0000、凭据清空、通路开启、
 *   清除蓝牙配对），随后自动重启。
 *
 * USB 部分基于 ESP-IDF v5.5.6 官方 examples/peripherals/usb/device/tusb_hid
 * （Unlicense OR CC0-1.0），去掉了鼠标部分，只保留键盘；
 * 蓝牙部分基于 examples/bluetooth/esp_hid_device 的 NimBLE 键盘分支。
 */

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "driver/gpio.h"

#include "pin_store.h"
#include "web_auth.h"
#include "wifi_portal.h"
#include "bt_hid_kbd.h"
#include "dev_cfg.h"

/* ---------------- 可调参数 ---------------- */

#define APP_BUTTON          GPIO_NUM_0     /* 板载 BOOT 键，按下拉低 */
#define LONG_PRESS_MS       3000           /* 按住超过这个时长算长按：切换配置热点 */
#define FACTORY_RESET_MS    10000          /* 按住 10 秒：恢复出厂并重启 */
#define WAKE_DELAY_MS       1200           /* 唤醒键之后等锁屏界面就绪，太快会吞按键 */
#define LOCK_WAIT_MS        900            /* Win+L 之后等锁屏动画结束，锁屏不生效就加到 1200 */
#define KEY_PRESS_MS        15             /* 单键按下保持时间 */
#define KEY_GAP_MS          40             /* 相邻按键间隔 */

static const char *TAG = "UNLOCK";

/* 本次解锁走哪条通路：false = USB，true = 蓝牙。
 * 每次发送序列开始时决定，序列中途不变。 */
static bool s_use_ble;

/* ---------------- TinyUSB 描述符 ---------------- */

#define TUSB_DESC_TOTAL_LEN     (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

/* 只声明键盘，不再声明鼠标 */
const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD)),
};

const char *hid_string_descriptor[5] = {
    (char[]){0x09, 0x04},          /* 0: 语言 ID（英文 0x0409） */
    "Espressif",                   /* 1: 厂商 */
    "ESP32-S3 Unlock Key",         /* 2: 产品名 */
    "123456",                      /* 3: 序列号，量产应改用芯片 ID */
    "Unlock HID interface",        /* 4: HID 接口名 */
};

static const uint8_t hid_configuration_descriptor[] = {
    /* 配置号, 接口数, 字符串索引, 总长度, 属性(远程唤醒), 供电电流 mA */
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    /* 接口号, 字符串索引, boot 协议, 报告描述符长度, IN 端点, 包长, 轮询间隔 ms */
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
};

/* ---------------- TinyUSB 回调 ---------------- */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

/* ---------------- 按键与 PIN 发送 ---------------- */

/* 把 ASCII 字符映射成 HID 键码（美式键盘布局）。
 * 只支持 1–32 位的英文、数字和符号；中文等多字节字符无法用键盘直接输入。 */
static bool char_to_key(char c, uint8_t *mod, uint8_t *key)
{
    const uint8_t shift = KEYBOARD_MODIFIER_LEFTSHIFT;
    *mod = 0;

    if (c >= 'a' && c <= 'z') {
        *key = (uint8_t)(HID_KEY_A + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        *key = (uint8_t)(HID_KEY_A + (c - 'A'));
        *mod = shift;
        return true;
    }
    if (c >= '1' && c <= '9') {
        *key = (uint8_t)(HID_KEY_1 + (c - '1'));
        return true;
    }
    if (c == '0') {
        *key = HID_KEY_0;
        return true;
    }

    switch (c) {
    case ' ':  *key = HID_KEY_SPACE; return true;
    case '!':  *mod = shift; *key = HID_KEY_1; return true;
    case '"':  *mod = shift; *key = HID_KEY_APOSTROPHE; return true;
    case '#':  *mod = shift; *key = HID_KEY_3; return true;
    case '$':  *mod = shift; *key = HID_KEY_4; return true;
    case '%':  *mod = shift; *key = HID_KEY_5; return true;
    case '&':  *mod = shift; *key = HID_KEY_7; return true;
    case '\'': *key = HID_KEY_APOSTROPHE; return true;
    case '(':  *mod = shift; *key = HID_KEY_9; return true;
    case ')':  *mod = shift; *key = HID_KEY_0; return true;
    case '*':  *mod = shift; *key = HID_KEY_8; return true;
    case '+':  *mod = shift; *key = HID_KEY_EQUAL; return true;
    case ',':  *key = HID_KEY_COMMA; return true;
    case '-':  *key = HID_KEY_MINUS; return true;
    case '.':  *key = HID_KEY_PERIOD; return true;
    case '/':  *key = HID_KEY_SLASH; return true;
    case ':':  *mod = shift; *key = HID_KEY_SEMICOLON; return true;
    case ';':  *key = HID_KEY_SEMICOLON; return true;
    case '<':  *mod = shift; *key = HID_KEY_COMMA; return true;
    case '=':  *key = HID_KEY_EQUAL; return true;
    case '>':  *mod = shift; *key = HID_KEY_PERIOD; return true;
    case '?':  *mod = shift; *key = HID_KEY_SLASH; return true;
    case '@':  *mod = shift; *key = HID_KEY_2; return true;
    case '[':  *key = HID_KEY_BRACKET_LEFT; return true;
    case '\\': *key = HID_KEY_BACKSLASH; return true;
    case ']':  *key = HID_KEY_BRACKET_RIGHT; return true;
    case '^':  *mod = shift; *key = HID_KEY_6; return true;
    case '_':  *mod = shift; *key = HID_KEY_MINUS; return true;
    case '`':  *key = HID_KEY_GRAVE; return true;
    case '{':  *mod = shift; *key = HID_KEY_BRACKET_LEFT; return true;
    case '|':  *mod = shift; *key = HID_KEY_BACKSLASH; return true;
    case '}':  *mod = shift; *key = HID_KEY_BRACKET_RIGHT; return true;
    case '~':  *mod = shift; *key = HID_KEY_GRAVE; return true;
    default:   return false;
    }
}

/* 发一次「按下 + 松开」。USB 已挂载就走 USB，否则走蓝牙，时序完全一致。 */
static void send_key(uint8_t modifier, uint8_t keycode)
{
    if (s_use_ble) {
        if (bt_kbd_press(modifier, keycode) != ESP_OK) {
            ESP_LOGW(TAG, "蓝牙发键失败，连接可能已断开");
        }
        vTaskDelay(pdMS_TO_TICKS(KEY_PRESS_MS));
        bt_kbd_release();
        vTaskDelay(pdMS_TO_TICKS(KEY_GAP_MS));
        return;
    }

    uint8_t key[6] = {keycode, 0, 0, 0, 0, 0};

    tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, modifier, key);
    vTaskDelay(pdMS_TO_TICKS(KEY_PRESS_MS));
    tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, NULL);
    vTaskDelay(pdMS_TO_TICKS(KEY_GAP_MS));
}

static void tap_key(uint8_t keycode)
{
    send_key(0, keycode);
}

/* 带修饰键的组合键（如 Win + L） */
static void tap_combo(uint8_t modifier, uint8_t keycode)
{
    send_key(modifier, keycode);
}

static void send_unlock_sequence(void)
{
    unlock_mode_t mode = pin_store_get_mode();
    char secret[PWD_MAX_LEN + 1];

    if (mode == UNLOCK_MODE_NONE) {
        ESP_LOGW(TAG, "尚未设置解锁凭据：长按 BOOT 3 秒进网页，设置电脑 PIN 或电脑密码");
        return;
    }

    secret[0] = '\0';
    if (mode == UNLOCK_MODE_PIN) {
        pin_store_get_pin(secret, sizeof(secret));
    } else {
        pin_store_get_password(secret, sizeof(secret));
    }
    if (secret[0] == '\0') {
        ESP_LOGW(TAG, "当前解锁方式没有对应的凭据，请进网页重新设置");
        return;
    }

    bool usb_on = dev_cfg_usb_enabled();
    bool ble_on = dev_cfg_ble_enabled();

    /* USB 优先：插着线就用 USB，最稳，也不受蓝牙配对状态影响 */
    if (usb_on && tud_mounted()) {
        s_use_ble = false;
        ESP_LOGI(TAG, "使用 USB HID 通路");
    } else if (ble_on && bt_kbd_connected()) {
        s_use_ble = true;
        ESP_LOGI(TAG, "使用蓝牙 HID 通路");
    } else if (!usb_on && !ble_on) {
        ESP_LOGW(TAG, "USB 和蓝牙通路都被关闭，忽略本次触发（长按 BOOT 进网页开启）");
        return;
    } else if (!usb_on) {
        ESP_LOGW(TAG, "USB 通路已关闭，且蓝牙未连接，忽略本次触发");
        return;
    } else if (!ble_on) {
        ESP_LOGW(TAG, "蓝牙通路已关闭，且 USB 未挂载，忽略本次触发");
        return;
    } else {
        ESP_LOGW(TAG, "USB 未挂载且蓝牙未连接，忽略本次触发（插 USB 口，或先在系统里配对本设备）");
        return;
    }

    /* 先发 Win+L 强制锁屏：
     * 桌面、聊天窗口、输入框里误触时，密码也只会输进锁屏的安全桌面，
     * 不会出现在任何前台窗口里；本来就锁屏时这一下没有副作用。 */
    ESP_LOGI(TAG, "发送 Win+L 锁屏键");
    tap_combo(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_L);
    vTaskDelay(pdMS_TO_TICKS(LOCK_WAIT_MS));

    /* 锁屏默认只显示壁纸，按一个空格把输入框唤出来 */
    ESP_LOGI(TAG, "发送唤醒键");
    tap_key(HID_KEY_SPACE);
    vTaskDelay(pdMS_TO_TICKS(WAKE_DELAY_MS));

    if (mode == UNLOCK_MODE_PASSWORD) {
        ESP_LOGI(TAG, "开始输入电脑密码（%u 位）", (unsigned)strlen(secret));
        ESP_LOGW(TAG, "密码模式：请确保锁屏界面已切到「登录选项 → 密码」，PIN 框不接受字母");
    } else {
        ESP_LOGI(TAG, "开始输入电脑 PIN（%u 位）", (unsigned)strlen(secret));
    }

    for (const char *p = secret; *p != '\0'; p++) {
        uint8_t mod = 0;
        uint8_t keycode = 0;
        if (!char_to_key(*p, &mod, &keycode)) {
            ESP_LOGW(TAG, "跳过无法识别的字符（0x%02X）", (unsigned char)*p);
            continue;
        }
        if (mod == 0) {
            tap_key(keycode);
        } else {
            tap_combo(mod, keycode);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    tap_key(HID_KEY_ENTER);
    memset(secret, 0, sizeof(secret));
    ESP_LOGI(TAG, "解锁序列发送完成");
}

static void toggle_portal(void)
{
    if (wifi_portal_is_running()) {
        wifi_portal_stop();
    } else if (wifi_portal_start() != ESP_OK) {
        ESP_LOGW(TAG, "配置热点开启失败");
    }
}

/* 恢复出厂：网页密码回到 0000、凭据清空、通路开启、蓝牙配对清除 */
static void factory_reset(void)
{
    if (wifi_portal_is_running()) {
        wifi_portal_stop();
    }
    web_auth_reset();
    pin_store_reset();
    dev_cfg_reset();
    bt_kbd_forget_all();

    ESP_LOGW(TAG, "已恢复出厂设置：网页密码 0000（下次登录需重设）、解锁凭据清空、"
                  "USB/蓝牙通路开启、蓝牙配对已清除，3 秒后重启");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

static void button_task(void *arg)
{
    (void)arg;

    bool confirmed = false;      /* 消抖通过、确认按住了 */
    bool long_fired = false;     /* 本次按下已经触发过 3 秒长按 */
    bool reset_fired = false;    /* 本次按下已经触发过 10 秒恢复出厂 */
    TickType_t press_tick = 0;

    while (true) {
        bool pressed = (gpio_get_level(APP_BUTTON) == 0);

        if (!confirmed && pressed) {
            vTaskDelay(pdMS_TO_TICKS(20));                  /* 消抖 */
            if (gpio_get_level(APP_BUTTON) == 0) {
                confirmed = true;
                long_fired = false;
                reset_fired = false;
                press_tick = xTaskGetTickCount();
                ESP_LOGI(TAG, "BOOT 按下");
            }
        } else if (confirmed && pressed) {
            uint32_t held = (uint32_t)((xTaskGetTickCount() - press_tick) * portTICK_PERIOD_MS);

            if (!long_fired && held >= LONG_PRESS_MS) {
                long_fired = true;                          /* 长按只触发一次 */
                ESP_LOGI(TAG, "BOOT 长按 3 秒，切换配置热点");
                toggle_portal();
            }
            if (!reset_fired && held >= FACTORY_RESET_MS) {
                reset_fired = true;
                ESP_LOGW(TAG, "BOOT 长按 10 秒，恢复出厂设置");
                factory_reset();
            }
        } else if (confirmed && !pressed) {
            confirmed = false;
            if (!long_fired) {
                send_unlock_sequence();                     /* 短按才输密码 */
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ---------------- 入口 ---------------- */

void app_main(void)
{
    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << APP_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));

    ESP_ERROR_CHECK(pin_store_init());
    ESP_ERROR_CHECK(web_auth_init());
    dev_cfg_init();

    ESP_LOGI(TAG, "USB HID 初始化");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();

    tusb_cfg.descriptor.device = NULL;
    tusb_cfg.descriptor.full_speed_config = hid_configuration_descriptor;
    tusb_cfg.descriptor.string = hid_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = hid_configuration_descriptor;
#endif

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB HID 就绪");

    ESP_LOGI(TAG, "蓝牙 HID 初始化");
    esp_err_t bt_err = bt_kbd_init();
    if (bt_err != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙 HID 初始化失败（%s），本次只保留 USB 通路", esp_err_to_name(bt_err));
    } else if (!dev_cfg_ble_enabled()) {
        /* 网页里关过蓝牙：协议栈照常初始化，但不广播、不连接 */
        bt_kbd_set_enabled(false);
    }

    ESP_LOGI(TAG, "等待 BOOT 按键");

    xTaskCreate(button_task, "unlock_button", 4096, NULL, 5, NULL);
}