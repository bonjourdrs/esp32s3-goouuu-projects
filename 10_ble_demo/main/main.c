/*
 * ESP32-S3 BLE 双向通信最小 Demo
 *
 * 链路：Android 扫描 -> 连接 -> 写 RX -> ESP32 处理 -> Notify TX -> Android
 *
 * 只用 1 个自定义 Service + 2 个 Characteristic：
 *   RX：WRITE / WRITE_NO_RSP
 *   TX：READ  / NOTIFY（CCCD 由 NimBLE 自动生成）
 *
 * 说明：GATT 服务（0x1801）里的 Service Changed 带 INDICATE 属性，
 * 而 Android App 是"取第一个 NOTIFY/INDICATE 特征"，会把 Service Changed
 * 误当成接收通道。本 Demo 不注册 GATT 服务，只保留只读的 GAP 服务，
 * 保证 App 枚举到的第一个 WRITE / NOTIFY 就是本 Demo 的 RX / TX。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "BLE_DEMO";
static const char *DEVICE_NAME = "ESP32S3-BLE-DEMO";

#define TX_BUF_SIZE       512
#define TX_QUEUE_DEPTH    4
#define TICK_INTERVAL_US  (5 * 1000 * 1000)
#define HEX_LOG_MAX       64

/* -------------------------------------------------------------------------
 * UUID：BLE_UUID128_INIT 按小端字节序书写（字符串的最后两字节放在最前）
 * ------------------------------------------------------------------------- */
static const ble_uuid128_t svc_uuid =
    BLE_UUID128_INIT(0x2f, 0x3e, 0x4d, 0x5c, 0x6b, 0x7a, 0x3f, 0x9d,
                     0x2e, 0x4c, 0x4a, 0x9b, 0x01, 0x00, 0x1d, 0x8f);

static const ble_uuid128_t rx_uuid =
    BLE_UUID128_INIT(0x2f, 0x3e, 0x4d, 0x5c, 0x6b, 0x7a, 0x3f, 0x9d,
                     0x2e, 0x4c, 0x4a, 0x9b, 0x02, 0x00, 0x1d, 0x8f);

static const ble_uuid128_t tx_uuid =
    BLE_UUID128_INIT(0x2f, 0x3e, 0x4d, 0x5c, 0x6b, 0x7a, 0x3f, 0x9d,
                     0x2e, 0x4c, 0x4a, 0x9b, 0x03, 0x00, 0x1d, 0x8f);

/* -------------------------------------------------------------------------
 * 全局状态
 * ------------------------------------------------------------------------- */
static uint8_t own_addr_type;

/* NimBLE 主机任务写，发送任务读，用 volatile 足够（单写单读） */
static volatile uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool tx_notify_enabled = false;

static uint16_t rx_val_handle;
static uint16_t tx_val_handle;

/* TX 特征最近一次的值，供 READ 使用；跨任务访问加临界区 */
static portMUX_TYPE tx_value_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t tx_last_value[TX_BUF_SIZE];
static uint16_t tx_last_len;

typedef struct {
    uint16_t conn_handle;
    uint16_t len;
    uint8_t data[TX_BUF_SIZE];
} tx_msg_t;

static QueueHandle_t tx_queue;

/* -------------------------------------------------------------------------
 * 工具函数
 * ------------------------------------------------------------------------- */

/* 判断是否是可打印文本（ASCII 或合法 UTF-8，且不含控制字符） */
static bool is_printable_text(const uint8_t *data, size_t len)
{
    size_t i = 0;

    if (len == 0) {
        return false;
    }

    while (i < len) {
        uint8_t c = data[i];

        if (c >= 0x20 && c <= 0x7e) {
            i++;
            continue;
        }
        if (c < 0x80) {
            return false; /* 控制字符（含 \r\n）不算可打印文本 */
        }

        int extra;
        uint32_t cp;

        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
            if (cp < 2) {
                return false; /* 过长编码 */
            }
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }

        for (int k = 1; k <= extra; k++) {
            if (i + k >= len || (data[i + k] & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (data[i + k] & 0x3F);
        }

        if (cp < 0xA0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }

    return true;
}

/* 去掉首尾空白（命令匹配用），返回去空白后的长度 */
static size_t trim_bounds(const uint8_t *data, size_t len, size_t *start)
{
    size_t s = 0;
    size_t e = len;

    while (s < e && (data[s] == ' ' || data[s] == '\t' ||
                     data[s] == '\r' || data[s] == '\n')) {
        s++;
    }
    while (e > s && (data[e - 1] == ' ' || data[e - 1] == '\t' ||
                     data[e - 1] == '\r' || data[e - 1] == '\n')) {
        e--;
    }

    *start = s;
    return e - s;
}

static void log_hex(const char *prefix, const uint8_t *data, uint16_t len)
{
    char hex[HEX_LOG_MAX * 3 + 8];
    size_t off = 0;
    uint16_t n = (len < HEX_LOG_MAX) ? len : HEX_LOG_MAX;

    if (n == 0) {
        return;
    }

    for (uint16_t i = 0; i < n; i++) {
        off += (size_t)snprintf(hex + off, sizeof(hex) - off,
                                "%s%02X", (i == 0) ? "" : " ", data[i]);
    }
    if (len > HEX_LOG_MAX) {
        snprintf(hex + off, sizeof(hex) - off, " ...");
    }

    ESP_LOGI(TAG, "%s %s", prefix, hex);
}

/* -------------------------------------------------------------------------
 * Notify 发送（在 ble_tx_task 中执行，避免在 BLE 回调里占用主机锁）
 * ------------------------------------------------------------------------- */
static void ble_notify(uint16_t conn, const uint8_t *data, uint16_t len)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE || !tx_notify_enabled || conn != conn_handle) {
        ESP_LOGW(TAG, "[BLE TX] skipped len=%u: client not subscribed", len);
        return;
    }

    uint16_t mtu = ble_att_mtu(conn);
    uint16_t max_len = (mtu > 3) ? (uint16_t)(mtu - 3) : 20;

    if (len > max_len) {
        ESP_LOGW(TAG, "[BLE TX] payload %u exceeds MTU limit %u, truncated",
                 len, max_len);
        len = max_len;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        ESP_LOGE(TAG, "[BLE TX] no mbuf available");
        return;
    }

    /* om 无论成功失败都由 NimBLE 释放 */
    int rc = ble_gatts_notify_custom(conn, tx_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "[BLE TX] notify failed rc=%d", rc);
        return;
    }

    portENTER_CRITICAL(&tx_value_mux);
    memcpy(tx_last_value, data, len);
    tx_last_len = len;
    portEXIT_CRITICAL(&tx_value_mux);

    if (is_printable_text(data, len)) {
        ESP_LOGI(TAG, "[BLE TX] len=%u text=\"%.*s\"", len, (int)len,
                 (const char *)data);
    } else {
        ESP_LOGI(TAG, "[BLE TX] len=%u (binary)", len);
        log_hex("[BLE TX HEX]", data, len);
    }
}

/* -------------------------------------------------------------------------
 * 收到 App 数据后的处理：PING / STATUS / 文本 Echo / 二进制原样回显
 * ------------------------------------------------------------------------- */
static void handle_rx(uint16_t conn, const uint8_t *data, uint16_t len)
{
    if (len == 0) {
        ESP_LOGW(TAG, "[BLE RX] len=0 empty payload, ignored");
        return;
    }

    size_t start = 0;
    size_t cmd_len = trim_bounds(data, len, &start);

    /* 去掉首尾空白后再判断：App 若自动补 \r\n，仍按文本处理 */
    bool text = is_printable_text(data + start, cmd_len);

    if (text) {
        ESP_LOGI(TAG, "[BLE RX] len=%u text=\"%.*s\"", len, (int)cmd_len,
                 (const char *)(data + start));
    } else {
        ESP_LOGI(TAG, "[BLE RX] len=%u (binary)", len);
    }
    log_hex("[BLE RX HEX]", data, len);

    uint8_t resp[TX_BUF_SIZE];
    uint16_t resp_len = 0;

    if (cmd_len == 4 && memcmp(data + start, "PING", 4) == 0) {
        memcpy(resp, "PONG", 4);
        resp_len = 4;
    } else if (cmd_len == 6 && memcmp(data + start, "STATUS", 6) == 0) {
        int64_t uptime_ms = esp_timer_get_time() / 1000;
        int n = snprintf((char *)resp, sizeof(resp),
                         "{\"device\":\"ESP32-S3\",\"ble\":true,\"uptime\":%lld}",
                         (long long)uptime_ms);
        if (n <= 0) {
            return;
        }
        resp_len = (uint16_t)((n < (int)sizeof(resp)) ? n : (int)sizeof(resp) - 1);
    } else if (text) {
        const uint16_t prefix_len = 5; /* "ECHO:" */
        uint16_t copy = (cmd_len > TX_BUF_SIZE - prefix_len)
                            ? (uint16_t)(TX_BUF_SIZE - prefix_len)
                            : (uint16_t)cmd_len;

        if (copy < cmd_len) {
            ESP_LOGW(TAG, "[BLE RX] echo truncated %u -> %u",
                     (unsigned)cmd_len, copy);
        }
        memcpy(resp, "ECHO:", prefix_len);
        memcpy(resp + prefix_len, data + start, copy);
        resp_len = prefix_len + copy;
    } else {
        /* 二进制：原样回显 */
        resp_len = len;
        memcpy(resp, data, resp_len);
    }

    if (resp_len == 0) {
        return;
    }

    tx_msg_t msg = {0};
    msg.conn_handle = conn;
    msg.len = resp_len;
    memcpy(msg.data, resp, resp_len);

    /* 不在 BLE 回调里发 Notify，也不等待队列 */
    if (xQueueSend(tx_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "[BLE TX] queue full, response dropped");
    }
}

/* -------------------------------------------------------------------------
 * GATT 访问回调
 * ------------------------------------------------------------------------- */
static int gatt_access_cb(uint16_t conn, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        if (attr_handle != rx_val_handle) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[TX_BUF_SIZE];

        if (len > TX_BUF_SIZE) {
            ESP_LOGW(TAG, "[BLE RX] payload %u exceeds %d, rejected",
                     len, TX_BUF_SIZE);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (len > 0 && os_mbuf_copydata(ctxt->om, 0, len, buf) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        handle_rx(conn, buf, len);
        return 0;
    }

    case BLE_GATT_ACCESS_OP_READ_CHR: {
        if (attr_handle != tx_val_handle) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        uint8_t buf[TX_BUF_SIZE];
        uint16_t len;

        portENTER_CRITICAL(&tx_value_mux);
        len = tx_last_len;
        if (len > 0) {
            memcpy(buf, tx_last_value, len);
        }
        portEXIT_CRITICAL(&tx_value_mux);

        int rc = os_mbuf_append(ctxt->om, buf, len);
        return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                /* RX：App -> ESP32 */
                .uuid = &rx_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &rx_val_handle,
            },
            {
                /* TX：ESP32 -> App，CCCD 由 NimBLE 自动添加 */
                .uuid = &tx_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &tx_val_handle,
            },
            {0},
        },
    },
    {0},
};

/* -------------------------------------------------------------------------
 * 广播
 * ------------------------------------------------------------------------- */
static void ble_advertise(void);

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
            tx_notify_enabled = false;
            ESP_LOGI(TAG, "[BLE] client connected (conn_handle=%u)", conn_handle);
        } else {
            ESP_LOGW(TAG, "[BLE] connection failed, status=%d",
                     event->connect.status);
            ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "[BLE] client disconnected (reason=%d)",
                 event->disconnect.reason);
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        tx_notify_enabled = false;
        ESP_LOGI(TAG, "[BLE] restart advertising");
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "[BLE] advertising complete (reason=%d)",
                 event->adv_complete.reason);
        if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "[BLE] mtu updated to %d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == tx_val_handle) {
            tx_notify_enabled = (event->subscribe.cur_notify != 0);
            if (tx_notify_enabled) {
                ESP_LOGI(TAG, "[BLE] TX notify enabled");
            } else {
                ESP_LOGI(TAG, "[BLE] TX notify disabled");
            }
        } else {
            ESP_LOGI(TAG, "[BLE] subscribe attr_handle=%u conn=%u notify=%d",
                     event->subscribe.attr_handle, event->subscribe.conn_handle,
                     event->subscribe.cur_notify);
        }
        return 0;

    default:
        return 0;
    }
}

static void ble_advertise(void)
{
    struct ble_gap_adv_params params;

    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                               &params, gap_event_cb, NULL);
    if (rc == BLE_HS_EALREADY) {
        ESP_LOGI(TAG, "BLE advertising already active");
        return;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE advertising start failed rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE advertising started");
}

/* 广播包放设备名（Android 扫描列表靠它显示），扫描响应里放 Service UUID */
static void ble_set_adv_data(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = (uint8_t)strlen(DEVICE_NAME);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "set adv fields failed rc=%d", rc);
        return;
    }

    memset(&rsp, 0, sizeof(rsp));
    rsp.uuids128 = &svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "set scan response fields failed rc=%d", rc);
    }
}

/* -------------------------------------------------------------------------
 * NimBLE 主机
 * ------------------------------------------------------------------------- */
static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure BLE address failed rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer BLE address type failed rc=%d", rc);
        return;
    }

    uint8_t addr[6] = {0};
    if (ble_hs_id_copy_addr(own_addr_type, addr, NULL) == 0) {
        ESP_LOGI(TAG, "BLE address: %02X:%02X:%02X:%02X:%02X:%02X",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    }

    ESP_LOGI(TAG, "RX value handle=%u, TX value handle=%u",
             rx_val_handle, tx_val_handle);

    ble_set_adv_data();
    ble_advertise();
}

static void ble_host_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* -------------------------------------------------------------------------
 * 发送任务：处理 Echo 响应 + 每 5 秒主动推送 UPTIME
 * ------------------------------------------------------------------------- */
static void ble_tx_task(void *param)
{
    (void)param;

    int64_t last_tick = esp_timer_get_time();

    while (1) {
        tx_msg_t msg;

        if (xQueueReceive(tx_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            ble_notify(msg.conn_handle, msg.data, msg.len);
        }

        int64_t now = esp_timer_get_time();
        if (now - last_tick >= TICK_INTERVAL_US) {
            last_tick = now;

            uint16_t conn = conn_handle;
            if (conn != BLE_HS_CONN_HANDLE_NONE && tx_notify_enabled) {
                char buf[32];
                int n = snprintf(buf, sizeof(buf), "UPTIME:%lld",
                                 (long long)(now / 1000));
                if (n > 0 && n < (int)sizeof(buf)) {
                    ble_notify(conn, (const uint8_t *)buf, (uint16_t)n);
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------------------- */
static void print_banner(void)
{
    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "ESP32-S3 BLE Demo");
    ESP_LOGI(TAG, "Device: %s", DEVICE_NAME);
    ESP_LOGI(TAG, "Service: 8f1d0001-9b4a-4c2e-9d3f-7a6b5c4d3e2f");
    ESP_LOGI(TAG, "RX:      8f1d0002-9b4a-4c2e-9d3f-7a6b5c4d3e2f");
    ESP_LOGI(TAG, "TX:      8f1d0003-9b4a-4c2e-9d3f-7a6b5c4d3e2f");
    ESP_LOGI(TAG, "================================");
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    print_banner();

    tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(tx_msg_t));
    if (tx_queue == NULL) {
        ESP_LOGE(TAG, "create tx queue failed");
        return;
    }
    if (xTaskCreate(ble_tx_task, "ble_tx", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create ble_tx task failed");
        return;
    }

    memcpy(tx_last_value, "READY", 5);
    tx_last_len = 5;

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    /* GAP 服务只提供只读的 Device Name / Appearance，不含 WRITE / NOTIFY，
     * 不会干扰 App 的"第一个 WRITE / NOTIFY"选择逻辑 */
    ble_svc_gap_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed rc=%d", rc);
        abort();
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed rc=%d", rc);
        abort();
    }

    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "set device name failed rc=%d", rc);
    }

    nimble_port_freertos_init(ble_host_task);
}
