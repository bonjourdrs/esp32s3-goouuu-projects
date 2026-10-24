#include "app_sensor_reporter.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cmath>

#include "app_ble_manager.h"
#include "app_device_status.h"
#include "app_mqtt_manager.h"
#include "app_network_manager.h"
#include "gps_receiver.h"
#include "sensor_hub.h"

static const char* TAG = "AppSensors";

namespace appdemo {

namespace {
constexpr uint32_t kImuIntervalMs = 100;  // 10Hz，兼顾 3D 姿态流畅度与 App 处理负载
constexpr uint32_t kEnvIntervalMs = 2000;
constexpr uint32_t kBaroIntervalMs = 2000;
constexpr uint32_t kGpsIntervalMs = 1000;
constexpr uint32_t kDeviceIntervalMs = 5000;
constexpr uint32_t kWifiIntervalMs = 2000;
constexpr uint32_t kCellIntervalMs = 3000;

// 控制 JSON 数字位数，保证 20Hz IMU 单条报文能放进 MTU 247
double RoundTo(double value, int decimals) {
    double scale = std::pow(10.0, decimals);
    return std::round(value * scale) / scale;
}

std::string JsonToString(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    std::string result = text != nullptr ? text : "{}";
    if (text != nullptr) {
        cJSON_free(text);
    }
    cJSON_Delete(root);
    return result;
}

// 协议冻结：motion 只能是 still / moving / shake
const char* MotionToProtocol(const std::string& state) {
    if (state == "walking") {
        return "moving";
    }
    if (state == "shaking" || state == "impact") {
        return "shake";
    }
    return "still";
}

// 协议冻结：pose 只能是 face_up / face_down / portrait / landscape
const char* PoseToProtocol(const std::string& orientation) {
    if (orientation == "face_up") {
        return "face_up";
    }
    if (orientation == "face_down") {
        return "face_down";
    }
    if (orientation == "landscape_left" || orientation == "landscape_right") {
        return "landscape";
    }
    return "portrait";
}
}  // namespace

AppSensorReporter& AppSensorReporter::GetInstance() {
    static AppSensorReporter instance;
    return instance;
}

void AppSensorReporter::Start() {
    if (started_.exchange(true)) {
        return;
    }
    xTaskCreate(TaskEntry, "app_sensors", 6144, this, 3, &task_);
}

void AppSensorReporter::TaskEntry(void* arg) {
    static_cast<AppSensorReporter*>(arg)->Loop();
    vTaskDelete(nullptr);
}

bool AppSensorReporter::SendIfReady(const std::string& json) {
    if (!AppBleManager::GetInstance().CanNotify()) {
        return false;
    }
    return AppBleManager::GetInstance().Send(json);
}

void AppSensorReporter::SendAllNow() {
    auto& net = AppNetworkManager::GetInstance();
    SendIfReady(net.BuildRouteJson());
    SendIfReady(net.BuildWifiJson());
    SendIfReady(net.BuildCellJson());
    SendIfReady(BuildEnvJson());
    SendIfReady(BuildBaroJson());
    SendIfReady(BuildImuJson());
    SendIfReady(BuildGpsJson());
    SendIfReady(AppDeviceStatus::BuildJson(
        AppMqttManager::GetInstance().GetCloudStateString()));
}

void AppSensorReporter::SendCapabilities() {
    SendIfReady(BuildCapabilitiesJson());
}

void AppSensorReporter::Loop() {
    uint32_t last_imu = 0;
    uint32_t last_env = 0;
    uint32_t last_baro = 0;
    uint32_t last_gps = 0;
    uint32_t last_device = 0;
    uint32_t last_wifi = 0;
    uint32_t last_cell = 0;
    bool was_fix = false;
    bool primed = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (!AppBleManager::GetInstance().CanNotify()) {
            primed = false;
            continue;
        }
        uint32_t now = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (!primed) {
            SendAllNow();
            last_imu = last_env = last_baro = last_gps = last_device = now;
            last_wifi = last_cell = now;
            GpsFix fix = GpsReceiver::GetInstance().GetFix();
            was_fix = fix.valid;
            primed = true;
            continue;
        }

        if (now - last_imu >= kImuIntervalMs) {
            last_imu = now;
            std::string imu = BuildImuJson();
            static int imu_log_counter = 0;
            if (++imu_log_counter % 20 == 0) {
                ESP_LOGI(TAG, "imu(%u): %s", static_cast<unsigned>(imu.size()), imu.c_str());
            }
            // 高频姿态只发最新一帧，BLE 拥塞时丢弃旧样本而不是排队补发
            AppBleManager::GetInstance().SendLatest(imu);
        }
        if (now - last_env >= kEnvIntervalMs) {
            last_env = now;
            SendIfReady(BuildEnvJson());
        }
        if (now - last_baro >= kBaroIntervalMs) {
            last_baro = now;
            SendIfReady(BuildBaroJson());
        }
        if (now - last_wifi >= kWifiIntervalMs) {
            last_wifi = now;
            SendIfReady(AppNetworkManager::GetInstance().BuildWifiJson());
        }
        if (now - last_cell >= kCellIntervalMs) {
            last_cell = now;
            SendIfReady(AppNetworkManager::GetInstance().BuildCellJson());
        }
        if (now - last_device >= kDeviceIntervalMs) {
            last_device = now;
            SendIfReady(AppDeviceStatus::BuildJson(
        AppMqttManager::GetInstance().GetCloudStateString()));
        }

        GpsFix fix = GpsReceiver::GetInstance().GetFix();
        // Fix 状态变化立即上报，周期上报仍保留
        if (fix.valid != was_fix) {
            was_fix = fix.valid;
            last_gps = now;
            SendIfReady(BuildGpsJson());
        } else if (now - last_gps >= kGpsIntervalMs) {
            last_gps = now;
            SendIfReady(BuildGpsJson());
        }
    }
}

std::string AppSensorReporter::BuildCapabilitiesJson() {
    EnvironmentReading env = SensorHub::GetInstance().GetEnvironment();
    MotionReading motion = SensorHub::GetInstance().GetMotion();
    bool gps = GpsReceiver::GetInstance().IsStarted();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "cap");
    cJSON_AddNumberToObject(root, "wifi", 1);
    cJSON_AddNumberToObject(root, "4g", 1);
    cJSON_AddNumberToObject(root, "aht20", env.aht_present ? 1 : 0);
    cJSON_AddNumberToObject(root, "bmp280", env.bmp_present ? 1 : 0);
    cJSON_AddNumberToObject(root, "mpu6050", motion.present ? 1 : 0);
    cJSON_AddNumberToObject(root, "gps", gps ? 1 : 0);
    return JsonToString(root);
}

std::string AppSensorReporter::BuildEnvJson() {
    EnvironmentReading env = SensorHub::GetInstance().GetEnvironment();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "env");
    if (!env.aht_present) {
        cJSON_AddStringToObject(root, "err", "AHT20_NOT_FOUND");
        return JsonToString(root);
    }
    if (!env.temperature_valid || !env.humidity_valid) {
        cJSON_AddStringToObject(root, "err", "AHT20_READ_FAILED");
        return JsonToString(root);
    }
    cJSON_AddNumberToObject(root, "temp", env.temperature_c);
    cJSON_AddNumberToObject(root, "hum", env.humidity_pct);
    return JsonToString(root);
}

std::string AppSensorReporter::BuildBaroJson() {
    EnvironmentReading env = SensorHub::GetInstance().GetEnvironment();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "baro");
    if (!env.bmp_present) {
        cJSON_AddStringToObject(root, "err", "BMP280_NOT_FOUND");
        return JsonToString(root);
    }
    if (!env.pressure_valid) {
        cJSON_AddStringToObject(root, "err", "BMP280_READ_FAILED");
        return JsonToString(root);
    }
    cJSON_AddNumberToObject(root, "pressure", env.pressure_hpa);
    if (env.bmp_temperature_valid) {
        cJSON_AddNumberToObject(root, "chip_temp", env.bmp_temperature_c);
    }
    return JsonToString(root);
}

std::string AppSensorReporter::BuildImuJson() {
    MotionReading motion = SensorHub::GetInstance().GetMotion();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "imu");
    if (!motion.present) {
        cJSON_AddStringToObject(root, "err", "MPU6050_NOT_FOUND");
        return JsonToString(root);
    }
    if (!motion.valid) {
        cJSON_AddStringToObject(root, "err", "MPU6050_READ_FAILED");
        return JsonToString(root);
    }
    cJSON_AddNumberToObject(root, "ax", RoundTo(motion.accel_g[0], 3));
    cJSON_AddNumberToObject(root, "ay", RoundTo(motion.accel_g[1], 3));
    cJSON_AddNumberToObject(root, "az", RoundTo(motion.accel_g[2], 3));
    cJSON_AddNumberToObject(root, "gx", RoundTo(motion.gyro_dps[0], 2));
    cJSON_AddNumberToObject(root, "gy", RoundTo(motion.gyro_dps[1], 2));
    cJSON_AddNumberToObject(root, "gz", RoundTo(motion.gyro_dps[2], 2));
    if (motion.attitude_valid) {
        // 四元数姿态（App V2.1 自行转 Roll/Pitch/Yaw 驱动 3D Cube）
        cJSON_AddNumberToObject(root, "qw", RoundTo(motion.quat_wxyz[0], 4));
        cJSON_AddNumberToObject(root, "qx", RoundTo(motion.quat_wxyz[1], 4));
        cJSON_AddNumberToObject(root, "qy", RoundTo(motion.quat_wxyz[2], 4));
        cJSON_AddNumberToObject(root, "qz", RoundTo(motion.quat_wxyz[3], 4));
    }
    cJSON_AddStringToObject(root, "motion", MotionToProtocol(motion.state));
    cJSON_AddStringToObject(root, "pose", PoseToProtocol(motion.orientation));
    return JsonToString(root);
}

std::string AppSensorReporter::BuildGpsJson() {
    GpsFix fix = GpsReceiver::GetInstance().GetFix();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "gps");
    if (!fix.started) {
        cJSON_AddNumberToObject(root, "fix", 0);
        cJSON_AddStringToObject(root, "err", "GPS_DISABLED");
        return JsonToString(root);
    }
    if (!fix.valid) {
        // 明确区分“当前无定位”，不上报旧坐标
        cJSON_AddNumberToObject(root, "fix", 0);
        return JsonToString(root);
    }
    cJSON_AddNumberToObject(root, "fix", 1);
    cJSON_AddNumberToObject(root, "lat", fix.latitude);
    cJSON_AddNumberToObject(root, "lon", fix.longitude);
    cJSON_AddNumberToObject(root, "alt", fix.altitude_m);
    cJSON_AddNumberToObject(root, "speed", fix.speed_kmh);
    cJSON_AddNumberToObject(root, "course", fix.course_deg);
    cJSON_AddNumberToObject(root, "sat", fix.satellites);
    cJSON_AddNumberToObject(root, "hdop", fix.hdop);
    return JsonToString(root);
}

}  // namespace appdemo
