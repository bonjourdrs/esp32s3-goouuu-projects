#ifndef APP_NETWORK_MANAGER_H
#define APP_NETWORK_MANAGER_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app_types.h"

class AtModem;
class NetworkInterface;

namespace appdemo {

// Demo 固件的选路/网络状态中心。
//
// 设计约束（见需求第 3~13 节）：
//  - selected_route 只能被 App set_route 命令和 NVS 恢复修改；
//  - 任何网络成功/失败都不会改变 selected_route；
//  - 不调用 DualNetworkBoard::StartNetwork()，因此 AUTO 的 WiFi 超时降级、
//    4G 恢复兜底定时器都不会启动；Wi-Fi 和 4G 都使用底层驱动直接受控；
//  - set_route 先落 NVS，再异步重新初始化目标通路。
class AppNetworkManager {
public:
    static AppNetworkManager& GetInstance();

    void Start();

    SelectedRoute GetSelectedRoute() const;
    // 只修改并持久化 selected_route，不做任何网络操作，供 BLE 命令层在 ACK 前调用
    bool SetSelectedRoute(SelectedRoute route);
    // 触发一次目标通路的真实重新初始化，并立即上报 route.phase=initializing
    void BeginRouteInit();

    WifiStatus GetWifiStatus();
    CellularStatus GetCellularStatus();
    bool IsRouteInitializing() const { return reinit_in_progress_.load(); }

    std::string BuildRouteJson();
    std::string BuildWifiJson();
    std::string BuildCellJson();

    // 产品业务连接必须通过该接口拿网络，4G 时为模组 socket，Wi-Fi 时为 lwIP
    NetworkInterface* GetProductNetwork();

    void SetNotifyCallback(std::function<void(const std::string&)> callback);
    void SetRouteChangeCallback(std::function<void(SelectedRoute)> callback);

private:
    AppNetworkManager() = default;
    AppNetworkManager(const AppNetworkManager&) = delete;
    AppNetworkManager& operator=(const AppNetworkManager&) = delete;

    static void ControlTaskEntry(void* arg);
    static void ProbeTaskEntry(void* arg);
    void ControlLoop();
    void ProbeLoop();

    void LoadRouteFromNvs();
    void SaveRouteToNvs(SelectedRoute route);

    void HandleReinit();
    void ReinitWifi();
    void ReinitCellular();
    void RefreshWifiStatusLocked();
    void RefreshCellularStatusLocked();

    bool CheckSimPresent();
    bool QueryCellularIp(std::string* ip);
    bool ProbeWifi();
    bool ProbeCellular();
    void ApplyCellularProbeResult(bool ok, bool initial);
    void ApplyWifiProbeResult(bool ok, bool initial);

    void NotifyRoute();
    void NotifyWifi();
    void NotifyCell();
    void NotifyJson(const std::string& json);

    std::unique_ptr<AtModem> modem_;
    // 串行化所有模组 AT / socket 操作，避免周期性查询打断 socket 的 URC 等待
    std::mutex modem_mutex_;

    mutable std::mutex status_mutex_;
    SelectedRoute selected_route_ = SelectedRoute::WIFI;
    RoutePhase phase_ = RoutePhase::IDLE;
    std::string route_error_;
    WifiStatus wifi_;
    CellularStatus cell_;

    // 模组网络状态 URC 回调只写原子变量，避免在 URC 任务里抢状态锁造成死锁
    std::atomic<int8_t> cell_urc_registered_{-1};
    std::atomic<bool> cell_notify_pending_{false};

    std::atomic<bool> started_{false};
    std::atomic<bool> reinit_pending_{false};
    std::atomic<bool> reinit_in_progress_{false};
    TaskHandle_t control_task_ = nullptr;
    TaskHandle_t probe_task_ = nullptr;

    std::function<void(const std::string&)> notify_callback_;
    std::function<void(SelectedRoute)> route_change_callback_;
};

}  // namespace appdemo

#endif  // APP_NETWORK_MANAGER_H
