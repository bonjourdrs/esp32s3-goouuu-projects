#include "app_demo.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstring>

#include "app_ble_manager.h"
#include "app_device_status.h"
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

    auto& network = AppNetworkManager::GetInstance();
    network.SetNotifyCallback([](const std::string& json) {
        AppBleManager::GetInstance().Send(json);
    });
    network.SetRouteChangeCallback([](SelectedRoute route) {
        // Demo 里没有云端/WebSocket 业务连接；真实产品在这里关闭旧通路上的
        // socket，待目标通路初始化完成后再重建，避免 selected_route 与实际
        // 出网接口不一致。
        ESP_LOGW(TAG, "product internet route -> %s, close old business connections",
                 RouteToString(route));
    });
    network.Start();

    AppSensorReporter::GetInstance().Start();
}

void AppDemo::SendAck(int id, bool ok, const char* error) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddNumberToObject(root, "id", id);
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
        ESP_LOGW(TAG, "invalid json: %s", json.c_str());
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
    } else {
        SendAck(id, false, kErrUnknownCmd);
    }

    cJSON_Delete(root);
}

}  // namespace appdemo
