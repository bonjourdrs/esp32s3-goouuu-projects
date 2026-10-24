#include "app_mqtt_manager.h"

#include <cJSON.h>
#include <esp_log.h>
#include <mqtt_client.h>

#include <algorithm>
#include <cstring>

#include "app_mqtt_config.h"
#include "app_device_status.h"
#include "app_network_manager.h"
#include "app_sensor_reporter.h"
#include "settings.h"

// 密码只在 NVS（BLE 下发）或本地 mqtt_secrets.h（开发 fallback）中，不写日志、不入库
#if __has_include("mqtt_secrets.h")
#include "mqtt_secrets.h"
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif

// CA 证书通过 CMake 的 EMBED_TXTFILES 编译进固件，用于 TLS 校验
extern "C" const char ca_pem_start[] asm("_binary_emqxsl_ca_crt_start");
extern "C" const char ca_pem_end[] asm("_binary_emqxsl_ca_crt_end");

static const char* TAG = "AppMqtt";

namespace appdemo {

namespace {
constexpr int kBackoffMs[] = {1000, 2000, 5000, 10000, 30000};
constexpr int kBackoffCount = sizeof(kBackoffMs) / sizeof(kBackoffMs[0]);
constexpr int kLoopIntervalMs = 500;

// NVS key（namespace app_demo）：密码只存这里，绝不打印
constexpr const char* kNvsProvisioned = "provisioned";
constexpr const char* kNvsDeviceId = "cloud_device_id";
constexpr const char* kNvsHost = "cloud_host";
constexpr const char* kNvsPort = "cloud_port";
constexpr const char* kNvsUsername = "cloud_username";
constexpr const char* kNvsPassword = "cloud_password";

bool IsPlaceholderPassword(const char* password) {
    return password == nullptr || password[0] == '\0' ||
           strstr(password, "REPLACE_WITH") != nullptr;
}

// device_id 会进入 Topic 与 JSON，只允许安全字符
bool IsSafeDeviceId(const std::string& id) {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    for (char c : id) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
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
}  // namespace

AppMqttManager& AppMqttManager::GetInstance() {
    static AppMqttManager instance;
    return instance;
}

const char* AppMqttManager::GetCloudStateString() const {
    switch (state_.load()) {
        case State::DISABLED:
            return "disabled";
        case State::WAIT_NETWORK:
            return "waiting_network";
        case State::CONNECTING:
            return "connecting";
        case State::ONLINE:
            return "online";
        case State::BACKOFF:
            return "backoff";
        case State::ERROR:
            return "error";
    }
    return "disabled";
}

const char* AppMqttManager::CloudErrorToString(CloudError error) {
    switch (error) {
        case CloudError::NONE:
            return "";
        case CloudError::AUTH_FAILED:
            return "auth_failed";
        case CloudError::CONNECTION_REFUSED:
            return "connection_refused";
        case CloudError::TLS_FAILED:
            return "tls_failed";
        case CloudError::TRANSPORT_ERROR:
            return "transport_error";
        case CloudError::TIMEOUT:
            return "timeout";
    }
    return "";
}

std::string AppMqttManager::DeviceId() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_.device_id;
}

std::string AppMqttManager::BuildCloudJson() const {
    CloudConfig config;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config = config_;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "cloud");
    cJSON_AddBoolToObject(root, "provisioned", provisioned_.load());
    cJSON_AddStringToObject(root, "state", GetCloudStateString());
    cJSON_AddStringToObject(root, "device_id", config.device_id.c_str());
    cJSON_AddStringToObject(root, "host", config.host.c_str());
    cJSON_AddNumberToObject(root, "port", config.port);
    cJSON_AddStringToObject(root, "username", config.username.c_str());

    CloudError error = static_cast<CloudError>(last_error_.load());
    if (error != CloudError::NONE && state_.load() != State::ONLINE) {
        cJSON_AddStringToObject(root, "error", CloudErrorToString(error));
    }
    // 绝不返回 password
    return JsonToString(root);
}

void AppMqttManager::LoadCloudConfig() {
    Settings settings("app_demo");
    bool provisioned = settings.GetInt(kNvsProvisioned, 0) == 1;

    CloudConfig nvs;
    nvs.device_id = settings.GetString(kNvsDeviceId, "");
    nvs.host = settings.GetString(kNvsHost, "");
    nvs.port = settings.GetInt(kNvsPort, 0);
    nvs.username = settings.GetString(kNvsUsername, "");
    nvs.password = settings.GetString(kNvsPassword, "");

    bool nvs_ready = provisioned && IsSafeDeviceId(nvs.device_id) && !nvs.host.empty() &&
                     nvs.port > 0 && nvs.port <= 65535 && !nvs.username.empty() &&
                     !nvs.password.empty();

    if (nvs_ready) {
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            config_ = nvs;
        }
        provisioned_ = true;
        ESP_LOGI(TAG, "cloud config loaded from NVS, device_id=%s", nvs.device_id.c_str());
    } else if (!IsPlaceholderPassword(MQTT_PASSWORD)) {
        CloudConfig fallback;
        fallback.device_id = mqttcfg::kDeviceId;
        fallback.host = mqttcfg::kHost;
        fallback.port = mqttcfg::kPort;
        fallback.username = mqttcfg::kUsername;
        fallback.password = MQTT_PASSWORD;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            config_ = fallback;
        }
        provisioned_ = true;
        ESP_LOGW(TAG, "no NVS cloud config, using mqtt_secrets.h dev fallback");
    } else {
        CloudConfig empty;
        empty.device_id = mqttcfg::kDeviceId;  // 仅用于展示，无密码不会连接
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            config_ = empty;
        }
        provisioned_ = false;
        ESP_LOGI(TAG, "no cloud credential (NVS / dev fallback), cloud disabled");
    }
}

void AppMqttManager::RebuildDerived(const CloudConfig& config) {
    credentials_ready_ = !config.device_id.empty() && !config.host.empty() && config.port > 0 &&
                         config.port <= 65535 && !config.username.empty() &&
                         !config.password.empty();

    uri_ = "mqtts://" + config.host + ":" + std::to_string(config.port);

    const std::string base = "v1/devices/" + config.device_id;
    topics_.presence = base + mqttcfg::kSuffixPresence;
    topics_.status = base + mqttcfg::kSuffixStatus;
    topics_.env = base + mqttcfg::kSuffixEnv;
    topics_.baro = base + mqttcfg::kSuffixBaro;
    topics_.gps = base + mqttcfg::kSuffixGps;
    topics_.wifi = base + mqttcfg::kSuffixWifi;
    topics_.cell = base + mqttcfg::kSuffixCell;
    topics_.event = base + mqttcfg::kSuffixEvent;
    topics_.ack = base + mqttcfg::kSuffixAck;
    topics_.cmd = base + mqttcfg::kSuffixCmd;

    cJSON* lwt = cJSON_CreateObject();
    cJSON_AddStringToObject(lwt, "type", "presence");
    cJSON_AddStringToObject(lwt, "device_id", config.device_id.c_str());
    cJSON_AddBoolToObject(lwt, "online", false);
    lwt_payload_ = JsonToString(lwt);
}

void AppMqttManager::PersistCloudConfig(const CloudConfig& config) {
    Settings settings("app_demo", true);
    settings.SetString(kNvsDeviceId, config.device_id);
    settings.SetString(kNvsHost, config.host);
    settings.SetInt(kNvsPort, config.port);
    settings.SetString(kNvsUsername, config.username);
    settings.SetString(kNvsPassword, config.password);
    settings.SetInt(kNvsProvisioned, 1);
}

void AppMqttManager::EraseCloudConfig() {
    Settings settings("app_demo", true);
    settings.EraseKey(kNvsDeviceId);
    settings.EraseKey(kNvsHost);
    settings.EraseKey(kNvsPort);
    settings.EraseKey(kNvsUsername);
    settings.EraseKey(kNvsPassword);
    settings.EraseKey(kNvsProvisioned);
}

bool AppMqttManager::ApplyCloudProvision(const CloudConfig& config, const char** error) {
    if (!IsSafeDeviceId(config.device_id) || config.host.empty() || config.username.empty() ||
        config.password.empty() || config.port <= 0 || config.port > 65535 ||
        config.host.size() > 128 || config.username.size() > 64 ||
        config.password.size() > 128) {
        *error = kErrInvalidParam;
        return false;
    }

    PersistCloudConfig(config);
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config_ = config;
    }
    provisioned_ = true;
    last_error_ = static_cast<int>(CloudError::NONE);
    no_credentials_logged_ = false;
    config_gen_.fetch_add(1);
    // 不打印 password
    ESP_LOGI(TAG, "cloud provision accepted: device_id=%s host=%s port=%d",
             config.device_id.c_str(), config.host.c_str(), config.port);
    *error = kErrNone;
    return true;
}

void AppMqttManager::ClearCloudProvision() {
    EraseCloudConfig();
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        // 先覆盖内存中的密码再释放
        if (!config_.password.empty()) {
            memset(&config_.password[0], 0, config_.password.size());
        }
        config_ = CloudConfig{};
    }
    provisioned_ = false;
    last_error_ = static_cast<int>(CloudError::NONE);
    config_gen_.fetch_add(1);
    // 立即把状态置为 disabled，MQTT task 本轮会关闭旧 client
    SetState(State::DISABLED, CloudError::NONE, "cloud_clear");
    ESP_LOGI(TAG, "cloud credentials cleared by BLE");
}

void AppMqttManager::Start() {
    if (started_.exchange(true)) {
        return;
    }
    LoadCloudConfig();
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        RebuildDerived(config_);
    }
    xTaskCreate(TaskEntry, "app_mqtt", 6144, this, 4, &task_);
}

void AppMqttManager::OnRouteChanged(SelectedRoute route) {
    ESP_LOGW(TAG, "route -> %s, close old mqtt socket", RouteToString(route));
    force_reconnect_ = true;
}

void AppMqttManager::OnNetworkJson(const std::string& json) {
    // 只缓存最新一份 wifi / cell，交给 MQTT task 发送，避免阻塞网络状态机
    if (json.find("\"type\":\"wifi\"") != std::string::npos) {
        std::lock_guard<std::mutex> lock(net_mutex_);
        pending_wifi_ = json;
        wifi_dirty_ = true;
    } else if (json.find("\"type\":\"cell\"") != std::string::npos) {
        std::lock_guard<std::mutex> lock(net_mutex_);
        pending_cell_ = json;
        cell_dirty_ = true;
    }
}

void AppMqttManager::TaskEntry(void* arg) {
    static_cast<AppMqttManager*>(arg)->Loop();
    vTaskDelete(nullptr);
}

void AppMqttManager::NotifyCloudState() {
    if (cloud_state_callback_) {
        cloud_state_callback_(BuildCloudJson());
    }
}

void AppMqttManager::SetState(State next, CloudError error, const char* reason) {
    if (next == State::ONLINE) {
        last_error_ = static_cast<int>(CloudError::NONE);
    } else if (error != CloudError::NONE) {
        last_error_ = static_cast<int>(error);
    }

    State old = state_.exchange(next);
    if (old == next) {
        return;
    }
    const char* old_name = "disabled";
    switch (old) {
        case State::DISABLED: old_name = "disabled"; break;
        case State::WAIT_NETWORK: old_name = "waiting_network"; break;
        case State::CONNECTING: old_name = "connecting"; break;
        case State::ONLINE: old_name = "online"; break;
        case State::BACKOFF: old_name = "backoff"; break;
        case State::ERROR: old_name = "error"; break;
    }
    ESP_LOGI(TAG, "cloud state changed %s -> %s%s%s", old_name, GetCloudStateString(),
             reason != nullptr ? " reason=" : "", reason != nullptr ? reason : "");
    if (next == State::WAIT_NETWORK) {
        ESP_LOGI(TAG, "MQTT WAIT_NETWORK");
    } else if (next == State::BACKOFF) {
        ESP_LOGI(TAG, "MQTT BACKOFF");
    } else if (next == State::ERROR) {
        ESP_LOGW(TAG, "MQTT ERROR");
    }
    NotifyCloudState();
}

void AppMqttManager::ApplyPendingConfig() {
    if (client_ != nullptr) {
        StopClient();
    }
    CloudConfig config;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config = config_;
        RebuildDerived(config);
    }
    applied_gen_ = config_gen_.load();
    ESP_LOGI(TAG, "cloud config applied: provisioned=%d ready=%d", provisioned_.load() ? 1 : 0,
             credentials_ready_ ? 1 : 0);
    if (!provisioned_.load() || !credentials_ready_) {
        SetState(State::DISABLED, CloudError::NONE, "config_applied");
    } else {
        SetState(State::WAIT_NETWORK, CloudError::NONE, "config_applied");
    }
}

void AppMqttManager::Loop() {
    TickType_t last_loop = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_loop, pdMS_TO_TICKS(kLoopIntervalMs));
        TickType_t now = xTaskGetTickCount();

        if (config_gen_.load() != applied_gen_) {
            ApplyPendingConfig();
        }

        if (!provisioned_.load() || !credentials_ready_) {
            if (!no_credentials_logged_.exchange(true)) {
                ESP_LOGW(TAG, "cloud credential missing, Cloud stays disabled");
            }
            if (state_.load() != State::DISABLED) {
                SetState(State::DISABLED, CloudError::NONE, "no_credentials");
            }
            continue;
        }
        no_credentials_logged_ = false;

        SelectedRoute route = AppNetworkManager::GetInstance().GetSelectedRoute();
        bool internet = SelectedRouteHasInternet();

        // 4G：当前 esp-mqtt 走 lwIP，ML307R 是 AT socket，第一阶段不伪造支持
        if (route == SelectedRoute::CELLULAR_4G) {
            if (!gps_cell_bl_unsupported_logged_.exchange(true)) {
                ESP_LOGW(TAG, "4G route selected but ML307R AT socket cannot host esp-mqtt/lwIP TLS; "
                              "Cloud stays waiting_network");
            }
            if (client_ != nullptr) {
                StopClient();
            }
            if (state_.load() != State::WAIT_NETWORK) {
                SetState(State::WAIT_NETWORK, CloudError::NONE, "4g_unsupported");
            }
            continue;
        }

        if (force_reconnect_.exchange(false) && client_ != nullptr) {
            StopClient();
            SetState(State::WAIT_NETWORK, CloudError::NONE, "route_changed");
            continue;
        }

        switch (state_.load()) {
            case State::DISABLED:
                if (!internet) {
                    SetState(State::WAIT_NETWORK, CloudError::NONE);
                } else {
                    SetState(State::CONNECTING, CloudError::NONE);
                    if (!StartClient()) {
                        SetState(State::BACKOFF, CloudError::NONE, "start_failed");
                        retry_at_ = now + pdMS_TO_TICKS(kBackoffMs[backoff_index_]);
                        backoff_index_ = std::min(backoff_index_ + 1, kBackoffCount - 1);
                    } else {
                        connect_deadline_ = now + pdMS_TO_TICKS(mqttcfg::kConnectTimeoutMs);
                    }
                }
                break;

            case State::WAIT_NETWORK:
                if (internet) {
                    if (StartClient()) {
                        SetState(State::CONNECTING, CloudError::NONE);
                        connect_deadline_ = now + pdMS_TO_TICKS(mqttcfg::kConnectTimeoutMs);
                    } else {
                        SetState(State::BACKOFF, CloudError::NONE, "start_failed");
                        retry_at_ = now + pdMS_TO_TICKS(kBackoffMs[backoff_index_]);
                        backoff_index_ = std::min(backoff_index_ + 1, kBackoffCount - 1);
                    }
                }
                break;

            case State::CONNECTING:
                if (!internet) {
                    StopClient();
                    SetState(State::WAIT_NETWORK, CloudError::NONE, "internet_lost");
                } else if (stop_requested_.exchange(false)) {
                    StopClient();
                    SetState(State::BACKOFF, CloudError::NONE, "disconnected");
                    retry_at_ = now + pdMS_TO_TICKS(kBackoffMs[backoff_index_]);
                    backoff_index_ = std::min(backoff_index_ + 1, kBackoffCount - 1);
                } else if (error_pending_.exchange(false)) {
                    StopClient();
                    SetState(State::ERROR, static_cast<CloudError>(last_error_.load()), "mqtt_error");
                    retry_at_ = now + pdMS_TO_TICKS(kBackoffMs[backoff_index_]);
                    backoff_index_ = std::min(backoff_index_ + 1, kBackoffCount - 1);
                } else if (now > connect_deadline_) {
                    ESP_LOGW(TAG, "MQTT connect timeout");
                    StopClient();
                    SetState(State::ERROR, CloudError::TIMEOUT, "timeout");
                    retry_at_ = now + pdMS_TO_TICKS(kBackoffMs[backoff_index_]);
                    backoff_index_ = std::min(backoff_index_ + 1, kBackoffCount - 1);
                }
                break;

            case State::ONLINE:
                if (!internet) {
                    StopClient();
                    SetState(State::WAIT_NETWORK, CloudError::NONE, "internet_lost");
                    break;
                }
                if (error_pending_.exchange(false)) {
                    StopClient();
                    SetState(State::ERROR, static_cast<CloudError>(last_error_.load()), "mqtt_error");
                    retry_at_ = now + pdMS_TO_TICKS(kBackoffMs[backoff_index_]);
                    backoff_index_ = std::min(backoff_index_ + 1, kBackoffCount - 1);
                    break;
                }
                if (stop_requested_.exchange(false)) {
                    StopClient();
                    SetState(State::BACKOFF, CloudError::NONE, "disconnected");
                    retry_at_ = now + pdMS_TO_TICKS(kBackoffMs[backoff_index_]);
                    backoff_index_ = std::min(backoff_index_ + 1, kBackoffCount - 1);
                    break;
                }
                PublishPendingNetworkJson();
                if (now - last_env_tick_ >= pdMS_TO_TICKS(mqttcfg::kTelemetryIntervalMs)) {
                    last_env_tick_ = now;
                    PublishEnv();
                    PublishBaro();
                }
                {
                    std::string gps = AppSensorReporter::GetInstance().BuildGpsJson();
                    bool has_fix = gps.find("\"fix\":1") != std::string::npos;
                    int interval =
                        has_fix ? mqttcfg::kGpsIntervalMs : mqttcfg::kGpsNoFixIntervalMs;
                    if (now - last_gps_tick_ >= pdMS_TO_TICKS(interval)) {
                        last_gps_tick_ = now;
                        PublishGps();
                    }
                }
                if (now - last_status_tick_ >= pdMS_TO_TICKS(mqttcfg::kStatusIntervalMs)) {
                    last_status_tick_ = now;
                    PublishStatus();
                }
                break;

            case State::BACKOFF:
                if (client_ != nullptr) {
                    StopClient();
                }
                if (now >= retry_at_) {
                    backoff_index_ = std::min(backoff_index_ + 1, kBackoffCount - 1);
                    SetState(internet ? State::CONNECTING : State::WAIT_NETWORK, CloudError::NONE,
                             "retry");
                    if (internet && StartClient()) {
                        connect_deadline_ = now + pdMS_TO_TICKS(mqttcfg::kConnectTimeoutMs);
                    } else if (internet) {
                        SetState(State::BACKOFF, CloudError::NONE, "start_failed");
                        retry_at_ = now + pdMS_TO_TICKS(kBackoffMs[backoff_index_]);
                    }
                }
                break;

            case State::ERROR:
                // TLS / 鉴权失败：先暴露 error 给 App，再按 backoff 自动重试
                if (client_ != nullptr) {
                    StopClient();
                }
                if (now >= retry_at_) {
                    backoff_index_ = std::min(backoff_index_ + 1, kBackoffCount - 1);
                    SetState(internet ? State::CONNECTING : State::WAIT_NETWORK, CloudError::NONE,
                             "retry");
                    if (internet && StartClient()) {
                        connect_deadline_ = now + pdMS_TO_TICKS(mqttcfg::kConnectTimeoutMs);
                    } else if (internet) {
                        SetState(State::BACKOFF, CloudError::NONE, "start_failed");
                        retry_at_ = now + pdMS_TO_TICKS(kBackoffMs[backoff_index_]);
                    }
                }
                break;
        }
    }
}

bool AppMqttManager::SelectedRouteHasInternet() {
    auto& network = AppNetworkManager::GetInstance();
    if (network.GetSelectedRoute() == SelectedRoute::WIFI) {
        return network.GetWifiStatus().internet_ok;
    }
    return network.GetCellularStatus().internet_ok;
}

bool AppMqttManager::StartClient() {
    if (client_ != nullptr) {
        return true;
    }
    if (!credentials_ready_) {
        return false;
    }
    // 新一轮连接开始，清掉上一轮遗留的错误标记
    error_pending_ = false;
    stop_requested_ = false;

    CloudConfig config;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config = config_;
    }

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = uri_.c_str();
    cfg.credentials.username = config.username.c_str();
    cfg.credentials.client_id = config.device_id.c_str();
    cfg.credentials.authentication.password = config.password.c_str();
    cfg.broker.verification.certificate = ca_pem_start;
    cfg.broker.verification.certificate_len =
        static_cast<size_t>(ca_pem_end - ca_pem_start);

    cfg.session.last_will.topic = topics_.presence.c_str();
    cfg.session.last_will.msg = lwt_payload_.c_str();
    cfg.session.last_will.msg_len = static_cast<int>(lwt_payload_.size());
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;
    cfg.session.disable_clean_session = false;  // clean session = true
    cfg.session.keepalive = 60;

    cfg.network.disable_auto_reconnect = true;  // 由本任务统一做 backoff
    cfg.network.timeout_ms = 10000;
    cfg.task.stack_size = 6144;
    cfg.task.priority = 5;
    // get_status 会连续发 ack + 5 条状态，out buffer 需要留够余量
    cfg.buffer.size = 2048;
    cfg.buffer.out_size = 4096;

    client_ = esp_mqtt_client_init(&cfg);
    if (client_ == nullptr) {
        ESP_LOGW(TAG, "esp_mqtt_client_init failed");
        return false;
    }
    esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, EventHandler, this);
    esp_err_t err = esp_mqtt_client_start(client_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "MQTT CONNECTING");
    return true;
}

void AppMqttManager::StopClient() {
    if (client_ == nullptr) {
        return;
    }
    esp_mqtt_client_stop(client_);
    esp_mqtt_client_destroy(client_);
    client_ = nullptr;
    ESP_LOGI(TAG, "MQTT client stopped");
}

void AppMqttManager::EventHandler(void* handler_args, esp_event_base_t base, int32_t event_id,
                                  void* event_data) {
    auto* self = static_cast<AppMqttManager*>(handler_args);
    self->HandleEvent(static_cast<esp_mqtt_event_t*>(event_data));
}

void AppMqttManager::HandleEvent(esp_mqtt_event_t* event) {
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED: {
            error_pending_ = false;
            SetState(State::ONLINE, CloudError::NONE, "connected");
            backoff_index_ = 0;
            ESP_LOGI(TAG, "MQTT CONNECTED");
            esp_mqtt_client_subscribe(client_, topics_.cmd.c_str(), 1);
            PublishPresence(true);
            PublishStatus();
            PublishNetworkSnapshot();
            last_env_tick_ = 0;
            last_gps_tick_ = 0;
            last_status_tick_ = xTaskGetTickCount();
            break;
        }
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT SUBSCRIBED %.*s", event->topic_len, event->topic);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT DISCONNECTED");
            stop_requested_ = true;
            break;
        case MQTT_EVENT_DATA: {
            std::string topic(event->topic, event->topic_len);
            if (topic == topics_.cmd) {
                std::string payload(event->data, event->data_len);
                HandleCommand(payload, event->retain);
            }
            break;
        }
        case MQTT_EVENT_ERROR: {
            CloudError error = CloudError::TRANSPORT_ERROR;
            esp_mqtt_error_codes_t* codes = event->error_handle;
            if (codes != nullptr) {
                if (codes->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    if (codes->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                        codes->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
                        error = CloudError::AUTH_FAILED;
                    } else {
                        error = CloudError::CONNECTION_REFUSED;
                    }
                } else if (codes->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                    error = codes->esp_tls_cert_verify_flags != 0 ? CloudError::TLS_FAILED
                                                                  : CloudError::TRANSPORT_ERROR;
                }
            }
            last_error_ = static_cast<int>(error);
            ESP_LOGW(TAG, "MQTT ERROR %s", CloudErrorToString(error));
            State st = state_.load();
            // 连接/在线阶段的 TCP、TLS、鉴权错误统一暴露为 cloud_state=error
            if (st == State::CONNECTING || st == State::ONLINE) {
                error_pending_ = true;
            }
            break;
        }
        default:
            break;
    }
}

void AppMqttManager::HandleCommand(const std::string& payload, bool retained) {
    cJSON* root = cJSON_ParseWithLength(payload.c_str(), payload.size());
    if (root == nullptr) {
        ESP_LOGW(TAG, "invalid command json, drop");
        return;
    }
    cJSON* id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON* cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    int id = cJSON_IsNumber(id_item) ? static_cast<int>(id_item->valuedouble) : 0;
    const char* cmd = cJSON_IsString(cmd_item) ? cmd_item->valuestring : "";

    ESP_LOGI(TAG, "MQTT CMD id=%d cmd=%s", id, cmd);

    // retained command 一律拒绝，避免重新上线执行旧命令
    if (retained) {
        PublishAck(id, false, "retained_command_rejected");
        cJSON_Delete(root);
        return;
    }

    // QoS1 duplicate 防护
    for (int i = 0; i < recent_count_; ++i) {
        if (recent_[i].id == id) {
            ESP_LOGW(TAG, "duplicate cmd id=%d, resend ack", id);
            PublishAckRaw(recent_[i].ack);
            cJSON_Delete(root);
            return;
        }
    }

    if (strcmp(cmd, "get_status") == 0) {
        PublishAck(id, true);
        PublishStatus();
        PublishEnv();
        PublishBaro();
        PublishGps();
        PublishNetworkSnapshot();
    } else {
        PublishAck(id, false, "unsupported_command");
    }
    cJSON_Delete(root);
}

void AppMqttManager::PublishAck(int id, bool ok, const char* error) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddNumberToObject(root, "id", id);
    if (ok) {
        cJSON_AddBoolToObject(root, "ok", true);
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", error != nullptr ? error : "unknown");
    }
    char* text = cJSON_PrintUnformatted(root);
    if (text != nullptr) {
        std::string ack(text);
        PublishAckRaw(ack);
        recent_[recent_next_].id = id;
        recent_[recent_next_].ack = ack;
        recent_next_ = (recent_next_ + 1) % mqttcfg::kRecentCmdCacheSize;
        if (recent_count_ < mqttcfg::kRecentCmdCacheSize) {
            ++recent_count_;
        }
        ESP_LOGI(TAG, "MQTT ACK id=%d ok=%d", id, ok ? 1 : 0);
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

void AppMqttManager::PublishAckRaw(const std::string& payload) {
    Publish(topics_.ack.c_str(), payload, 1, false);
}

std::string AppMqttManager::WithSeq(const std::string& json) {
    if (json.size() < 2 || json.back() != '}') {
        return json;
    }
    char suffix[24];
    snprintf(suffix, sizeof(suffix), ",\"seq\":%u}",
             static_cast<unsigned>(++seq_));
    return json.substr(0, json.size() - 1) + suffix;
}

void AppMqttManager::Publish(const char* topic, const std::string& payload, int qos, bool retain) {
    if (client_ == nullptr || state_.load() != State::ONLINE) {
        return;
    }
    int msg_id = esp_mqtt_client_publish(client_, topic, payload.c_str(),
                                         static_cast<int>(payload.size()), qos, retain ? 1 : 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "publish failed topic=%s", topic);
    }
}

void AppMqttManager::PublishPresence(bool online) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "presence");
    cJSON_AddStringToObject(root, "device_id", DeviceId().c_str());
    cJSON_AddBoolToObject(root, "online", online);
    Publish(topics_.presence.c_str(), JsonToString(root), 1, true);
}

void AppMqttManager::PublishStatus() {
    std::string json = AppDeviceStatus::BuildJson(GetCloudStateString());
    Publish(topics_.status.c_str(), WithSeq(json), 1, true);
}

void AppMqttManager::PublishEnv() {
    Publish(topics_.env.c_str(), WithSeq(AppSensorReporter::GetInstance().BuildEnvJson()), 0,
            false);
}

void AppMqttManager::PublishBaro() {
    Publish(topics_.baro.c_str(), WithSeq(AppSensorReporter::GetInstance().BuildBaroJson()), 0,
            false);
}

void AppMqttManager::PublishGps() {
    Publish(topics_.gps.c_str(), WithSeq(AppSensorReporter::GetInstance().BuildGpsJson()), 0,
            false);
}

void AppMqttManager::PublishNetworkSnapshot() {
    {
        std::lock_guard<std::mutex> lock(net_mutex_);
        pending_wifi_ = AppNetworkManager::GetInstance().BuildWifiJson();
        pending_cell_ = AppNetworkManager::GetInstance().BuildCellJson();
        wifi_dirty_ = true;
        cell_dirty_ = true;
    }
    PublishPendingNetworkJson();
}

void AppMqttManager::PublishPendingNetworkJson() {
    if (wifi_dirty_.exchange(false)) {
        std::string json;
        {
            std::lock_guard<std::mutex> lock(net_mutex_);
            json = pending_wifi_;
        }
        if (!json.empty()) {
            Publish(topics_.wifi.c_str(), WithSeq(json), 1, false);
        }
    }
    if (cell_dirty_.exchange(false)) {
        std::string json;
        {
            std::lock_guard<std::mutex> lock(net_mutex_);
            json = pending_cell_;
        }
        if (!json.empty()) {
            Publish(topics_.cell.c_str(), WithSeq(json), 1, false);
        }
    }
}

}  // namespace appdemo
