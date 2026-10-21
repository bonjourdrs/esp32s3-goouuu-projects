#ifndef _GPS_RECEIVER_H_
#define _GPS_RECEIVER_H_

#include <cstdint>
#include <mutex>
#include <string>

#include "esp_err.h"

struct GpsFix {
    bool started = false;   // 驱动是否成功启动
    bool valid = false;     // 是否拿到有效定位
    double latitude = 0.0;
    double longitude = 0.0;
    float altitude_m = 0.0f;
    float speed_kmh = 0.0f;
    float course_deg = 0.0f;
    float hdop = 99.9f;
    int satellites = 0;
    int fix_quality = 0;
    int64_t fix_us = 0;
    std::string city;
    float city_distance_km = -1.0f;
};

// ATGM336H NMEA 接收器：UART2 + GPIO43/44，1 Hz 解析 GGA/RMC。
// 城市名使用内置离线城市表做最近匹配，不依赖网络和第三方 key。
class GpsReceiver {
public:
    static GpsReceiver& GetInstance();

    esp_err_t Start();
    bool IsStarted();
    GpsFix GetFix();

private:
    GpsReceiver() = default;
    GpsReceiver(const GpsReceiver&) = delete;
    GpsReceiver& operator=(const GpsReceiver&) = delete;

    void Task();
    void HandleLine(char* line);

    std::mutex mutex_;
    GpsFix fix_;
    bool started_ = false;
};

#endif  // _GPS_RECEIVER_H_
