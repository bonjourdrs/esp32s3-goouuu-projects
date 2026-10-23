#ifndef _SENSOR_HUB_H_
#define _SENSOR_HUB_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

struct EnvironmentReading {
    bool aht_present = false;
    bool bmp_present = false;
    bool temperature_valid = false;
    bool humidity_valid = false;
    bool pressure_valid = false;
    float temperature_c = 0.0f;
    float humidity_pct = 0.0f;
    float pressure_hpa = 0.0f;
    // BMP280 自带温度通道（用于气压补偿与 App baro.chip_temp）
    bool bmp_temperature_valid = false;
    float bmp_temperature_c = 0.0f;
    // 最近 1 小时气压变化量（hPa），必须攒满 1 小时样本才置为有效
    bool pressure_trend_valid = false;
    float pressure_trend_1h_hpa = 0.0f;
    int64_t updated_us = 0;      // 任一传感器最近一次成功读数的时间
    int64_t aht_updated_us = 0;  // AHT20 最近一次成功读数
    int64_t bmp_updated_us = 0;  // BMP280 最近一次成功读数
};

struct MotionReading {
    bool present = false;
    bool valid = false;
    float accel_g[3] = {0.0f, 0.0f, 0.0f};
    float gyro_dps[3] = {0.0f, 0.0f, 0.0f};
    float dynamic_g = 0.0f;      // 去掉重力后的动态加速度幅值
    // 姿态四元数（Mahony 互补滤波，加速度计校正，无磁力计 → yaw 会漂移）
    bool attitude_valid = false;
    float quat_wxyz[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    std::string state = "unknown";        // still / walking / shaking / impact
    std::string orientation = "unknown";  // face_up / face_down / upright / upside_down / landscape_* / tilted
    float moving_duration_s = 0.0f;
    bool impact_recent = false;
    bool face_down = false;
    int64_t updated_us = 0;
};

// 摇一摇力度：轻摇给一点语气，重摇触发抱怨
enum class ShakeStrength {
    kLight,
    kHeavy,
};

struct ShakeStats {
    int total_count = 0;
    int heavy_count = 0;
    bool has_last = false;
    ShakeStrength last_strength = ShakeStrength::kLight;
    int seconds_since_last = -1;  // -1 表示从未摇晃过
};

// 板载 I2C 传感器统一入口：AHT20 温湿度、BMP280 气压、MPU6050 六轴。
// 所有传感器都可能不存在，读写失败只记录日志，不影响语音主链路。
class SensorHub {
public:
    static SensorHub& GetInstance();

    void Start();
    void SetShakeCallback(std::function<void(ShakeStrength)> callback);

    EnvironmentReading GetEnvironment();
    MotionReading GetMotion();
    ShakeStats GetShakeStats();

private:
    SensorHub() = default;
    SensorHub(const SensorHub&) = delete;
    SensorHub& operator=(const SensorHub&) = delete;

    void EnvironmentTask();
    void MotionTask();

    std::mutex env_mutex_;
    std::mutex motion_mutex_;
    EnvironmentReading env_;
    MotionReading motion_;
    std::function<void(ShakeStrength)> shake_callback_;
    bool started_ = false;

    std::atomic<int> shake_count_{0};
    std::atomic<int> heavy_shake_count_{0};
    std::atomic<int64_t> last_shake_us_{0};
    std::atomic<int> last_shake_strength_{0};
};

#endif  // _SENSOR_HUB_H_
