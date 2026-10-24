#include "app_demo.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstring>

#include "app_ble_manager.h"
#include "app_device_status.h"
#include "app_mqtt_manager.h"
#include "app_network_manager.h"
#include "app_sensor_reporter.h"
#include "board.h"

static const char* TAG = "AppDemo";

namespace appdemo {

AppDemo& AppDemo::GetInstance() {
    static AppDemo instance;
    return instance;
}

void AppDemo::Start() {
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "ESP32-S3 AI Assistant V2 App Demo firmware %s",
             AppDeviceStatus::kFirmwareVersion);
    ESP_LOGI(TAG, "BLE service 8F1D0001-9B4A-4C2E-9D3F-7A6B5C4D3E2F");
    ESP_LOGI(TAG, "audio/cloud/AI disabled, BLE + route + sensors only");
    ESP_LOGI(TAG, "==================================================");

    // 强制构造板级对象：屏幕、按键、SensorHub、GPS 都会在这里完成初始化
    Board::GetInstance();

    AppBleManager::GetInstance().Start(
        [this](const std::string& json) { HandleCommand(json); });

    // cloud_state 变化推送给 App（type=cloud，不含密码）
    AppMqttManager::GetInstance().SetCloudStateCallback([](const std::string& json) {
        AppBleManager::GetInstance().Send(json);
    });

    auto& network = AppNetworkManager::GetInstance();
    network.SetNotifyCallback([](const std::string& json) {
        AppBleManager::GetInstance().Send(json);
        AppMqttManager::GetInstance().OnNetworkJson(json);
    });
    network.SetRouteChangeCallback([](SelectedRoute route) {
        // 路由切换后旧 socket 不能跨 interface 复用，交给 MqttManager 关闭并重建
        ESP_LOGW(TAG, "product internet route -> %s, close old business connections",
                 RouteToString(route));
        AppMqttManager::GetInstance().OnRouteChanged(route);
    });
    network.Start();

    AppSensorReporter::GetInstance().Start();
    AppMqttManager::GetInstance().Start();
}

namespace {

// cloud_provision 参数解析：只取字段，不打印 password
bool ParseCloudProvision(cJSON* root, CloudConfig* config, const char** error) {
    cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (version != nullptr &&
        (!cJSON_IsNumber(version) || static_cast<int>(version->valuedouble) != 1)) {
        *error = kErrUnsupportedVersion;
        return false;
    }
    cJSON* device_id = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    cJSON* host = cJSON_GetObjectItemCaseSensitive(root, "host");
    cJSON* port = cJSON_GetObjectItemCaseSensitive(root, "port");
    cJSON* username = cJSON_GetObjectItemCaseSensitive(root, "username");
    cJSON* password = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsString(device_id) || !cJSON_IsString(host) || !cJSON_IsNumber(port) ||
        !cJSON_IsString(username) || !cJSON_IsString(password)) {
        *error = kErrInvalidParam;
        return false;
    }
    config->device_id = device_id->valuestring;
    config->host = host->valuestring;
    config->port = static_cast<int>(port->valuedouble);
    config->username = username->valuestring;
    config->password = password->valuestring;
    *error = kErrNone;
    return true;
}

}  // namespace

void AppDemo::SendAck(int id, bool ok, const char* error, const char* cmd) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddNumberToObject(root, "id", id);
    if (cmd != nullptr) {
        cJSON_AddStringToObject(root, "cmd", cmd);
    }
    if (ok) {
        cJSON_AddBoolToObject(root, "ok", true);
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "err", error != nullptr ? error : kErrInternal);
    }
    char* text = cJSON_PrintUnformatted(root);
    if (text != nullptr) {
        AppBleManager::GetInstance().Send(text);
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

void AppDemo::HandleCommand(const std::string& json) {
    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    if (root == nullptr) {
        // 不打印原始 payload：cloud_provision 里可能含 password
        ESP_LOGW(TAG, "invalid json len=%u", static_cast<unsigned>(json.size()));
        SendAck(0, false, kErrInvalidJson);
        return;
    }

    cJSON* type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON* id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON* cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    int id = cJSON_IsNumber(id_item) ? static_cast<int>(id_item->valuedouble) : 0;
    const char* type = cJSON_IsString(type_item) ? type_item->valuestring : "";
    const char* cmd = cJSON_IsString(cmd_item) ? cmd_item->valuestring : "";

    if (strcmp(type, "cmd") != 0 || cmd[0] == '\0') {
        SendAck(id, false, kErrInvalidJson);
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "cmd id=%d cmd=%s", id, cmd);

    if (strcmp(cmd, "get_capabilities") == 0) {
        SendAck(id, true);
        AppSensorReporter::GetInstance().SendCapabilities();
    } else if (strcmp(cmd, "get_status") == 0) {
        SendAck(id, true);
        AppSensorReporter::GetInstance().SendAllNow();
    } else if (strcmp(cmd, "set_route") == 0) {
        cJSON* route_item = cJSON_GetObjectItemCaseSensitive(root, "route");
        bool route_ok = false;
        SelectedRoute route = SelectedRoute::WIFI;
        if (cJSON_IsString(route_item)) {
            route = RouteFromString(route_item->valuestring, &route_ok);
        }
        if (!route_ok) {
            SendAck(id, false, kErrInvalidRoute);
        } else {
            // 先改 selected_route 并立即落 NVS，再 ACK，最后异步重新初始化
            AppNetworkManager::GetInstance().SetSelectedRoute(route);
            SendAck(id, true);
            AppNetworkManager::GetInstance().BeginRouteInit();
        }
    } else if (strcmp(cmd, "reinit_route") == 0) {
        SendAck(id, true);
        AppNetworkManager::GetInstance().BeginRouteInit();
    } else if (strcmp(cmd, "cloud_provision") == 0) {
        CloudConfig config;
        const char* error = kErrNone;
        if (!ParseCloudProvision(root, &config, &error)) {
            SendAck(id, false, error, cmd);
        } else if (!AppMqttManager::GetInstance().ApplyCloudProvision(config, &error)) {
            SendAck(id, false, error, cmd);
        } else {
            // ACK 只表示“配置已接受并写入 NVS”，连接结果由后续 type=cloud 推送
            SendAck(id, true, nullptr, cmd);
        }
    } else if (strcmp(cmd, "get_cloud_status") == 0) {
        SendAck(id, true, nullptr, cmd);
        AppBleManager::GetInstance().Send(AppMqttManager::GetInstance().BuildCloudJson());
    } else if (strcmp(cmd, "cloud_clear") == 0) {
        AppMqttManager::GetInstance().ClearCloudProvision();
        SendAck(id, true, nullptr, cmd);
    } else {
        SendAck(id, false, kErrUnknownCmd);
    }

    cJSON_Delete(root);
}

}  // namespace appdemo
