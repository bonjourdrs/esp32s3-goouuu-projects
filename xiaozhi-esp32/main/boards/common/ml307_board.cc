#include "ml307_board.h"

#include "audio_codec.h"
#include "display.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <material_symbols.h>
#include <utility>

static const char *TAG = "Ml307Board";

// Maximum retry count for modem detection
static constexpr int MODEM_DETECT_MAX_RETRIES = 30;
// Maximum retry count for network registration
static constexpr int NETWORK_REG_MAX_RETRIES = 6;
// 模组自行重启（+MATREADY）后给它的恢复窗口。重启 + 重新驻网 + 重建 PDP
// 通常 30–60 s，超过这个时间才认为真的断网
static constexpr int MODEM_RECOVERY_GRACE_SEC = 60;
static constexpr int MODEM_RECOVERY_POLL_MS = 5000;

Ml307Board::Ml307Board(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin, int baud_rate)
    : tx_pin_(tx_pin), rx_pin_(rx_pin), dtr_pin_(dtr_pin), baud_rate_(baud_rate) {
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            static_cast<Ml307Board*>(arg)->OnRecoveryTick();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ml307_recover",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&timer_args, &recovery_timer_);
}

Ml307Board::~Ml307Board() {
    if (recovery_timer_ != nullptr) {
        esp_timer_stop(recovery_timer_);
        esp_timer_delete(recovery_timer_);
        recovery_timer_ = nullptr;
    }
}

std::string Ml307Board::GetBoardType() {
    return "ml307";
}

void Ml307Board::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void Ml307Board::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::ModemDetecting:
            ESP_LOGI(TAG, "Detecting modem...");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "Registering network...");
            break;
        case NetworkEvent::Connected:
            ESP_LOGI(TAG, "Network connected");
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "Network disconnected");
            break;
        case NetworkEvent::ModemErrorNoSim:
            ESP_LOGE(TAG, "No SIM card detected");
            break;
        case NetworkEvent::ModemErrorRegDenied:
            ESP_LOGE(TAG, "Network registration denied");
            break;
        case NetworkEvent::ModemErrorInitFailed:
            ESP_LOGE(TAG, "Modem initialization failed");
            break;
        case NetworkEvent::ModemErrorTimeout:
            ESP_LOGE(TAG, "Operation timeout");
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void Ml307Board::NetworkTask() {
    // Notify modem detection started
    OnNetworkEvent(NetworkEvent::ModemDetecting);

    // Try to detect modem with retry limit
    int detect_retries = 0;
    while (detect_retries < MODEM_DETECT_MAX_RETRIES) {
        modem_ = AtModem::Detect(tx_pin_, rx_pin_, dtr_pin_, baud_rate_);
        if (modem_ != nullptr) {
            break;
        }
        detect_retries++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (modem_ == nullptr) {
        ESP_LOGE(TAG, "Failed to detect modem after %d retries", MODEM_DETECT_MAX_RETRIES);
        OnNetworkEvent(NetworkEvent::ModemErrorInitFailed);
        return;
    }

    ESP_LOGI(TAG, "Modem detected successfully");

    // Set up network state change callback
    // Note: Don't call GetCarrierName() here as it sends AT command and will block ReceiveTask
    modem_->OnNetworkStateChanged([this](bool network_ready) {
        HandleModemStateChanged(network_ready);
    });

    // Notify network registration started
    OnNetworkEvent(NetworkEvent::Connecting);

    // Wait for network ready with retry limit
    int reg_retries = 0;
    while (reg_retries < NETWORK_REG_MAX_RETRIES) {
        auto result = modem_->WaitForNetworkReady();
        if (result == NetworkStatus::Ready) {
            break;
        } else if (result == NetworkStatus::ErrorInsertPin) {
            // 无 SIM 卡不是瞬态问题，重试也不会变好：直接上报终态并结束，
            // 让上层立刻降级，避免反复提示并空等一分钟
            OnNetworkEvent(NetworkEvent::ModemErrorNoSim);
            return;
        } else if (result == NetworkStatus::ErrorRegistrationDenied) {
            OnNetworkEvent(NetworkEvent::ModemErrorRegDenied);
        } else if (result == NetworkStatus::ErrorTimeout) {
            OnNetworkEvent(NetworkEvent::ModemErrorTimeout);
        }
        reg_retries++;
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    if (!modem_->network_ready()) {
        ESP_LOGE(TAG, "Failed to register network after %d retries", NETWORK_REG_MAX_RETRIES);
        // 超时重试耗尽同样属于终态失败，需要通知上层进入统一 recovery，
        // 否则 AUTO 模式会停在“检测模组”且无法自愈
        OnNetworkEvent(NetworkEvent::ModemErrorRegistrationExhausted);
        return;
    }

    // Print the ML307 modem information
    std::string module_revision = modem_->GetModuleRevision();
    std::string imei = modem_->GetImei();
    std::string iccid = modem_->GetIccid();
    ESP_LOGI(TAG, "ML307 Revision: %s", module_revision.c_str());
    // IMEI/ICCID 属于可识别设备与用户的敏感标识，日志只保留末 4 位
    auto mask_tail = [](const std::string& value) {
        if (value.size() <= 4) {
            return std::string("****");
        }
        return std::string("****") + value.substr(value.size() - 4);
    };
    ESP_LOGI(TAG, "ML307 IMEI: %s", mask_tail(imei).c_str());
    ESP_LOGI(TAG, "ML307 ICCID: %s", mask_tail(iccid).c_str());
}

void Ml307Board::HandleModemStateChanged(bool network_ready) {
    if (network_ready) {
        bool was_recovering = recovery_pending_.exchange(false);
        if (was_recovering) {
            if (recovery_timer_ != nullptr) {
                esp_timer_stop(recovery_timer_);
            }
            ESP_LOGI(TAG, "Modem recovered without leaving network");
        }
        OnNetworkEvent(NetworkEvent::Connected);
        return;
    }

    // +MATREADY 表示模组内部重启，PDP 会短暂失效。这里先不向上层报断网，
    // 给它一个恢复窗口，避免界面闪回联网页、AUTO 模式误切 WiFi。
    bool expected = false;
    if (!recovery_pending_.compare_exchange_strong(expected, true)) {
        return;  // 已经在恢复窗口里，忽略重复的断网通知
    }

    recovery_deadline_us_.store(esp_timer_get_time() + MODEM_RECOVERY_GRACE_SEC * 1000000LL);
    ESP_LOGW(TAG, "Modem reset detected, waiting up to %d s for recovery...",
             MODEM_RECOVERY_GRACE_SEC);
    if (recovery_timer_ != nullptr) {
        esp_timer_stop(recovery_timer_);
        esp_timer_start_periodic(recovery_timer_, MODEM_RECOVERY_POLL_MS * 1000ULL);
    }
}

void Ml307Board::OnRecoveryTick() {
    if (!recovery_pending_.load()) {
        if (recovery_timer_ != nullptr) {
            esp_timer_stop(recovery_timer_);
        }
        return;
    }

    // 模组自己重新驻网成功（可能只发生了 MIPCALL，没有回调），直接恢复
    if (modem_ != nullptr && modem_->network_ready()) {
        recovery_pending_.store(false);
        if (recovery_timer_ != nullptr) {
            esp_timer_stop(recovery_timer_);
        }
        ESP_LOGI(TAG, "Modem recovered within grace period");
        OnNetworkEvent(NetworkEvent::Connected);
        return;
    }

    if (esp_timer_get_time() >= recovery_deadline_us_.load()) {
        recovery_pending_.store(false);
        if (recovery_timer_ != nullptr) {
            esp_timer_stop(recovery_timer_);
        }
        ESP_LOGW(TAG, "Modem did not recover within %d s, reporting disconnect",
                 MODEM_RECOVERY_GRACE_SEC);
        OnNetworkEvent(NetworkEvent::Disconnected);
    }
}

void Ml307Board::StartNetwork() {
    // Create network initialization task and return immediately
    xTaskCreate([](void* arg) {
        Ml307Board* board = static_cast<Ml307Board*>(arg);
        board->NetworkTask();
        vTaskDelete(NULL);
    }, "ml307_net", 4096, this, 5, NULL);
}

NetworkInterface* Ml307Board::GetNetwork() {
    return modem_.get();
}

const char* Ml307Board::GetNetworkStateIcon() {
    if (modem_ == nullptr || !modem_->network_ready()) {
        return MATERIAL_SYMBOLS_ANDROID_CELL_4_BAR_OFF;
    }
    int csq = modem_->GetCsq();
    if (csq == -1) {
        return MATERIAL_SYMBOLS_ANDROID_CELL_4_BAR_OFF;
    } else if (csq >= 0 && csq <= 9) {
        return MATERIAL_SYMBOLS_SIGNAL_CELLULAR_ALT_1_BAR;
    } else if (csq >= 10 && csq <= 14) {
        return MATERIAL_SYMBOLS_SIGNAL_CELLULAR_ALT_2_BAR;
    } else if (csq >= 15 && csq <= 19) {
        return MATERIAL_SYMBOLS_SIGNAL_CELLULAR_ALT;
    } else if (csq >= 20 && csq <= 31) {
        return MATERIAL_SYMBOLS_ANDROID_CELL_4_BAR;
    }

    ESP_LOGW(TAG, "Invalid CSQ: %d", csq);
    return MATERIAL_SYMBOLS_ANDROID_CELL_4_BAR_OFF;
}

std::string Ml307Board::GetBoardJson() {
    cJSON* json = cJSON_CreateObject();
    if (json == nullptr) {
        return std::string("{}");
    }
    cJSON_AddStringToObject(json, "type", BOARD_TYPE);
    cJSON_AddStringToObject(json, "name", BOARD_NAME);
    cJSON_AddStringToObject(json, "manufacturer", BOARD_MANUFACTURER);

    if (modem_ != nullptr) {
        cJSON_AddStringToObject(json, "revision", modem_->GetModuleRevision().c_str());
        cJSON_AddStringToObject(json, "carrier", modem_->GetCarrierName().c_str());
        cJSON_AddStringToObject(json, "csq", std::to_string(modem_->GetCsq()).c_str());
        cJSON_AddStringToObject(json, "imei", modem_->GetImei().c_str());
        cJSON_AddStringToObject(json, "iccid", modem_->GetIccid().c_str());
        cJSON* cereg = cJSON_Parse(modem_->GetRegistrationState().ToString().c_str());
        if (cereg != nullptr) {
            cJSON_AddItemToObject(json, "cereg", cereg);
        }
    } else {
        cJSON_AddStringToObject(json, "status", "offline");
    }

    char* str = cJSON_PrintUnformatted(json);
    std::string result = (str != nullptr) ? str : "{}";
    if (str != nullptr) {
        cJSON_free(str);
    }
    cJSON_Delete(json);
    return result;
}

void Ml307Board::SetPowerSaveLevel(PowerSaveLevel level) {
    // TODO: Implement power save level for ML307
    (void)level;
}

std::string Ml307Board::GetDeviceStatusJson() {
    /*
     * 返回设备状态JSON
     * 
     * 返回的JSON结构如下：
     * {
     *     "audio_speaker": {
     *         "volume": 70
     *     },
     *     "screen": {
     *         "brightness": 100,
     *         "theme": "light"
     *     },
     *     "battery": {
     *         "level": 50,
     *         "charging": true
     *     },
     *     "network": {
     *         "type": "cellular",
     *         "carrier": "CHINA MOBILE",
     *         "csq": 10
     *     }
     * }
     */
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) { // For LCD display only
        auto theme = display->GetTheme();
        if (theme != nullptr) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "cellular");
    cJSON_AddStringToObject(network, "carrier", modem_->GetCarrierName().c_str());
    int csq = modem_->GetCsq();
    if (csq == -1) {
        cJSON_AddStringToObject(network, "signal", "unknown");
    } else if (csq >= 0 && csq <= 14) {
        cJSON_AddStringToObject(network, "signal", "very weak");
    } else if (csq >= 15 && csq <= 19) {
        cJSON_AddStringToObject(network, "signal", "weak");
    } else if (csq >= 20 && csq <= 24) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else if (csq >= 25 && csq <= 31) {
        cJSON_AddStringToObject(network, "signal", "strong");
    }
    cJSON_AddItemToObject(root, "network", network);

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
