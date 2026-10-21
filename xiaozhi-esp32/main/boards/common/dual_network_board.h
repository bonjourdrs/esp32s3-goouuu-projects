#ifndef DUAL_NETWORK_BOARD_H
#define DUAL_NETWORK_BOARD_H

#include "board.h"
#include "wifi_board.h"
#include "ml307_board.h"
#include <esp_timer.h>
#include <atomic>
#include <memory>

//enum NetworkType
enum class NetworkType {
    WIFI,
    ML307
};

// 网络模式：自动（WiFi 优先，连不上切 4G）/ 仅 WiFi / 仅 4G
enum class NetworkMode {
    AUTO,
    WIFI_ONLY,
    ML307_ONLY
};

// 双网络板卡类，可以在WiFi和ML307之间切换
class DualNetworkBoard : public Board {
private:
    // 使用基类指针存储当前活动的板卡
    std::unique_ptr<Board> current_board_;
    // 自动降级后保留旧的 WiFi 板卡，避免 WifiManager 内部回调指向已释放对象
    std::unique_ptr<Board> retired_board_;
    // 以下状态会被网络回调、esp_timer 回调和主任务同时读写，使用原子类型避免数据竞争
    std::atomic<NetworkType> network_type_{NetworkType::ML307};  // Default to ML307
    std::atomic<NetworkMode> mode_{NetworkMode::ML307_ONLY};
    NetworkEventCallback network_event_callback_;
    esp_timer_handle_t wifi_fallback_timer_ = nullptr;
    // 仅 4G 模式下模组持续不可用时的兜底定时器
    esp_timer_handle_t cellular_recovery_timer_ = nullptr;
    // 自动模式下曾成功连上 WiFi，掉线后才值得重新计时兜底
    std::atomic<bool> wifi_was_connected_{false};
    // 自动模式下 4G 不可用时只安排一次重启，避免反复触发
    std::atomic<bool> cellular_recovery_scheduled_{false};

    // ML307的引脚配置
    gpio_num_t ml307_tx_pin_;
    gpio_num_t ml307_rx_pin_;
    gpio_num_t ml307_dtr_pin_;
    int ml307_baud_rate_;

    // 从Settings加载网络模式
    NetworkMode LoadNetworkModeFromSettings(int32_t default_net_type);

    // 保存网络模式到Settings
    void SaveNetworkModeToSettings(NetworkMode mode);

    // 初始化当前网络模式对应的板卡
    void InitializeCurrentBoard();

    // 启动蜂窝网络（自动模式下作为 WiFi 的兜底）
    void StartCellular();

    // 把当前板卡的网络事件转发给上层回调
    void ForwardNetworkEvent(NetworkEvent event, const std::string& data);

    static void OnWifiFallbackTimeout(void* arg);
    static void OnCellularRecoveryTimeout(void* arg);

public:
    DualNetworkBoard(gpio_num_t ml307_tx_pin, gpio_num_t ml307_rx_pin, gpio_num_t ml307_dtr_pin = GPIO_NUM_NC, int32_t default_net_type = 1, int ml307_baud_rate = 921600);
    virtual ~DualNetworkBoard();

    // 切换网络类型（WiFi <-> 4G，写入设置后重启）
    void SwitchNetworkType();

    // 按 自动 -> 仅WiFi -> 仅4G -> 自动 循环，写入设置后重启
    void CycleNetworkMode();

    // 仅保存网络模式，重启后生效
    void SetNetworkMode(NetworkMode mode);

    // 获取当前网络类型
    NetworkType GetNetworkType() const { return network_type_; }

    // 获取配置的网络模式
    NetworkMode GetNetworkMode() const { return mode_; }

    // auto / wifi / 4g
    const char* GetNetworkModeName() const;

    // 当前是否使用 WiFi 链路
    bool IsWifiActive() const { return network_type_ == NetworkType::WIFI; }

    // 当前网络是否已就绪
    bool IsNetworkConnected() const;

    // 进入 WiFi 配网模式；非 WiFi 链路时提示用户先切换模式
    bool EnterWifiConfigMode();

    // 获取当前活动的板卡引用
    Board& GetCurrentBoard() const { return *current_board_; }

    // 重写Board接口
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual std::string GetBoardJson() override;
    virtual std::string GetDeviceStatusJson() override;
};

#endif // DUAL_NETWORK_BOARD_H
