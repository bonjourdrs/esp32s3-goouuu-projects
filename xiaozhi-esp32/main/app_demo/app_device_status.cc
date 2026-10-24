#include "app_device_status.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>

namespace appdemo {

uint32_t AppDeviceStatus::UptimeSeconds() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000);
}

size_t AppDeviceStatus::FreeHeap() {
    return esp_get_free_heap_size();
}

size_t AppDeviceStatus::FreePsram() {
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

std::string AppDeviceStatus::BuildJson(const char* cloud_state) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "device");
    cJSON_AddNumberToObject(root, "uptime", UptimeSeconds());
    cJSON_AddNumberToObject(root, "heap", static_cast<double>(FreeHeap()));
    cJSON_AddNumberToObject(root, "psram", static_cast<double>(FreePsram()));
    cJSON_AddStringToObject(root, "fw", kFirmwareVersion);
    cJSON_AddStringToObject(root, "cloud_state", cloud_state != nullptr ? cloud_state : "disabled");
    char* text = cJSON_PrintUnformatted(root);
    std::string result = text != nullptr ? text : "{}";
    if (text != nullptr) {
        cJSON_free(text);
    }
    cJSON_Delete(root);
    return result;
}

}  // namespace appdemo
