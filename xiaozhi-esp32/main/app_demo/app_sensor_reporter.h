#ifndef APP_SENSOR_REPORTER_H
#define APP_SENSOR_REPORTER_H

#include <atomic>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace appdemo {

// 传感器 / GPS / 设备状态 / 网络状态的周期上报。
// 只有 BLE 已连接且 App 已订阅 TX Notify 时才产生数据，避免队列堆积。
class AppSensorReporter {
public:
    static AppSensorReporter& GetInstance();

    void Start();
    void SendAllNow();
    void SendCapabilities();

    std::string BuildCapabilitiesJson();
    std::string BuildEnvJson();
    std::string BuildBaroJson();
    std::string BuildImuJson();
    std::string BuildGpsJson();

private:
    AppSensorReporter() = default;
    AppSensorReporter(const AppSensorReporter&) = delete;
    AppSensorReporter& operator=(const AppSensorReporter&) = delete;

    static void TaskEntry(void* arg);
    void Loop();
    bool SendIfReady(const std::string& json);

    std::atomic<bool> started_{false};
    TaskHandle_t task_ = nullptr;
};

}  // namespace appdemo

#endif  // APP_SENSOR_REPORTER_H
