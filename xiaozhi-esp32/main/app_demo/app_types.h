#ifndef APP_DEMO_TYPES_H
#define APP_DEMO_TYPES_H

#include <cstdint>
#include <string>

namespace appdemo {

enum class SelectedRoute : uint8_t {
    WIFI = 1,
    CELLULAR_4G = 2,
};

enum class RoutePhase : uint8_t {
    IDLE = 0,
    INITIALIZING,
    CONNECTING,
    CHECKING_INTERNET,
    READY,
    FAILED,
};

enum class WifiState : uint8_t {
    DISABLED = 0,
    INITIALIZING,
    CONNECTING,
    AP_CONNECTED,
    GOT_IP,
    CHECKING_INTERNET,
    ONLINE,
    OFFLINE,
    ERROR,
};

enum class CellularState : uint8_t {
    DISABLED = 0,
    INITIALIZING,
    CHECKING_SIM,
    NO_SIM,
    SIM_READY,
    REGISTERING,
    REGISTERED,
    STARTING_DATA,
    DATA_CONNECTED,
    CHECKING_INTERNET,
    ONLINE,
    OFFLINE,
    ERROR,
};

struct WifiStatus {
    bool hw_ready = false;
    bool initialized = false;

    bool ap_connected = false;
    bool ip_ready = false;
    bool internet_ok = false;

    std::string ssid;
    std::string ip;

    int rssi = 0;

    WifiState state = WifiState::DISABLED;
    std::string last_error;
};

struct CellularStatus {
    bool module_ready = false;
    bool sim_present = false;
    bool registered = false;
    bool data_session_up = false;
    bool internet_ok = false;

    int rssi = 0;
    int csq = -1;

    std::string operator_name;
    std::string ip;

    CellularState state = CellularState::DISABLED;
    std::string last_error;
};

const char* RouteToString(SelectedRoute route);
SelectedRoute RouteFromString(const std::string& value, bool* ok);
const char* PhaseToString(RoutePhase phase);
const char* WifiStateToString(WifiState state);
const char* CellularStateToString(CellularState state);

// 错误码统一使用字符串，避免 App 端和固件端枚举不同步
constexpr const char* kErrNone = "";
constexpr const char* kErrApNotFound = "AP_NOT_FOUND";
constexpr const char* kErrAuthFailed = "AUTH_FAILED";
constexpr const char* kErrDhcpFailed = "DHCP_FAILED";
constexpr const char* kErrInternatUnreachable = "NO_INTERNET";
constexpr const char* kErrModemNotResponding = "MODEM_NOT_RESPONDING";
constexpr const char* kErrSimNotInserted = "SIM_NOT_INSERTED";
constexpr const char* kErrRegistrationFailed = "REGISTRATION_FAILED";
constexpr const char* kErrRegistrationDenied = "REGISTRATION_DENIED";
constexpr const char* kErrDataSessionFailed = "DATA_SESSION_FAILED";
constexpr const char* kErrTimeout = "TIMEOUT";
constexpr const char* kErrInternal = "INTERNAL_ERROR";
constexpr const char* kErrInvalidRoute = "INVALID_ROUTE";
constexpr const char* kErrUnknownCmd = "UNKNOWN_CMD";
constexpr const char* kErrInvalidJson = "INVALID_JSON";

}  // namespace appdemo

#endif  // APP_DEMO_TYPES_H
