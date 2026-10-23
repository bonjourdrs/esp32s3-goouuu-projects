#include "app_network_manager.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "at_modem.h"
#include "config.h"
#include "esp_network.h"
#include "settings.h"
#include "ssid_manager.h"
#include "tcp.h"
#include "wifi_manager.h"

static const char* TAG = "AppNetwork";

namespace appdemo {

namespace {
constexpr int kWifiConnectTimeoutMs = 30000;
constexpr int kWifiIpTimeoutMs = 12000;
constexpr int kCellularRegisterTimeoutMs = 60000;
constexpr int kCellularDataTimeoutMs = 20000;
constexpr int kProbeIntervalMs = 15000;
constexpr int kStatusRefreshIntervalMs = 3000;
constexpr int kInternetProbeTimeoutMs = 4000;
constexpr const char* kProbeHost = "223.5.5.5";
constexpr int kProbePort = 80;

int CsqToRssi(int csq) {
    if (csq < 0 || csq == 99) {
        return 0;
    }
    int rssi = -113 + 2 * csq;
    return std::max(-113, std::min(-51, rssi));
}

// 从 AT+MIPCALL? 的响应里取第一个带点的被引号包裹的 IP 字符串
bool ParseQuotedIp(const std::string& response, std::string* ip) {
    size_t pos = 0;
    while ((pos = response.find('"', pos)) != std::string::npos) {
        size_t end = response.find('"', pos + 1);
        if (end == std::string::npos) {
            break;
        }
        std::string candidate = response.substr(pos + 1, end - pos - 1);
        if (candidate.find('.') != std::string::npos &&
            candidate.find_first_of("0123456789") != std::string::npos) {
            *ip = candidate;
            return true;
        }
        pos = end + 1;
    }
    return false;
}

bool ConnectProbeSocket() {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return false;
    }
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kProbePort);
    addr.sin_addr.s_addr = inet_addr(kProbeHost);

    bool ok = false;
    int ret = connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(sock, &write_set);
        struct timeval tv = {};
        tv.tv_sec = kInternetProbeTimeoutMs / 1000;
        tv.tv_usec = (kInternetProbeTimeoutMs % 1000) * 1000;
        int sel = select(sock + 1, nullptr, &write_set, nullptr, &tv);
        if (sel > 0) {
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
            ok = (so_error == 0);
        }
    }
    close(sock);
    return ok;
}
}  // namespace

const char* RouteToString(SelectedRoute route) {
    return route == SelectedRoute::WIFI ? "wifi" : "4g";
}

SelectedRoute RouteFromString(const std::string& value, bool* ok) {
    // 协议冻结：只接受小写 "wifi" / "4g"，不接受任何别名
    if (value == "wifi") {
        if (ok) *ok = true;
        return SelectedRoute::WIFI;
    }
    if (value == "4g") {
        if (ok) *ok = true;
        return SelectedRoute::CELLULAR_4G;
    }
    if (ok) *ok = false;
    return SelectedRoute::WIFI;
}

const char* PhaseToString(RoutePhase phase) {
    switch (phase) {
        case RoutePhase::IDLE: return "idle";
        case RoutePhase::INITIALIZING: return "initializing";
        case RoutePhase::CONNECTING: return "connecting";
        case RoutePhase::CHECKING_INTERNET: return "checking_internet";
        case RoutePhase::READY: return "ready";
        case RoutePhase::FAILED: return "failed";
    }
    return "idle";
}

const char* WifiStateToString(WifiState state) {
    switch (state) {
        case WifiState::DISABLED: return "disabled";
        case WifiState::INITIALIZING: return "initializing";
        case WifiState::CONNECTING: return "connecting";
        case WifiState::AP_CONNECTED: return "ap_connected";
        case WifiState::GOT_IP: return "got_ip";
        case WifiState::CHECKING_INTERNET: return "checking_internet";
        case WifiState::ONLINE: return "online";
        case WifiState::OFFLINE: return "offline";
        case WifiState::ERROR: return "error";
    }
    return "disabled";
}

const char* CellularStateToString(CellularState state) {
    switch (state) {
        case CellularState::DISABLED: return "disabled";
        case CellularState::INITIALIZING: return "initializing";
        case CellularState::CHECKING_SIM: return "checking_sim";
        case CellularState::NO_SIM: return "no_sim";
        case CellularState::SIM_READY: return "sim_ready";
        case CellularState::REGISTERING: return "registering";
        case CellularState::REGISTERED: return "registered";
        case CellularState::STARTING_DATA: return "starting_data";
        case CellularState::DATA_CONNECTED: return "data_connected";
        case CellularState::CHECKING_INTERNET: return "checking_internet";
        case CellularState::ONLINE: return "online";
        case CellularState::OFFLINE: return "offline";
        case CellularState::ERROR: return "error";
    }
    return "disabled";
}

AppNetworkManager& AppNetworkManager::GetInstance() {
    static AppNetworkManager instance;
    return instance;
}

void AppNetworkManager::Start() {
    if (started_.exchange(true)) {
        return;
    }
    LoadRouteFromNvs();
    {
        // 开机即进入 initializing，App 连接后 get_status 能拿到当前 route 状态
        std::lock_guard<std::mutex> lock(status_mutex_);
        phase_ = RoutePhase::INITIALIZING;
    }

    auto& wifi = WifiManager::GetInstance();
    wifi.SetEventCallback([this](WifiEvent event, const std::string& data) {
        if (event == WifiEvent::Connected) {
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                wifi_.ap_connected = true;
                wifi_.ssid = WifiManager::GetInstance().GetSsid();
                wifi_.ip = WifiManager::GetInstance().GetIpAddress();
                wifi_.ip_ready = !wifi_.ip.empty();
                wifi_.rssi = WifiManager::GetInstance().GetRssi();
                if (wifi_.ip_ready && wifi_.state != WifiState::ONLINE &&
                    wifi_.state != WifiState::CHECKING_INTERNET) {
                    wifi_.state = WifiState::GOT_IP;
                }
            }
            ESP_LOGI(TAG, "WiFi connected ssid=%s ip=%s",
                     WifiManager::GetInstance().GetSsid().c_str(),
                     WifiManager::GetInstance().GetIpAddress().c_str());
            NotifyWifi();
        } else if (event == WifiEvent::Disconnected) {
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                wifi_.ap_connected = false;
                wifi_.ip_ready = false;
                wifi_.ip.clear();
                wifi_.internet_ok = false;
                wifi_.rssi = 0;
                if (wifi_.state != WifiState::ERROR) {
                    wifi_.state = WifiState::OFFLINE;
                }
            }
            ESP_LOGW(TAG, "WiFi disconnected");
            NotifyWifi();
        } else if (event == WifiEvent::Connecting) {
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                wifi_.state = WifiState::CONNECTING;
                wifi_.ssid = data.empty() ? wifi_.ssid : data;
            }
            NotifyWifi();
        }
    });

    xTaskCreate(ControlTaskEntry, "app_net_ctrl", 6144, this, 4, &control_task_);
    xTaskCreate(ProbeTaskEntry, "app_net_probe", 6144, this, 3, &probe_task_);

    reinit_pending_ = true;
    if (control_task_ != nullptr) {
        xTaskNotifyGive(control_task_);
    }
}

SelectedRoute AppNetworkManager::GetSelectedRoute() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return selected_route_;
}

void AppNetworkManager::SetNotifyCallback(std::function<void(const std::string&)> callback) {
    notify_callback_ = std::move(callback);
}

void AppNetworkManager::SetRouteChangeCallback(std::function<void(SelectedRoute)> callback) {
    route_change_callback_ = std::move(callback);
}

void AppNetworkManager::LoadRouteFromNvs() {
    Settings settings("app_demo", true);
    int saved = settings.GetInt("route", 1);
    SelectedRoute route = (saved == 2) ? SelectedRoute::CELLULAR_4G : SelectedRoute::WIFI;
    std::lock_guard<std::mutex> lock(status_mutex_);
    selected_route_ = route;
    ESP_LOGI(TAG, "restore selected_route from NVS: %s", RouteToString(route));
}

void AppNetworkManager::SaveRouteToNvs(SelectedRoute route) {
    // Settings 析构时自动提交，这里用作用域保证在 SetSelectedRoute 返回前落盘
    Settings settings("app_demo", true);
    settings.SetInt("route", route == SelectedRoute::CELLULAR_4G ? 2 : 1);
}

bool AppNetworkManager::SetSelectedRoute(SelectedRoute route) {
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        selected_route_ = route;
        route_error_.clear();
    }
    SaveRouteToNvs(route);
    ESP_LOGI(TAG, "selected_route -> %s (saved to NVS)", RouteToString(route));
    return true;
}

void AppNetworkManager::BeginRouteInit() {
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        phase_ = RoutePhase::INITIALIZING;
        route_error_.clear();
    }
    NotifyRoute();
    reinit_pending_ = true;
    if (control_task_ != nullptr) {
        xTaskNotifyGive(control_task_);
    }
}

void AppNetworkManager::ControlTaskEntry(void* arg) {
    static_cast<AppNetworkManager*>(arg)->ControlLoop();
    vTaskDelete(nullptr);
}

void AppNetworkManager::ProbeTaskEntry(void* arg) {
    static_cast<AppNetworkManager*>(arg)->ProbeLoop();
    vTaskDelete(nullptr);
}

void AppNetworkManager::ControlLoop() {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kStatusRefreshIntervalMs));
        if (reinit_pending_.exchange(false)) {
            HandleReinit();
        }
        if (reinit_in_progress_.load()) {
            continue;
        }
        {
            std::lock_guard<std::mutex> modem_lock(modem_mutex_);
            std::lock_guard<std::mutex> lock(status_mutex_);
            RefreshWifiStatusLocked();
            RefreshCellularStatusLocked();
        }
        if (cell_notify_pending_.exchange(false)) {
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                int8_t registered = cell_urc_registered_.load();
                if (registered >= 0) {
                    cell_.registered = (registered != 0);
                    if (registered != 0 && cell_.state == CellularState::REGISTERING) {
                        cell_.state = CellularState::REGISTERED;
                    }
                    if (registered == 0) {
                        cell_.data_session_up = false;
                        cell_.internet_ok = false;
                    }
                }
            }
            NotifyCell();
        }
    }
}

void AppNetworkManager::HandleReinit() {
    reinit_in_progress_ = true;
    SelectedRoute route;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        route = selected_route_;
    }

    ESP_LOGW(TAG, "reinitialize selected route: %s", RouteToString(route));
    if (route_change_callback_) {
        route_change_callback_(route);
    }

    if (route == SelectedRoute::WIFI) {
        ReinitWifi();
    } else {
        ReinitCellular();
    }
    reinit_in_progress_ = false;
}

void AppNetworkManager::ReinitWifi() {
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        wifi_.state = WifiState::INITIALIZING;
        wifi_.last_error.clear();
        wifi_.ap_connected = false;
        wifi_.ip_ready = false;
        wifi_.internet_ok = false;
        wifi_.ip.clear();
        wifi_.rssi = 0;
        phase_ = RoutePhase::CONNECTING;
    }
    NotifyWifi();
    NotifyRoute();

    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized()) {
        WifiManagerConfig config;
        config.ssid_prefix = "ESP32S3-AI";
        wifi.Initialize(config);
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        wifi_.hw_ready = wifi.IsInitialized();
        wifi_.initialized = wifi.IsInitialized();
    }

    if (SsidManager::GetInstance().GetSsidList().empty()) {
        ESP_LOGW(TAG, "no WiFi credentials saved, report AP_NOT_FOUND");
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            wifi_.state = WifiState::ERROR;
            wifi_.last_error = kErrApNotFound;
            phase_ = RoutePhase::FAILED;
            route_error_ = kErrApNotFound;
        }
        NotifyWifi();
        NotifyRoute();
        return;
    }

    if (wifi.IsConfigMode()) {
        wifi.StopConfigAp();
    }

    // 真实重连：先断开旧连接，再重新发起 STA 连接
    wifi.StopStation();
    vTaskDelay(pdMS_TO_TICKS(300));
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        wifi_.state = WifiState::CONNECTING;
    }
    NotifyWifi();
    wifi.StartStation();

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kWifiConnectTimeoutMs);
    bool ap_connected = false;
    while (xTaskGetTickCount() < deadline) {
        if (wifi.IsConnected()) {
            ap_connected = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!ap_connected) {
        ESP_LOGW(TAG, "WiFi connect timeout");
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            wifi_.state = WifiState::ERROR;
            wifi_.ap_connected = false;
            wifi_.ip_ready = false;
            wifi_.internet_ok = false;
            wifi_.last_error = kErrApNotFound;
            phase_ = RoutePhase::FAILED;
            route_error_ = kErrApNotFound;
        }
        NotifyWifi();
        NotifyRoute();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        wifi_.ap_connected = true;
        wifi_.ssid = wifi.GetSsid();
        wifi_.rssi = wifi.GetRssi();
        wifi_.state = WifiState::AP_CONNECTED;
    }
    NotifyWifi();

    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kWifiIpTimeoutMs);
    std::string ip;
    while (xTaskGetTickCount() < deadline) {
        ip = wifi.GetIpAddress();
        if (!ip.empty()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ip.empty()) {
        ESP_LOGW(TAG, "WiFi got no IP");
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            wifi_.state = WifiState::ERROR;
            wifi_.ip_ready = false;
            wifi_.last_error = kErrDhcpFailed;
            phase_ = RoutePhase::FAILED;
            route_error_ = kErrDhcpFailed;
        }
        NotifyWifi();
        NotifyRoute();
        return;
    }

    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif != nullptr) {
        esp_netif_set_default_netif(sta_netif);
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        wifi_.ssid = wifi.GetSsid();
        wifi_.ip = ip;
        wifi_.ip_ready = true;
        wifi_.rssi = wifi.GetRssi();
        wifi_.state = WifiState::CHECKING_INTERNET;
        phase_ = RoutePhase::CHECKING_INTERNET;
    }
    NotifyWifi();
    NotifyRoute();

    bool online = ProbeWifi();
    ApplyWifiProbeResult(online, true);
}

void AppNetworkManager::ReinitCellular() {
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        cell_.state = CellularState::INITIALIZING;
        cell_.last_error.clear();
        cell_.registered = false;
        cell_.data_session_up = false;
        cell_.internet_ok = false;
        cell_.ip.clear();
        phase_ = RoutePhase::CONNECTING;
    }
    NotifyCell();
    NotifyRoute();

    std::lock_guard<std::mutex> modem_lock(modem_mutex_);

    if (!modem_) {
        ESP_LOGI(TAG, "detect ML307 modem...");
        modem_ = AtModem::Detect(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, ML307_BAUD_RATE);
        if (!modem_) {
            ESP_LOGE(TAG, "ML307 not responding");
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                cell_.state = CellularState::ERROR;
                cell_.module_ready = false;
                cell_.last_error = kErrModemNotResponding;
                phase_ = RoutePhase::FAILED;
                route_error_ = kErrModemNotResponding;
            }
            NotifyCell();
            NotifyRoute();
            return;
        }
        modem_->OnNetworkStateChanged([this](bool ready) {
            cell_urc_registered_ = ready ? 1 : 0;
            cell_notify_pending_ = true;
        });
    } else {
        auto uart = modem_->GetAtUart();
        if (!uart->SendCommand("AT", 2000)) {
            ESP_LOGW(TAG, "modem AT not responding, reboot once");
            modem_->Reboot();
            vTaskDelay(pdMS_TO_TICKS(15000));
            modem_ = AtModem::Detect(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, ML307_BAUD_RATE);
            if (!modem_) {
                {
                    std::lock_guard<std::mutex> lock(status_mutex_);
                    cell_.state = CellularState::ERROR;
                    cell_.module_ready = false;
                    cell_.last_error = kErrModemNotResponding;
                    phase_ = RoutePhase::FAILED;
                    route_error_ = kErrModemNotResponding;
                }
                ESP_LOGE(TAG, "ML307 still not responding");
                NotifyCell();
                NotifyRoute();
                return;
            }
            modem_->OnNetworkStateChanged([this](bool ready) {
                cell_urc_registered_ = ready ? 1 : 0;
                cell_notify_pending_ = true;
            });
        } else {
            // 软重初始化：关闭旧 PDP，再让模组重新驻网
            uart->SendCommand("AT+MIPCALL=0", 3000);
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                cell_.data_session_up = false;
                cell_.ip.clear();
                cell_.registered = false;
            }
            modem_->SetFlightMode(true);
            vTaskDelay(pdMS_TO_TICKS(1500));
            modem_->SetFlightMode(false);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        cell_.module_ready = true;
        cell_.state = CellularState::CHECKING_SIM;
    }
    NotifyCell();

    bool sim_present = CheckSimPresent();
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        cell_.sim_present = sim_present;
        if (!sim_present) {
            cell_.state = CellularState::NO_SIM;
            cell_.last_error = kErrSimNotInserted;
            phase_ = RoutePhase::FAILED;
            route_error_ = kErrSimNotInserted;
        } else {
            cell_.state = CellularState::SIM_READY;
        }
    }
    NotifyCell();
    if (!sim_present) {
        NotifyRoute();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        cell_.state = CellularState::REGISTERING;
        phase_ = RoutePhase::CONNECTING;
    }
    NotifyCell();
    NotifyRoute();

    NetworkStatus status = modem_->WaitForNetworkReady(kCellularRegisterTimeoutMs);
    if (status == NetworkStatus::ErrorInsertPin) {
        ESP_LOGW(TAG, "SIM removed during registration");
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            cell_.sim_present = false;
            cell_.registered = false;
            cell_.state = CellularState::NO_SIM;
            cell_.last_error = kErrSimNotInserted;
            phase_ = RoutePhase::FAILED;
            route_error_ = kErrSimNotInserted;
        }
        NotifyCell();
        NotifyRoute();
        return;
    }
    if (status != NetworkStatus::Ready) {
        CeregState cereg = modem_->GetRegistrationState();
        bool registered = (cereg.stat == 1 || cereg.stat == 5);
        const char* err = (status == NetworkStatus::ErrorRegistrationDenied) ? kErrRegistrationDenied
                          : (status == NetworkStatus::ErrorTimeout)           ? kErrTimeout
                                                                              : kErrRegistrationFailed;
        ESP_LOGW(TAG, "cellular registration failed: %d", static_cast<int>(status));
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            cell_.registered = registered;
            cell_.state = CellularState::ERROR;
            cell_.last_error = err;
            phase_ = RoutePhase::FAILED;
            route_error_ = err;
        }
        NotifyCell();
        NotifyRoute();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        cell_.registered = true;
        cell_.state = CellularState::REGISTERED;
    }
    NotifyCell();

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        cell_.state = CellularState::STARTING_DATA;
    }
    NotifyCell();

    // WaitForNetworkReady 内部的 MIPCALL 等待很短，这里再显式等 PDP 和 IP
    std::string ip;
    bool data_up = false;
    TickType_t data_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kCellularDataTimeoutMs);
    while (xTaskGetTickCount() < data_deadline) {
        if (QueryCellularIp(&ip)) {
            data_up = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    int csq = modem_->GetCsq();
    std::string carrier = modem_->GetCarrierName();
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        cell_.module_ready = true;
        cell_.sim_present = true;
        cell_.registered = true;
        cell_.csq = csq;
        cell_.rssi = CsqToRssi(csq);
        cell_.operator_name = carrier;
        cell_.ip = ip;
        cell_.data_session_up = data_up;
        if (data_up) {
            cell_.state = CellularState::DATA_CONNECTED;
        } else {
            cell_.state = CellularState::OFFLINE;
            cell_.last_error = kErrDataSessionFailed;
            phase_ = RoutePhase::FAILED;
            route_error_ = kErrDataSessionFailed;
        }
    }
    NotifyCell();
    if (!data_up) {
        NotifyRoute();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        cell_.state = CellularState::CHECKING_INTERNET;
        phase_ = RoutePhase::CHECKING_INTERNET;
    }
    NotifyCell();
    NotifyRoute();

    bool online = ProbeCellular();
    ApplyCellularProbeResult(online, true);
}

bool AppNetworkManager::CheckSimPresent() {
    if (!modem_) {
        return false;
    }
    auto uart = modem_->GetAtUart();
    if (!uart->SendCommand("AT+CPIN?", 3000)) {
        return false;
    }
    std::string response = uart->GetResponse();
    if (response.find("READY") != std::string::npos) {
        return true;
    }
    return modem_->pin_ready();
}

bool AppNetworkManager::QueryCellularIp(std::string* ip) {
    if (!modem_) {
        return false;
    }
    auto uart = modem_->GetAtUart();
    if (!uart->SendCommand("AT+MIPCALL?", 3000)) {
        return false;
    }
    std::string response = uart->GetResponse();
    if (response.find("MIPCALL") == std::string::npos) {
        return false;
    }
    std::string parsed;
    if (!ParseQuotedIp(response, &parsed)) {
        return false;
    }
    *ip = parsed;
    return true;
}

bool AppNetworkManager::ProbeWifi() {
    if (!WifiManager::GetInstance().IsConnected()) {
        return false;
    }
    if (WifiManager::GetInstance().GetIpAddress().empty()) {
        return false;
    }
    return ConnectProbeSocket();
}

bool AppNetworkManager::ProbeCellular() {
    if (!modem_) {
        return false;
    }
    std::unique_ptr<Tcp> tcp = modem_->CreateTcp(0);
    if (!tcp) {
        return false;
    }
    bool ok = tcp->Connect(kProbeHost, kProbePort);
    tcp->Disconnect();
    return ok;
}

void AppNetworkManager::ApplyWifiProbeResult(bool ok, bool initial) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        changed = (wifi_.internet_ok != ok) || initial;
        wifi_.internet_ok = ok;
        wifi_.state = ok ? WifiState::ONLINE : WifiState::OFFLINE;
        wifi_.last_error = ok ? "" : kErrInternatUnreachable;
        if (initial) {
            phase_ = RoutePhase::READY;
            route_error_.clear();
        }
    }
    if (changed) {
        NotifyWifi();
    }
    if (initial) {
        NotifyRoute();
    }
}

void AppNetworkManager::ApplyCellularProbeResult(bool ok, bool initial) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        changed = (cell_.internet_ok != ok) || initial;
        cell_.internet_ok = ok;
        cell_.state = ok ? CellularState::ONLINE : CellularState::OFFLINE;
        cell_.last_error = ok ? "" : kErrInternatUnreachable;
        if (initial) {
            phase_ = RoutePhase::READY;
            route_error_.clear();
        }
    }
    if (changed) {
        NotifyCell();
    }
    if (initial) {
        NotifyRoute();
    }
}

void AppNetworkManager::ProbeLoop() {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kProbeIntervalMs));
        if (reinit_in_progress_.load()) {
            continue;
        }

        bool wifi_ready = false;
        bool cell_ready = false;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            wifi_ready = wifi_.ip_ready;
            cell_ready = cell_.data_session_up;
        }

        if (wifi_ready) {
            bool ok = ProbeWifi();
            ApplyWifiProbeResult(ok, false);
        }
        if (cell_ready) {
            std::lock_guard<std::mutex> modem_lock(modem_mutex_);
            bool ok = ProbeCellular();
            ApplyCellularProbeResult(ok, false);
        }
    }
}

void AppNetworkManager::RefreshWifiStatusLocked() {
    auto& wifi = WifiManager::GetInstance();
    wifi_.initialized = wifi.IsInitialized();
    wifi_.hw_ready = wifi.IsInitialized();
    wifi_.ap_connected = wifi.IsConnected();
    wifi_.ip = wifi.GetIpAddress();
    wifi_.ip_ready = !wifi_.ip.empty();
    wifi_.ssid = wifi.GetSsid();
    wifi_.rssi = wifi_.ap_connected ? wifi.GetRssi() : 0;

    if (!wifi_.initialized) {
        wifi_.state = WifiState::DISABLED;
    } else if (!wifi_.ap_connected && wifi_.state != WifiState::ERROR &&
               wifi_.state != WifiState::CONNECTING && wifi_.state != WifiState::INITIALIZING) {
        wifi_.state = WifiState::OFFLINE;
        wifi_.internet_ok = false;
    }
}

void AppNetworkManager::RefreshCellularStatusLocked() {
    if (!modem_) {
        return;
    }
    cell_.module_ready = true;
    cell_.sim_present = modem_->pin_ready();
    CeregState cereg = modem_->GetRegistrationState();
    cell_.registered = (cereg.stat == 1 || cereg.stat == 5);
    int csq = modem_->GetCsq();
    if (csq >= 0 && csq != 99) {
        cell_.csq = csq;
        cell_.rssi = CsqToRssi(csq);
    }
    if (cell_.operator_name.empty()) {
        cell_.operator_name = modem_->GetCarrierName();
    }
    std::string ip;
    if (QueryCellularIp(&ip)) {
        cell_.ip = ip;
        cell_.data_session_up = true;
    } else if (!cell_.registered) {
        cell_.data_session_up = false;
        cell_.ip.clear();
    }
    if (!cell_.registered) {
        cell_.data_session_up = false;
        cell_.internet_ok = false;
    }
}

std::string AppNetworkManager::BuildRouteJson() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "route");
    SelectedRoute route;
    RoutePhase phase;
    std::string err;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        route = selected_route_;
        phase = phase_;
        err = route_error_;
    }
    cJSON_AddStringToObject(root, "selected", RouteToString(route));
    cJSON_AddStringToObject(root, "phase", PhaseToString(phase));
    if (!err.empty()) {
        cJSON_AddStringToObject(root, "err", err.c_str());
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string result = text != nullptr ? text : "{}";
    if (text != nullptr) {
        cJSON_free(text);
    }
    cJSON_Delete(root);
    return result;
}

std::string AppNetworkManager::BuildWifiJson() {
    WifiStatus status;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status = wifi_;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "wifi");
    cJSON_AddStringToObject(root, "state", WifiStateToString(status.state));
    cJSON_AddNumberToObject(root, "ap", status.ap_connected ? 1 : 0);
    cJSON_AddNumberToObject(root, "ip_ready", status.ip_ready ? 1 : 0);
    cJSON_AddNumberToObject(root, "internet", status.internet_ok ? 1 : 0);
    if (!status.ssid.empty()) {
        cJSON_AddStringToObject(root, "ssid", status.ssid.c_str());
    }
    if (!status.ip.empty()) {
        cJSON_AddStringToObject(root, "ip", status.ip.c_str());
    }
    cJSON_AddNumberToObject(root, "rssi", status.rssi);
    if (!status.last_error.empty()) {
        cJSON_AddStringToObject(root, "err", status.last_error.c_str());
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string result = text != nullptr ? text : "{}";
    if (text != nullptr) {
        cJSON_free(text);
    }
    cJSON_Delete(root);
    return result;
}

std::string AppNetworkManager::BuildCellJson() {
    CellularStatus status;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status = cell_;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "cell");
    cJSON_AddStringToObject(root, "state", CellularStateToString(status.state));
    cJSON_AddNumberToObject(root, "module", status.module_ready ? 1 : 0);
    cJSON_AddNumberToObject(root, "sim", status.sim_present ? 1 : 0);
    cJSON_AddNumberToObject(root, "registered", status.registered ? 1 : 0);
    cJSON_AddNumberToObject(root, "data", status.data_session_up ? 1 : 0);
    cJSON_AddNumberToObject(root, "internet", status.internet_ok ? 1 : 0);
    cJSON_AddNumberToObject(root, "rssi", status.rssi);
    cJSON_AddNumberToObject(root, "csq", status.csq);
    if (!status.operator_name.empty()) {
        cJSON_AddStringToObject(root, "operator", status.operator_name.c_str());
    }
    if (!status.ip.empty()) {
        cJSON_AddStringToObject(root, "ip", status.ip.c_str());
    }
    if (!status.last_error.empty()) {
        cJSON_AddStringToObject(root, "err", status.last_error.c_str());
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string result = text != nullptr ? text : "{}";
    if (text != nullptr) {
        cJSON_free(text);
    }
    cJSON_Delete(root);
    return result;
}

WifiStatus AppNetworkManager::GetWifiStatus() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return wifi_;
}

CellularStatus AppNetworkManager::GetCellularStatus() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return cell_;
}

NetworkInterface* AppNetworkManager::GetProductNetwork() {
    SelectedRoute route;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        route = selected_route_;
    }
    if (route == SelectedRoute::WIFI) {
        static EspNetwork wifi_network;
        return &wifi_network;
    }
    return modem_.get();
}

void AppNetworkManager::NotifyRoute() {
    NotifyJson(BuildRouteJson());
}

void AppNetworkManager::NotifyWifi() {
    NotifyJson(BuildWifiJson());
}

void AppNetworkManager::NotifyCell() {
    NotifyJson(BuildCellJson());
}

void AppNetworkManager::NotifyJson(const std::string& json) {
    ESP_LOGI(TAG, "notify %s", json.c_str());
    if (notify_callback_) {
        notify_callback_(json);
    }
}

}  // namespace appdemo
