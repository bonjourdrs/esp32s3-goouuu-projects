#include "app_ble_manager.h"

#include <esp_log.h>
#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#include <cstring>

static const char* TAG = "AppBle";

namespace appdemo {

namespace {

constexpr const char* kDeviceName = "ESP32S3-AI-ASSISTANT";

// 8F1D0001-9B4A-4C2E-9D3F-7A6B5C4D3E2F
ble_uuid128_t g_service_uuid;
// 8F1D0002-9B4A-4C2E-9D3F-7A6B5C4D3E2F  (RX, App -> 设备)
ble_uuid128_t g_rx_uuid;
// 8F1D0003-9B4A-4C2E-9D3F-7A6B5C4D3E2F  (TX, 设备 -> App)
ble_uuid128_t g_tx_uuid;

// NimBLE 的 128-bit UUID 使用小端字节序（UUID 字符串按字节反转）
const uint8_t kServiceBytes[16] = {0x2f, 0x3e, 0x4d, 0x5c, 0x6b, 0x7a, 0x3f, 0x9d,
                                   0x2e, 0x4c, 0x4a, 0x9b, 0x01, 0x00, 0x1d, 0x8f};
const uint8_t kRxBytes[16] = {0x2f, 0x3e, 0x4d, 0x5c, 0x6b, 0x7a, 0x3f, 0x9d,
                              0x2e, 0x4c, 0x4a, 0x9b, 0x02, 0x00, 0x1d, 0x8f};
const uint8_t kTxBytes[16] = {0x2f, 0x3e, 0x4d, 0x5c, 0x6b, 0x7a, 0x3f, 0x9d,
                              0x2e, 0x4c, 0x4a, 0x9b, 0x03, 0x00, 0x1d, 0x8f};

void InitUuid(ble_uuid128_t* uuid, const uint8_t bytes[16]) {
    memset(uuid, 0, sizeof(*uuid));
    uuid->u.type = BLE_UUID_TYPE_128;
    memcpy(uuid->value, bytes, 16);
}

void InitUuids() {
    InitUuid(&g_service_uuid, kServiceBytes);
    InitUuid(&g_rx_uuid, kRxBytes);
    InitUuid(&g_tx_uuid, kTxBytes);
}

uint16_t g_tx_val_handle = 0;
uint8_t g_own_addr_type = BLE_OWN_ADDR_PUBLIC;
AppBleManager* g_ble = nullptr;

int GattAccess(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt,
               void* arg);
void StartAdvertising();
int GapEvent(struct ble_gap_event* event, void* arg);

const struct ble_gatt_chr_def kCharacteristics[] = {
    {
        .uuid = &g_rx_uuid.u,
        .access_cb = GattAccess,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .min_key_size = 0,
        .val_handle = nullptr,
        .cpfd = nullptr,
    },
    {
        .uuid = &g_tx_uuid.u,
        .access_cb = GattAccess,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
        .min_key_size = 0,
        .val_handle = &g_tx_val_handle,
        .cpfd = nullptr,
    },
    {0},
};

const struct ble_gatt_svc_def kGattServices[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_service_uuid.u,
        .includes = nullptr,
        .characteristics = kCharacteristics,
    },
    {0},
};

int GattAccess(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt,
               void* arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > 512) {
            ESP_LOGW(TAG, "BLE write too long: %u", len);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint8_t buffer[512];
        if (os_mbuf_copydata(ctxt->om, 0, len, buffer) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (g_ble != nullptr) {
            g_ble->OnRx(buffer, len);
        }
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

void StartAdvertising() {
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &g_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed: %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields = {};
    rsp_fields.name = reinterpret_cast<const uint8_t*>(kDeviceName);
    rsp_fields.name_len = strlen(kDeviceName);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan rsp fields failed: %d", rc);
    }

    struct ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 0x20;
    params.itvl_max = 0x40;
    rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER, &params, GapEvent, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as %s", kDeviceName);
    }
}

int GapEvent(struct ble_gap_event* event, void* arg) {
    if (g_ble == nullptr) {
        return 0;
    }
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                g_ble->OnConnected(event->connect.conn_handle);
            } else {
                ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
                StartAdvertising();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "client disconnected, reason=%d", event->disconnect.reason);
            g_ble->OnDisconnected(event->disconnect.reason);
            StartAdvertising();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            StartAdvertising();
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == g_tx_val_handle) {
                bool enabled = event->subscribe.cur_notify != 0;
                ESP_LOGI(TAG, "notify %s", enabled ? "enabled" : "disabled");
                g_ble->OnSubscribed(enabled);
            }
            break;
        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "mtu updated: %d", event->mtu.value);
            break;
        default:
            break;
    }
    return 0;
}

void HostSync() {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr failed: %d", rc);
    }
    rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed: %d", rc);
    }
    if (g_ble != nullptr) {
        g_ble->OnHostSync();
    }
    StartAdvertising();
}

void HostReset(int reason) {
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
    if (g_ble != nullptr) {
        g_ble->OnHostReset(reason);
    }
}

void HostTask(void* param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

}  // namespace

AppBleManager& AppBleManager::GetInstance() {
    static AppBleManager instance;
    return instance;
}

bool AppBleManager::Start(CommandHandler handler) {
    if (started_.exchange(true)) {
        return true;
    }
    // NimBLE 默认在 INFO 级别逐条打印 GATT notify，20Hz 姿态上报会刷爆串口
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    command_handler_ = std::move(handler);
    g_ble = this;

    tx_queue_ = xQueueCreate(24, sizeof(Packet));
    latest_queue_ = xQueueCreate(1, sizeof(Packet));
    cmd_queue_ = xQueueCreate(8, sizeof(Packet));
    if (tx_queue_ == nullptr || latest_queue_ == nullptr || cmd_queue_ == nullptr) {
        ESP_LOGE(TAG, "create queue failed");
        started_ = false;
        return false;
    }
    xTaskCreate(TxTaskEntry, "app_ble_tx", 4096, this, 5, &tx_task_);
    xTaskCreate(CommandTaskEntry, "app_ble_cmd", 6144, this, 5, &cmd_task_);

    nimble_port_init();
    ble_hs_cfg.sync_cb = HostSync;
    ble_hs_cfg.reset_cb = HostReset;
    ble_att_set_preferred_mtu(247);

    InitUuids();
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(kGattServices);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts count cfg failed: %d", rc);
        return false;
    }
    rc = ble_gatts_add_svcs(kGattServices);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts add svcs failed: %d", rc);
        return false;
    }
    ble_svc_gap_device_name_set(kDeviceName);

    nimble_port_freertos_init(HostTask);
    ESP_LOGI(TAG, "BLE stack started");
    return true;
}

bool AppBleManager::Send(const std::string& json) {
    if (!CanNotify() || tx_queue_ == nullptr) {
        return false;
    }
    if (json.size() > kMaxPacket) {
        ESP_LOGW(TAG, "json too long, drop: %u", static_cast<unsigned>(json.size()));
        return false;
    }
    Packet packet = {};
    packet.len = static_cast<uint16_t>(json.size());
    memcpy(packet.data, json.data(), json.size());
    if (xQueueSend(tx_queue_, &packet, 0) != pdTRUE) {
        ESP_LOGW(TAG, "tx queue full, drop: %s", json.c_str());
        return false;
    }
    return true;
}

bool AppBleManager::SendLatest(const std::string& json) {
    if (!CanNotify() || latest_queue_ == nullptr) {
        return false;
    }
    if (json.size() > kMaxPacket) {
        ESP_LOGW(TAG, "json too long, drop: %u", static_cast<unsigned>(json.size()));
        return false;
    }
    Packet packet = {};
    packet.len = static_cast<uint16_t>(json.size());
    memcpy(packet.data, json.data(), json.size());

    if (xQueueSend(latest_queue_, &packet, 0) != pdTRUE) {
        // 深度 1 的队列满了：丢弃旧帧，保证只发最新姿态
        Packet stale;
        xQueueReceive(latest_queue_, &stale, 0);
        if (xQueueSend(latest_queue_, &packet, 0) != pdTRUE) {
            return false;
        }
    }
    return true;
}

void AppBleManager::TxTaskEntry(void* arg) {
    static_cast<AppBleManager*>(arg)->TxLoop();
    vTaskDelete(nullptr);
}

void AppBleManager::CommandTaskEntry(void* arg) {
    static_cast<AppBleManager*>(arg)->CommandLoop();
    vTaskDelete(nullptr);
}

void AppBleManager::TxLoop() {
    Packet packet;
    for (;;) {
        // 优先发送普通消息（ACK/状态）；空闲时再发最新一帧高频数据。
        // 注意：FreeRTOS tick 默认 100Hz，超时必须 >= 1 tick，否则退化成忙等并饿死 IDLE。
        bool has_packet =
            xQueueReceive(tx_queue_, &packet, pdMS_TO_TICKS(10)) == pdTRUE;
        if (!has_packet) {
            has_packet = xQueueReceive(latest_queue_, &packet, 0) == pdTRUE;
        }
        if (!has_packet) {
            continue;
        }
        if (!CanNotify()) {
            continue;
        }
        uint16_t mtu = ble_att_mtu(conn_handle_.load());
        if (mtu == 0) {
            mtu = 23;
        }
        if (packet.len + 3 > mtu) {
            ESP_LOGW(TAG, "notify exceeds mtu(%u): %u", mtu, packet.len);
            continue;
        }
        struct os_mbuf* om = ble_hs_mbuf_from_flat(packet.data, packet.len);
        if (om == nullptr) {
            ESP_LOGW(TAG, "alloc mbuf failed");
            continue;
        }
        int rc = ble_gatts_notify_custom(conn_handle_.load(), g_tx_val_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify failed: %d", rc);
        }
    }
}

void AppBleManager::CommandLoop() {
    Packet packet;
    for (;;) {
        if (xQueueReceive(cmd_queue_, &packet, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (command_handler_) {
            command_handler_(std::string(packet.data, packet.len));
        }
    }
}

void AppBleManager::OnHostSync() {
    ESP_LOGI(TAG, "host synced");
}

void AppBleManager::OnHostReset(int reason) {
    connected_ = false;
    notify_enabled_ = false;
    conn_handle_ = 0xFFFF;
}

void AppBleManager::OnConnected(uint16_t conn_handle) {
    conn_handle_ = conn_handle;
    connected_ = true;
    notify_enabled_ = false;
    ESP_LOGI(TAG, "client connected, conn_handle=%u", conn_handle);
}

void AppBleManager::OnDisconnected(int reason) {
    (void)reason;
    connected_ = false;
    notify_enabled_ = false;
    conn_handle_ = 0xFFFF;
}

void AppBleManager::OnSubscribed(bool notify_enabled) {
    notify_enabled_ = notify_enabled;
}

void AppBleManager::OnRx(const uint8_t* data, uint16_t len) {
    if (len == 0 || cmd_queue_ == nullptr) {
        return;
    }
    if (len > kMaxPacket) {
        ESP_LOGW(TAG, "command too long: %u", len);
        return;
    }
    Packet packet = {};
    packet.len = len;
    memcpy(packet.data, data, len);
    if (xQueueSend(cmd_queue_, &packet, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, drop");
    }
}

}  // namespace appdemo
