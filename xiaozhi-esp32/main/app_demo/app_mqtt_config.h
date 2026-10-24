#ifndef APP_MQTT_CONFIG_H
#define APP_MQTT_CONFIG_H

// EMQX 默认参数：仅作为开发 fallback（真实凭据由 BLE cloud_provision 写入 NVS）
// 这里只放非机密字段，密码只在 mqtt_secrets.h / NVS 中
namespace appdemo {
namespace mqttcfg {

constexpr const char* kHost = "k1eec7cc.ala.cn-hangzhou.emqxsl.cn";
constexpr int kPort = 8883;
constexpr const char* kDeviceId = "esp32_demo_001";
constexpr const char* kUsername = "device_esp32_demo_001";

// Topic 后缀，Base = v1/devices/<device_id>
constexpr const char* kSuffixPresence = "/presence";
constexpr const char* kSuffixStatus = "/status";
constexpr const char* kSuffixEnv = "/telemetry/env";
constexpr const char* kSuffixBaro = "/telemetry/baro";
constexpr const char* kSuffixGps = "/telemetry/gps";
constexpr const char* kSuffixWifi = "/network/wifi";
constexpr const char* kSuffixCell = "/network/cell";  // EMQX ACL 固定 cell，不是 cellular
constexpr const char* kSuffixEvent = "/event";
constexpr const char* kSuffixAck = "/ack";
constexpr const char* kSuffixCmd = "/cmd";

// 调度参数（第一阶段）
constexpr int kTelemetryIntervalMs = 5000;   // env / baro
constexpr int kGpsIntervalMs = 5000;         // 有 fix
constexpr int kGpsNoFixIntervalMs = 30000;   // 无 fix 时降频，避免重复上传旧坐标
constexpr int kStatusIntervalMs = 30000;     // status 心跳（retain），约 30 秒
constexpr int kConnectTimeoutMs = 15000;
constexpr int kRecentCmdCacheSize = 16;

}  // namespace mqttcfg
}  // namespace appdemo

#endif  // APP_MQTT_CONFIG_H
