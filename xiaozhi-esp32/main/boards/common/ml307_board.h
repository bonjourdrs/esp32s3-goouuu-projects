#ifndef ML307_BOARD_H
#define ML307_BOARD_H

#include <atomic>
#include <memory>
#include <at_modem.h>
#include <esp_timer.h>
#include "board.h"


class Ml307Board : public Board {
protected:
    std::unique_ptr<AtModem> modem_;
    gpio_num_t tx_pin_;
    gpio_num_t rx_pin_;
    gpio_num_t dtr_pin_;
    int baud_rate_;
    NetworkEventCallback network_event_callback_;

    // 模组重启（+MATREADY）后的恢复窗口：短时间内不向上层报断网，
    // 等模组重新注册成功就直接恢复，避免界面跳回联网页、误触发网络切换
    esp_timer_handle_t recovery_timer_ = nullptr;
    std::atomic<bool> recovery_pending_{false};
    std::atomic<int64_t> recovery_deadline_us_{0};

    void HandleModemStateChanged(bool network_ready);
    void OnRecoveryTick();

    virtual std::string GetBoardJson() override;

    // Internal helper to trigger network event callback
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");
    
    // Network initialization task (runs in FreeRTOS task)
    static void NetworkTaskEntry(void* arg);
    void NetworkTask();

public:
    Ml307Board(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin = GPIO_NUM_NC,
               int baud_rate = 921600);
    virtual ~Ml307Board();
    virtual std::string GetBoardType() override;
    bool IsNetworkReady() const { return modem_ != nullptr && modem_->network_ready(); }
    int GetCsq() const { return modem_ != nullptr ? modem_->GetCsq() : -1; }
    virtual void StartNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
};

#endif // ML307_BOARD_H
