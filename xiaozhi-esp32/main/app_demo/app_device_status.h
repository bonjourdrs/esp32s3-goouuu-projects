#ifndef APP_DEVICE_STATUS_H
#define APP_DEVICE_STATUS_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace appdemo {

// ESP32-S3 自身状态（运行时间 / 内存 / 固件版本）
class AppDeviceStatus {
public:
    static constexpr const char* kFirmwareVersion = "0.2-demo";

    static uint32_t UptimeSeconds();
    static size_t FreeHeap();
    static size_t FreePsram();
    static std::string BuildJson();
};

}  // namespace appdemo

#endif  // APP_DEVICE_STATUS_H
