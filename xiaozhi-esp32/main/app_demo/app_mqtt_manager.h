#ifndef APP_MQTT_MANAGER_H
#define APP_MQTT_MANAGER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mqtt_client.h>

#include "app_types.h"

namespace appdemo {

// Cloud 凭据：由 BLE cloud_provision 下发后写入 NVS
struct CloudConfig {
    std::string device_id;
    std::string host;
    int port = 8883;
    std::string username;
    std::string password;
};

// MQTT Cloud 通道（第一阶段：Wi-Fi + TLS + get_status）
//
// 设计约束：
//  - 独立 task，不在 sensor / BLE / network 回调里做 TLS 与 socket 写；
//  - 严格跟随 selected_route，绝不因为 Wi-Fi 失败而改走 4G；
//  - cmd 只接受 QoS1、retain=false，并对重复 id 做去重；
//  - 传感器数据不新增读取逻辑，直接复用 AppSensorReporter / AppNetworkManager 的 JSON；
//  - 凭据来源优先级：NVS（BLE 下发） > mqtt_secrets.h 开发 fallback > disabled。
class AppMqttManager {
public:
    static AppMqttManager& GetInstance();

    void Start();

    // 网络状态变化（来自 AppNetworkManager 的 Notify 回调，wifi / cell / route）
    void OnNetworkJson(const std::string& json);
    // selected_route 实际发生切换：关闭旧 socket，等新链路 ready 后再重连
    void OnRouteChanged(SelectedRoute route);

    // cloud_state 变化时推送给 BLE（payload 为 type=cloud JSON，不含密码）
    void SetCloudStateCallback(std::function<void(const std::string&)> callback) {
        cloud_state_callback_ = std::move(callback);
    }

    // cloud_provision：校验 + 写 NVS + 异步重建连接。失败返回 false 并给出错误码。
    bool ApplyCloudProvision(const CloudConfig& config, const char** error);
    // cloud_clear：断开 MQTT、清除 NVS 凭据
    void ClearCloudProvision();

    // get_cloud_status 与状态推送共用；绝不包含 password
    std::string BuildCloudJson() const;

    // 供 BLE device JSON 展示 cloud_state
    const char* GetCloudStateString() const;

private:
    enum class State : uint8_t {
        DISABLED = 0,
        WAIT_NETWORK,
        CONNECTING,
        ONLINE,
        BACKOFF,
        ERROR,
    };

    // 对外暴露到 cloud JSON 的错误码（不包含任何凭据信息）
    enum class CloudError : uint8_t {
        NONE = 0,
        AUTH_FAILED,
        CONNECTION_REFUSED,
        TLS_FAILED,
        TRANSPORT_ERROR,
        TIMEOUT,
    };

    struct Topics {
        std::string presence;
        std::string status;
        std::string env;
        std::string baro;
        std::string gps;
        std::string wifi;
        std::string cell;
        std::string event;
        std::string ack;
        std::string cmd;
    };

    AppMqttManager() = default;
    AppMqttManager(const AppMqttManager&) = delete;
    AppMqttManager& operator=(const AppMqttManager&) = delete;

    static void TaskEntry(void* arg);
    static void EventHandler(void* handler_args, esp_event_base_t base, int32_t event_id,
                             void* event_data);

    void Loop();
    void LoadCloudConfig();
    void RebuildDerived(const CloudConfig& config);
    void ApplyPendingConfig();
    void PersistCloudConfig(const CloudConfig& config);
    void EraseCloudConfig();
    bool SelectedRouteHasInternet();
    std::string WithSeq(const std::string& json);
    std::string DeviceId() const;
    static const char* CloudErrorToString(CloudError error);
    bool StartClient();
    void StopClient();
    void HandleEvent(esp_mqtt_event_t* event);
    void HandleCommand(const std::string& payload, bool retained);
    void SetState(State next, CloudError error = CloudError::NONE, const char* reason = nullptr);
    void NotifyCloudState();
    void LogState();

    void Publish(const char* topic, const std::string& payload, int qos, bool retain);
    void PublishPresence(bool online);
    void PublishStatus();
    void PublishEnv();
    void PublishBaro();
    void PublishGps();
    void PublishNetworkSnapshot();
    void PublishPendingNetworkJson();
    void PublishAck(int id, bool ok, const char* error = nullptr);
    void PublishAckRaw(const std::string& payload);

    struct RecentCmd {
        int id = -1;
        std::string ack;
    };

    std::atomic<bool> started_{false};
    std::atomic<State> state_{State::DISABLED};
    std::atomic<int> last_error_{static_cast<int>(CloudError::NONE)};
    std::atomic<bool> force_reconnect_{false};
    std::atomic<bool> stop_requested_{false};
    // 连接阶段收到 MQTT_EVENT_ERROR（TLS / 鉴权失败等），由 task 关闭旧 client 后进入 ERROR
    std::atomic<bool> error_pending_{false};

    std::atomic<bool> gps_cell_bl_unsupported_logged_{false};
    std::atomic<bool> no_credentials_logged_{false};

    // provisioned 只由 cloud_provision 置位、cloud_clear 清除；开机从 NVS 恢复
    std::atomic<bool> provisioned_{false};
    // 配置代次：BLE task 只写 config_ + 递增代次，MQTT task 负责停旧 client、应用新配置
    std::atomic<uint32_t> config_gen_{0};
    uint32_t applied_gen_ = 0;
    // 单调递增序号：App 用它判断数据新旧（工程暂无可靠 UTC）
    std::atomic<uint32_t> seq_{0};

    TaskHandle_t task_ = nullptr;
    esp_mqtt_client_handle_t client_ = nullptr;  // 仅在 task_ 内创建/销毁

    mutable std::mutex config_mutex_;
    CloudConfig config_{};
    // 以下派生字段只在 MQTT task 内重建/读取（停旧 client 之后）
    bool credentials_ready_ = false;
    std::string uri_{};
    Topics topics_{};
    std::string lwt_payload_{};

    std::function<void(const std::string&)> cloud_state_callback_;

    // 网络 JSON 只暂存最新一份，由 MQTT task 发送，避免阻塞网络状态机
    std::mutex net_mutex_;
    std::string pending_wifi_{};
    std::string pending_cell_{};
    std::atomic<bool> wifi_dirty_{false};
    std::atomic<bool> cell_dirty_{false};

    RecentCmd recent_[16];
    int recent_count_ = 0;
    int recent_next_ = 0;

    int backoff_index_ = 0;
    TickType_t connect_deadline_ = 0;
    TickType_t retry_at_ = 0;
    TickType_t last_env_tick_ = 0;
    TickType_t last_gps_tick_ = 0;
    TickType_t last_status_tick_ = 0;
};

}  // namespace appdemo

#endif  // APP_MQTT_MANAGER_H
