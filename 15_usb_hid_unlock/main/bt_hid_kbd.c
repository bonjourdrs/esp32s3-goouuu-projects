/*
 * bt_hid_kbd —— BLE 蓝牙键盘（HID over GATT）通路。
 *
 * 基于 ESP-IDF 官方 examples/bluetooth/esp_hid_device（Unlicense OR CC0-1.0）
 * 里 NimBLE 键盘的那一支，裁掉鼠标 / 经典蓝牙 / 控制台交互，只保留：
 *   控制器初始化 → NimBLE 启动 → 注册 HID 服务 → 广播 → 连接后发键。
 *
 * 配对方式是 Just Works（无显示无输入设备的标准做法）：
 * Windows 里点一次「添加设备」即可，不需要在电脑上输配对码；
 * bond 信息存在 NVS 里，下次自动重连。
 *
 * 已知限制：BLE 键盘只在 Windows 登录后的系统里有效，
 * BIOS / 开机密码界面没有蓝牙协议栈，只能走 USB。
 */

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_hidd.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "bt_hid_kbd.h"

static const char *TAG = "BTKBD";

#define KBD_REPORT_ID    1
#define KBD_REPORT_LEN   8      /* 1 修饰键 + 1 保留 + 6 键码 */

/* 键盘报告描述符：
 * 输入报告 8 字节（修饰键 1 + 保留 1 + 键码 6），另带 1 字节 LED 输出报告。 */
static const uint8_t s_report_map[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /* Usage (Keyboard) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, KBD_REPORT_ID, /*   Report ID (1) */
    0x05, 0x07,        /*   Usage Page (Keyboard/Keypad) */
    0x19, 0xE0,        /*   Usage Minimum (Left Control) */
    0x29, 0xE7,        /*   Usage Maximum (Right GUI) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x08,        /*   Report Count (8) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) —— 修饰键 */
    0x95, 0x01,        /*   Report Count (1) */
    0x75, 0x08,        /*   Report Size (8) */
    0x81, 0x03,        /*   Input (Const) —— 保留字节 */
    0x95, 0x05,        /*   Report Count (5) */
    0x75, 0x01,        /*   Report Size (1) */
    0x05, 0x08,        /*   Usage Page (LEDs) */
    0x19, 0x01,        /*   Usage Minimum (Num Lock) */
    0x29, 0x05,        /*   Usage Maximum (Kana) */
    0x91, 0x02,        /*   Output (Data,Var,Abs) —— LED 状态 */
    0x95, 0x01,        /*   Report Count (1) */
    0x75, 0x03,        /*   Report Size (3) */
    0x91, 0x03,        /*   Output (Const) */
    0x95, 0x06,        /*   Report Count (6) */
    0x75, 0x08,        /*   Report Size (8) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x65,        /*   Logical Maximum (101) */
    0x05, 0x07,        /*   Usage Page (Keyboard/Keypad) */
    0x19, 0x00,        /*   Usage Minimum (0) */
    0x29, 0x65,        /*   Usage Maximum (101) */
    0x81, 0x00,        /*   Input (Data,Array) —— 6 个键码 */
    0xC0,              /* End Collection */
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {
        .data = s_report_map,
        .len  = sizeof(s_report_map),
    },
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id         = 0x303A,
    .product_id        = 0x0001,
    .version           = 0x0100,
    .device_name       = "ESP32-S3 Unlock Key",
    .manufacturer_name = "Espressif",
    .serial_number     = "123456",
    .report_maps       = s_report_maps,
    .report_maps_len   = 1,
};

static esp_hidd_dev_t *s_hid_dev;
static struct ble_hs_adv_fields s_adv_fields;
static struct ble_hs_adv_fields s_rsp_fields;
static ble_uuid16_t s_hid_uuid = BLE_UUID16_INIT(0x1812);   /* HID over GATT */
static bool s_connected;
static bool s_enabled = true;                       /* 软件开关，可在网页里改 */
static uint8_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static SemaphoreHandle_t s_lock;                    /* 串行化网页任务与协议栈操作 */
static uint8_t s_own_addr_type;

void ble_store_config_init(void);

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);

static void start_advertising(void)
{
    if (!s_enabled) {
        return;
    }
    if (!ble_hs_synced()) {
        return;
    }
    if (s_connected) {
        return;
    }
    if (ble_gap_adv_active()) {
        return;
    }

    int rc = ble_gap_adv_set_fields(&s_adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播数据失败 rc=%d", rc);
        return;
    }

    /* 设备名放扫描响应包里：广播包只有 31 字节，名字放主包会超长 */
    rc = ble_gap_adv_rsp_set_fields(&s_rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置扫描响应失败 rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min  = BLE_GAP_ADV_ITVL_MS(40);
    params.itvl_max  = BLE_GAP_ADV_ITVL_MS(60);

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params,
                           ble_gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "启动广播失败 rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "蓝牙已开始广播：%s", s_hid_config.device_name);
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            ESP_LOGI(TAG, "电脑已连接，蓝牙键盘可用");
        } else {
            ESP_LOGW(TAG, "连接失败 status=%d，继续广播", event->connect.status);
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGW(TAG, "电脑断开连接 reason=%d，重新广播", event->disconnect.reason);
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "链路加密状态变化 status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* 电脑忘记了旧 bond 又重新配对：删掉旧记录，接受新配对 */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}

static void hid_event_cb(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;

    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID 服务已启动");
        start_advertising();          /* 主机同步后 on_sync 也会再试一次 */
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "HID 连接事件");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGW(TAG, "HID 断开事件");
        s_connected = false;
        start_advertising();
        break;
    default:
        break;
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "获取蓝牙地址失败 rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "推断地址类型失败 rc=%d", rc);
        return;
    }

    memset(&s_adv_fields, 0, sizeof(s_adv_fields));
    s_adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    s_adv_fields.appearance = ESP_HID_APPEARANCE_KEYBOARD;
    s_adv_fields.appearance_is_present = 1;
    s_adv_fields.tx_pwr_lvl_is_present = 1;
    s_adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    s_adv_fields.uuids16 = &s_hid_uuid;
    s_adv_fields.num_uuids16 = 1;
    s_adv_fields.uuids16_is_complete = 1;

    memset(&s_rsp_fields, 0, sizeof(s_rsp_fields));
    s_rsp_fields.name = (uint8_t *)s_hid_config.device_name;
    s_rsp_fields.name_len = strlen(s_hid_config.device_name);
    s_rsp_fields.name_is_complete = 1;

    start_advertising();
}

static void host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE 主机任务启动");
    nimble_port_run();               /* 只有 nimble_port_stop() 才会返回 */
    nimble_port_freertos_deinit();
}

esp_err_t bt_kbd_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }

    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        /* ESP32-S3 本来就没有经典蓝牙，这里失败可以忽略 */
        ESP_LOGD(TAG, "释放经典蓝牙内存返回 %s", esp_err_to_name(err));
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙控制器初始化失败 %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙控制器使能失败 %s", esp_err_to_name(err));
        return err;
    }

    err = esp_nimble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE 初始化失败 %s", esp_err_to_name(err));
        return err;
    }

    /* Just Works 配对 + bond 持久化：Windows 点一下就能配对，不用输码 */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sync_cb = on_sync;

    err = esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, hid_event_cb, &s_hid_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HID 设备注册失败 %s", esp_err_to_name(err));
        return err;
    }

    /* esp_hid 的 BLE 实现默认会建电池服务（BAS），初值 0%，Windows 会一直提示
     * 「电量不足」；本设备是 USB 供电，直接报 100%。 */
    esp_hidd_dev_battery_set(s_hid_dev, 100);
    /* 不设的话 GAP 设备名是 NimBLE 默认的 "nimble"，Windows 通知里会显示这个名字 */
    ble_svc_gap_device_name_set(s_hid_config.device_name);

    ble_store_config_init();
    nimble_port_freertos_init(host_task);

    return ESP_OK;
}

bool bt_kbd_connected(void)
{
    return s_connected;
}

static esp_err_t send_report(uint8_t modifier, uint8_t keycode)
{
    if (s_hid_dev == NULL || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t report[KBD_REPORT_LEN] = {modifier, 0, keycode, 0, 0, 0, 0, 0};
    return esp_hidd_dev_input_set(s_hid_dev, 0, KBD_REPORT_ID, report, sizeof(report));
}

esp_err_t bt_kbd_press(uint8_t modifier, uint8_t keycode)
{
    return send_report(modifier, keycode);
}

esp_err_t bt_kbd_release(void)
{
    return send_report(0, 0);
}

void bt_kbd_set_enabled(bool enabled)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }

    if (enabled == s_enabled) {
        if (s_lock != NULL) {
            xSemaphoreGive(s_lock);
        }
        return;
    }
    s_enabled = enabled;

    if (ble_hs_synced()) {
        if (enabled) {
            start_advertising();
        } else {
            if (ble_gap_adv_active()) {
                ble_gap_adv_stop();
            }
            if (s_connected) {
                ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
        }
    }

    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "蓝牙通路已%s", enabled ? "开启" : "关闭");
}

bool bt_kbd_is_enabled(void)
{
    return s_enabled;
}

void bt_kbd_forget_all(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }

    bool ready = ble_hs_synced();
    if (ready) {
        if (s_connected) {
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        ble_store_clear();
    }

    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }

    if (ready) {
        ESP_LOGI(TAG, "已清除所有蓝牙配对记录");
        start_advertising();
    }
}

void bt_kbd_get_status(bt_kbd_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->enabled = s_enabled;
    out->connected = s_connected;
    out->advertising = ble_hs_synced() && ble_gap_adv_active();

    uint8_t addr[6] = {0};
    if (ble_hs_synced() && ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr, NULL) == 0) {
        snprintf(out->address, sizeof(out->address), "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    } else {
        strlcpy(out->address, "未知", sizeof(out->address));
    }
}
