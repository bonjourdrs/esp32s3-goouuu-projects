#ifndef APP_BLE_MANAGER_H
#define APP_BLE_MANAGER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace appdemo {

// BLE 管理/控制/状态通道（V2 接口冻结）：
//  Service 8F1D0001-9B4A-4C2E-9D3F-7A6B5C4D3E2F
//  RX      8F1D0002-9B4A-4C2E-9D3F-7A6B5C4D3E2F  (App -> 设备, Write / Write Without Response)
//  TX      8F1D0003-9B4A-4C2E-9D3F-7A6B5C4D3E2F  (设备 -> App, Read / Notify)
class AppBleManager {
public:
    static AppBleManager& GetInstance();

    using CommandHandler = std::function<void(const std::string&)>;

    bool Start(CommandHandler handler);
    // 线程安全，可在任意任务里调用；未连接或未订阅时直接丢弃
    bool Send(const std::string& json);
    // 只保留最新一帧（高频 IMU 专用）：BLE 来不及发时丢弃旧样本，不排队补发
    bool SendLatest(const std::string& json);

    bool IsConnected() const { return connected_.load(); }
    bool CanNotify() const { return connected_.load() && notify_enabled_.load(); }

    // NimBLE 回调转发入口
    void OnHostSync();
    void OnHostReset(int reason);
    void OnConnected(uint16_t conn_handle);
    void OnDisconnected(int reason);
    void OnSubscribed(bool notify_enabled);
    void OnRx(const uint8_t* data, uint16_t len);
    uint16_t ConnHandle() const { return conn_handle_; }

private:
    AppBleManager() = default;
    AppBleManager(const AppBleManager&) = delete;
    AppBleManager& operator=(const AppBleManager&) = delete;

    static constexpr uint16_t kMaxPacket = 512;

    struct Packet {
        uint16_t len;
        char data[kMaxPacket];
    };

    static void TxTaskEntry(void* arg);
    static void CommandTaskEntry(void* arg);
    void TxLoop();
    void CommandLoop();

    QueueHandle_t tx_queue_ = nullptr;
    QueueHandle_t latest_queue_ = nullptr;  // 深度 1，新帧覆盖旧帧
    QueueHandle_t cmd_queue_ = nullptr;
    TaskHandle_t tx_task_ = nullptr;
    TaskHandle_t cmd_task_ = nullptr;

    CommandHandler command_handler_;

    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> notify_enabled_{false};
    std::atomic<uint16_t> conn_handle_{0xFFFF};
};

}  // namespace appdemo

#endif  // APP_BLE_MANAGER_H
