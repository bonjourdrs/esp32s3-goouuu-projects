#include "dual_network_board.h"
#include "application.h"
#include "display.h"
#include "assets/lang_config.h"
#include "settings.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ssid_manager.h>
#include <wifi_manager.h>
#include <utility>

static const char *TAG = "DualNetworkBoard";

// WiFi 连接尝试时长：超时后自动切到 4G
static constexpr int WIFI_FALLBACK_TIMEOUT_SEC = 20;

DualNetworkBoard::DualNetworkBoard(gpio_num_t ml307_tx_pin, gpio_num_t ml307_rx_pin, gpio_num_t ml307_dtr_pin, int32_t default_net_type, int ml307_baud_rate) 
    : Board(), 
      ml307_tx_pin_(ml307_tx_pin), 
      ml307_rx_pin_(ml307_rx_pin), 
      ml307_dtr_pin_(ml307_dtr_pin),
      ml307_baud_rate_(ml307_baud_rate) {

    esp_timer_create_args_t timer_args = {
        .callback = OnWifiFallbackTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_fallback",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&timer_args, &wifi_fallback_timer_);

    // 从Settings加载网络模式
    mode_ = LoadNetworkModeFromSettings(default_net_type);
    network_type_ = (mode_ == NetworkMode::ML307_ONLY) ? NetworkType::ML307 : NetworkType::WIFI;

    // 只初始化当前网络模式对应的板卡
    InitializeCurrentBoard();
}

DualNetworkBoard::~DualNetworkBoard() {
    if (wifi_fallback_timer_ != nullptr) {
        esp_timer_stop(wifi_fallback_timer_);
        esp_timer_delete(wifi_fallback_timer_);
    }
    if (cellular_recovery_timer_ != nullptr) {
        esp_timer_stop(cellular_recovery_timer_);
        esp_timer_delete(cellular_recovery_timer_);
    }
}

NetworkMode DualNetworkBoard::LoadNetworkModeFromSettings(int32_t default_net_type) {
    Settings settings("network", true);
    int network_type = settings.GetInt("type", default_net_type);
    switch (network_type) {
        case 0:
            return NetworkMode::WIFI_ONLY;
        case 2:
            return NetworkMode::AUTO;
        default:
            return NetworkMode::ML307_ONLY;
    }
}

void DualNetworkBoard::SaveNetworkModeToSettings(NetworkMode mode) {
    Settings settings("network", true);
    int network_type = 1;
    if (mode == NetworkMode::WIFI_ONLY) {
        network_type = 0;
    } else if (mode == NetworkMode::AUTO) {
        network_type = 2;
    }
    settings.SetInt("type", network_type);
}

void DualNetworkBoard::InitializeCurrentBoard() {
    if (network_type_ == NetworkType::ML307) {
        ESP_LOGI(TAG, "Initialize ML307 board");
        current_board_ = std::make_unique<Ml307Board>(ml307_tx_pin_, ml307_rx_pin_, ml307_dtr_pin_, ml307_baud_rate_);
    } else {
        ESP_LOGI(TAG, "Initialize WiFi board");
        current_board_ = std::make_unique<WifiBoard>();
    }
    current_board_->SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        ForwardNetworkEvent(event, data);
    });
}

void DualNetworkBoard::ForwardNetworkEvent(NetworkEvent event, const std::string& data) {
    if (event == NetworkEvent::Connected) {
        // 已经连上，取消自动降级
        if (network_type_ == NetworkType::WIFI) {
            wifi_was_connected_ = true;
        }
        if (wifi_fallback_timer_ != nullptr) {
            esp_timer_stop(wifi_fallback_timer_);
        }
        if (cellular_recovery_timer_ != nullptr) {
            esp_timer_stop(cellular_recovery_timer_);
        }
    } else if (event == NetworkEvent::WifiConfigModeEnter) {
        // 用户正在配网，绝不能在这里切 4G，否则会把配网流程拆掉并卡死状态机
        if (wifi_fallback_timer_ != nullptr) {
            esp_timer_stop(wifi_fallback_timer_);
        }
    } else if (event == NetworkEvent::Disconnected && mode_ == NetworkMode::AUTO &&
               network_type_ == NetworkType::WIFI && wifi_was_connected_) {
        // 自动模式下 WiFi 掉线：给 30 秒重连窗口，仍失败则切 4G
        wifi_was_connected_ = false;
        esp_timer_stop(wifi_fallback_timer_);
        esp_timer_start_once(wifi_fallback_timer_, 30 * 1000000ULL);
    } else if (network_type_ == NetworkType::ML307 &&
               (event == NetworkEvent::ModemErrorInitFailed || event == NetworkEvent::ModemErrorNoSim ||
                event == NetworkEvent::ModemErrorRegDenied ||
                event == NetworkEvent::ModemErrorRegistrationExhausted) &&
               !cellular_recovery_scheduled_) {
        cellular_recovery_scheduled_ = true;
        if (mode_ == NetworkMode::AUTO) {
            // 自动模式下 4G 起不来（没插卡/模组异常）：重启回 WiFi 优先，
            // 否则设备会一直停在启动态，屏幕卡在“检测模组”，唤醒词也不会工作。
            ESP_LOGW(TAG, "Cellular unavailable in auto mode, rebooting to retry WiFi");
            Application::GetInstance().Schedule([this]() {
                if (auto display = GetDisplay()) {
                    display->ShowNotification("4G 不可用，重启后优先连接 WiFi", 3000);
                }
                vTaskDelay(pdMS_TO_TICKS(1500));
                Application::GetInstance().Reboot();
            });
        } else {
            // 仅 4G 模式：给 60 秒自行恢复的机会，仍连不上就切回 WiFi 并进入配网，
            // 避免设备彻底失联（长按 BOOT 也能手动切回）
            ESP_LOGW(TAG, "Cellular unavailable, will fall back to WiFi in 60s");
            if (auto display = GetDisplay()) {
                display->ShowNotification("4G 不可用，60 秒后自动切回 WiFi 配网", 5000);
            }
            if (cellular_recovery_timer_ == nullptr) {
                esp_timer_create_args_t timer_args = {
                    .callback = OnCellularRecoveryTimeout,
                    .arg = this,
                    .dispatch_method = ESP_TIMER_TASK,
                    .name = "cell_recover",
                    .skip_unhandled_events = true,
                };
                esp_timer_create(&timer_args, &cellular_recovery_timer_);
            }
            esp_timer_start_once(cellular_recovery_timer_, 60 * 1000000ULL);
        }
    }
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void DualNetworkBoard::OnWifiFallbackTimeout(void* arg) {
    auto* board = static_cast<DualNetworkBoard*>(arg);
    if (board->network_type_ != NetworkType::WIFI) {
        return;
    }
    // 配网模式下不做任何降级
    if (WifiManager::GetInstance().IsConfigMode()) {
        ESP_LOGI(TAG, "In wifi config mode, skip cellular fallback");
        return;
    }
    ESP_LOGW(TAG, "WiFi connect timeout, falling back to cellular");
    // 定时器回调里不能销毁 WiFi 板卡，交给主任务处理
    Application::GetInstance().Schedule([board]() {
        board->StartCellular();
    });
}

void DualNetworkBoard::OnCellularRecoveryTimeout(void* arg) {
    auto* board = static_cast<DualNetworkBoard*>(arg);
    if (board->network_type_ != NetworkType::ML307 || board->IsNetworkConnected()) {
        return;
    }
    ESP_LOGW(TAG, "Cellular still unavailable, switch to WiFi config mode");
    board->SetNetworkMode(NetworkMode::WIFI_ONLY);
    // 定时器回调里不能直接重启，交给主任务
    Application::GetInstance().Schedule([]() { Application::GetInstance().Reboot(); });
}

void DualNetworkBoard::StartCellular() {
    if (wifi_fallback_timer_ != nullptr) {
        esp_timer_stop(wifi_fallback_timer_);
    }

    // 配网中不允许被切走，否则会卡在 wifi_configuring 状态导致唤醒词失效
    if (WifiManager::GetInstance().IsConfigMode()) {
        ESP_LOGW(TAG, "Cellular fallback blocked: wifi config mode active");
        return;
    }

    auto display = GetDisplay();
    if (display != nullptr) {
        display->ShowNotification(Lang::Strings::SWITCH_TO_4G_NETWORK, 5000);
    }

    // 如果降级发生在配网/启动阶段，把状态机拉回 starting，交给 4G 链路继续激活
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateWifiConfiguring) {
        app.SetDeviceState(kDeviceStateStarting);
    }

    if (network_type_ == NetworkType::WIFI) {
        auto* wifi_board = dynamic_cast<WifiBoard*>(current_board_.get());
        if (wifi_board != nullptr) {
            wifi_board->AbortWifiConnect();
        }
        // WiFi 板卡不再使用，但保留实例：WifiManager 里可能还有指向它的回调
        WifiManager::GetInstance().SetEventCallback([](WifiEvent, const std::string&) {});
        retired_board_ = std::move(current_board_);
    }

    network_type_ = NetworkType::ML307;
    wifi_was_connected_ = false;
    current_board_ = std::make_unique<Ml307Board>(ml307_tx_pin_, ml307_rx_pin_, ml307_dtr_pin_, ml307_baud_rate_);
    current_board_->SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        ForwardNetworkEvent(event, data);
    });

    if (display != nullptr) {
        display->SetStatus(Lang::Strings::DETECTING_MODULE);
    }
    current_board_->StartNetwork();
}

void DualNetworkBoard::SwitchNetworkType() {
    auto display = GetDisplay();
    if (network_type_ == NetworkType::WIFI) {    
        SaveNetworkModeToSettings(NetworkMode::ML307_ONLY);
        mode_ = NetworkMode::ML307_ONLY;
        display->ShowNotification(Lang::Strings::SWITCH_TO_4G_NETWORK);
    } else {
        SaveNetworkModeToSettings(NetworkMode::WIFI_ONLY);
        mode_ = NetworkMode::WIFI_ONLY;
        display->ShowNotification(Lang::Strings::SWITCH_TO_WIFI_NETWORK);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    auto& app = Application::GetInstance();
    app.Reboot();
}

void DualNetworkBoard::CycleNetworkMode() {
    NetworkMode next;
    const char* message = nullptr;
    switch (mode_.load()) {
        case NetworkMode::AUTO:
            next = NetworkMode::WIFI_ONLY;
            message = "网络模式：仅 WiFi";
            break;
        case NetworkMode::WIFI_ONLY:
            next = NetworkMode::ML307_ONLY;
            message = "网络模式：仅 4G";
            break;
        default:
            next = NetworkMode::AUTO;
            message = "网络模式：自动（WiFi 优先）";
            break;
    }

    SaveNetworkModeToSettings(next);
    mode_ = next;
    GetDisplay()->ShowNotification(message);
    vTaskDelay(pdMS_TO_TICKS(1000));
    Application::GetInstance().Reboot();
}

void DualNetworkBoard::SetNetworkMode(NetworkMode mode) {
    SaveNetworkModeToSettings(mode);
    mode_ = mode;
}

const char* DualNetworkBoard::GetNetworkModeName() const {
    switch (mode_.load()) {
        case NetworkMode::AUTO:
            return "auto";
        case NetworkMode::WIFI_ONLY:
            return "wifi";
        default:
            return "4g";
    }
}

bool DualNetworkBoard::IsNetworkConnected() const {
    if (current_board_ == nullptr) {
        return false;
    }
    if (network_type_ == NetworkType::WIFI) {
        return WifiManager::GetInstance().IsConnected();
    }
    auto* ml307_board = dynamic_cast<const Ml307Board*>(current_board_.get());
    return ml307_board != nullptr && ml307_board->IsNetworkReady();
}

bool DualNetworkBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode: mode=%s, network_type=%s", GetNetworkModeName(),
             network_type_ == NetworkType::WIFI ? "wifi" : "ml307");
    if (network_type_ != NetworkType::WIFI) {
        // 当前跑在 4G 上：切回 WiFi 模式并重启，让 WiFi 板卡完整初始化后进入配网
        SaveNetworkModeToSettings(NetworkMode::WIFI_ONLY);
        mode_ = NetworkMode::WIFI_ONLY;
        if (auto display = GetDisplay()) {
            display->ShowNotification("切换到 WiFi 模式并重启配网", 3000);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        Application::GetInstance().Reboot();
        return true;
    }

    auto* wifi_board = dynamic_cast<WifiBoard*>(current_board_.get());
    if (wifi_board == nullptr) {
        return false;
    }
    wifi_board->EnterWifiConfigMode();
    return true;
}

std::string DualNetworkBoard::GetBoardType() {
    return current_board_->GetBoardType();
}

void DualNetworkBoard::StartNetwork() {
    auto display = GetDisplay();
    ESP_LOGI(TAG, "StartNetwork: mode=%s, saved_ssid=%d", GetNetworkModeName(),
             (int)SsidManager::GetInstance().GetSsidList().size());

    if (mode_ == NetworkMode::AUTO) {
        if (SsidManager::GetInstance().GetSsidList().empty()) {
            // 没有任何 WiFi 记录：保持原厂行为先进配网，避免没有 SIM 卡时设备完全不可用
            ESP_LOGW(TAG, "No WiFi configured, enter config mode (double click BOOT for 4G)");
            if (display != nullptr) {
                display->ShowNotification("未配置 WiFi，请配网；双击 BOOT 可切 4G", 5000);
            }
            current_board_->StartNetwork();
            return;
        }

        if (display != nullptr) {
            display->SetStatus(Lang::Strings::CONNECTING);
        }
        wifi_was_connected_ = false;
        esp_timer_start_once(wifi_fallback_timer_, WIFI_FALLBACK_TIMEOUT_SEC * 1000000ULL);
        current_board_->StartNetwork();
        return;
    }

    if (display != nullptr) {
        if (network_type_ == NetworkType::WIFI) {
            display->SetStatus(Lang::Strings::CONNECTING);
        } else {
            display->SetStatus(Lang::Strings::DETECTING_MODULE);
        }
    }
    current_board_->StartNetwork();
}

void DualNetworkBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
    // 转发层始终保留，确保自动降级后新板卡的事件还能上来
    if (current_board_ != nullptr) {
        current_board_->SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
            ForwardNetworkEvent(event, data);
        });
    }
}

NetworkInterface* DualNetworkBoard::GetNetwork() {
    return current_board_->GetNetwork();
}

const char* DualNetworkBoard::GetNetworkStateIcon() {
    return current_board_->GetNetworkStateIcon();
}

void DualNetworkBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    current_board_->SetPowerSaveLevel(level);
}

std::string DualNetworkBoard::GetBoardJson() {   
    return current_board_->GetBoardJson();
}

std::string DualNetworkBoard::GetDeviceStatusJson() {
    return current_board_->GetDeviceStatusJson();
}
