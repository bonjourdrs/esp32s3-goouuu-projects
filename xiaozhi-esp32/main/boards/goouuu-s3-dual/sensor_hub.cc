#include "sensor_hub.h"

#include "config.h"

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cstring>

static const char* TAG = "SensorHub";

namespace {

constexpr uint8_t kAht20Addr = 0x38;
constexpr uint8_t kBmp280AddrPrimary = 0x76;
constexpr uint8_t kBmp280AddrSecondary = 0x77;
constexpr uint8_t kMpu6050AddrPrimary = 0x68;
constexpr uint8_t kMpu6050AddrSecondary = 0x69;

constexpr int kEnvPeriodMs = 5000;
constexpr int kMotionPeriodMs = 20;  // 50 Hz

// 气压历史：5 秒一采样，720 个点覆盖 1 小时
constexpr int kPressureHistorySize = 720;
// 字段语义是“过去一小时气压变化”，必须攒满一整小时样本才置为有效，
// 否则开机 10 分钟就把 10 分钟的变化当成 1 小时趋势报给用户
constexpr int kPressureTrendMinSamples = kPressureHistorySize;
float s_pressure_history[kPressureHistorySize];
int s_pressure_head = 0;
int s_pressure_count = 0;

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_aht20 = nullptr;
i2c_master_dev_handle_t s_bmp280 = nullptr;
i2c_master_dev_handle_t s_mpu6050 = nullptr;

// BMP280 出厂标定系数
uint16_t dig_T1;
int16_t dig_T2, dig_T3;
uint16_t dig_P1;
int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
int32_t s_t_fine = 0;

esp_err_t ReadReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* buffer, size_t length) {
    return i2c_master_transmit_receive(dev, &reg, 1, buffer, length, 100);
}

esp_err_t WriteReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
    uint8_t buffer[2] = {reg, value};
    return i2c_master_transmit(dev, buffer, sizeof(buffer), 100);
}

esp_err_t WriteBytes(i2c_master_dev_handle_t dev, const uint8_t* data, size_t length) {
    return i2c_master_transmit(dev, data, length, 100);
}

bool AddDevice(uint8_t address, i2c_master_dev_handle_t* out) {
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = SENSOR_I2C_FREQ,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, out) == ESP_OK;
}

bool InitI2cBus() {
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = SENSOR_I2C_SDA_PIN,
        .scl_io_num = SENSOR_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        s_bus = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "I2C ready: SDA=GPIO%d SCL=GPIO%d %d Hz", SENSOR_I2C_SDA_PIN,
             SENSOR_I2C_SCL_PIN, SENSOR_I2C_FREQ);
    return true;
}

bool InitAht20() {
    if (!AddDevice(kAht20Addr, &s_aht20)) {
        ESP_LOGW(TAG, "AHT20 not found at 0x%02x", kAht20Addr);
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(40));
    const uint8_t reset[] = {0xBA};
    WriteBytes(s_aht20, reset, sizeof(reset));
    vTaskDelay(pdMS_TO_TICKS(20));

    const uint8_t calibrate[] = {0xBE, 0x08, 0x00};
    if (WriteBytes(s_aht20, calibrate, sizeof(calibrate)) != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 does not answer at 0x%02x", kAht20Addr);
        i2c_master_bus_rm_device(s_aht20);
        s_aht20 = nullptr;
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t status = 0;
    if (ReadReg(s_aht20, 0x71, &status, 1) == ESP_OK) {
        ESP_LOGI(TAG, "AHT20 ready, status=0x%02x calibrated=%d", status,
                 (status & 0x08) ? 1 : 0);
    }
    return true;
}

bool ReadAht20(float* temperature_c, float* humidity_pct) {
    if (s_aht20 == nullptr) {
        return false;
    }
    const uint8_t trigger[] = {0xAC, 0x33, 0x00};
    if (WriteBytes(s_aht20, trigger, sizeof(trigger)) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(90));

    uint8_t raw[7] = {0};
    if (ReadReg(s_aht20, 0x00, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    if ((raw[0] & 0x80) != 0) {
        return false;
    }

    uint32_t raw_hum = ((uint32_t)raw[1] << 12) | ((uint32_t)raw[2] << 4) | ((uint32_t)raw[3] >> 4);
    uint32_t raw_temp = ((uint32_t)(raw[3] & 0x0F) << 16) | ((uint32_t)raw[4] << 8) | raw[5];

    *humidity_pct = (float)raw_hum * 100.0f / 1048576.0f;
    *temperature_c = (float)raw_temp * 200.0f / 1048576.0f - 50.0f;
    return true;
}

void LoadBmp280Calibration(const uint8_t* c) {
    dig_T1 = (uint16_t)((c[1] << 8) | c[0]);
    dig_T2 = (int16_t)((c[3] << 8) | c[2]);
    dig_T3 = (int16_t)((c[5] << 8) | c[4]);
    dig_P1 = (uint16_t)((c[7] << 8) | c[6]);
    dig_P2 = (int16_t)((c[9] << 8) | c[8]);
    dig_P3 = (int16_t)((c[11] << 8) | c[10]);
    dig_P4 = (int16_t)((c[13] << 8) | c[12]);
    dig_P5 = (int16_t)((c[15] << 8) | c[14]);
    dig_P6 = (int16_t)((c[17] << 8) | c[16]);
    dig_P7 = (int16_t)((c[19] << 8) | c[18]);
    dig_P8 = (int16_t)((c[21] << 8) | c[20]);
    dig_P9 = (int16_t)((c[23] << 8) | c[22]);
}

bool InitBmp280() {
    uint8_t address = 0;
    if (i2c_master_probe(s_bus, kBmp280AddrPrimary, 50) == ESP_OK) {
        address = kBmp280AddrPrimary;
    } else if (i2c_master_probe(s_bus, kBmp280AddrSecondary, 50) == ESP_OK) {
        address = kBmp280AddrSecondary;
    } else {
        ESP_LOGW(TAG, "BMP280 not found at 0x76/0x77");
        return false;
    }

    if (!AddDevice(address, &s_bmp280)) {
        return false;
    }

    uint8_t chip_id = 0;
    if (ReadReg(s_bmp280, 0xD0, &chip_id, 1) != ESP_OK || chip_id != 0x58) {
        ESP_LOGW(TAG, "0x%02x is not BMP280 (chip id 0x%02x)", address, chip_id);
        i2c_master_bus_rm_device(s_bmp280);
        s_bmp280 = nullptr;
        return false;
    }

    WriteReg(s_bmp280, 0xE0, 0xB6);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t calibration[26] = {0};
    if (ReadReg(s_bmp280, 0x88, calibration, sizeof(calibration)) != ESP_OK) {
        ESP_LOGW(TAG, "BMP280 calibration read failed");
        i2c_master_bus_rm_device(s_bmp280);
        s_bmp280 = nullptr;
        return false;
    }
    LoadBmp280Calibration(calibration);

    WriteReg(s_bmp280, 0xF5, 0x08);  // IIR x4
    WriteReg(s_bmp280, 0xF4, 0x55);  // osrs_t x2, osrs_p x16, forced mode

    ESP_LOGI(TAG, "BMP280 ready at 0x%02x", address);
    return true;
}

float CompensateTemperature(int32_t adc_t) {
    int32_t var1 = ((((adc_t >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    int32_t var2 = (((((adc_t >> 4) - ((int32_t)dig_T1)) * ((adc_t >> 4) - ((int32_t)dig_T1))) >> 12) *
                    ((int32_t)dig_T3)) >> 14;
    s_t_fine = var1 + var2;
    return (float)((s_t_fine * 5 + 128) >> 8) / 100.0f;
}

float CompensatePressure(int32_t adc_p) {
    int64_t var1 = ((int64_t)s_t_fine) - 128000;
    int64_t var2 = var1 * var1 * (int64_t)dig_P6;
    var2 = var2 + ((var1 * (int64_t)dig_P5) << 17);
    var2 = var2 + (((int64_t)dig_P4) << 35);

    var1 = ((var1 * var1 * (int64_t)dig_P3) >> 8) + ((var1 * (int64_t)dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)dig_P1) >> 33;
    if (var1 == 0) {
        return 0.0f;
    }

    int64_t p = 1048576 - adc_p;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)dig_P7) << 4);
    return (float)p / 256.0f / 100.0f;
}

bool ReadBmp280(float* temperature_c, float* pressure_hpa) {
    if (s_bmp280 == nullptr) {
        return false;
    }
    if (WriteReg(s_bmp280, 0xF4, 0x55) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t raw[6] = {0};
    if (ReadReg(s_bmp280, 0xF7, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }

    int32_t adc_p = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | ((int32_t)raw[2] >> 4);
    int32_t adc_t = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) | ((int32_t)raw[5] >> 4);

    *temperature_c = CompensateTemperature(adc_t);
    *pressure_hpa = CompensatePressure(adc_p);
    return true;
}

bool InitMpu6050() {
    uint8_t address = 0;
    for (uint8_t candidate = kMpu6050AddrPrimary; candidate <= kMpu6050AddrSecondary; ++candidate) {
        if (i2c_master_probe(s_bus, candidate, 50) == ESP_OK) {
            address = candidate;
            break;
        }
    }
    if (address == 0) {
        ESP_LOGW(TAG, "MPU6050 not found at 0x68/0x69");
        return false;
    }

    if (!AddDevice(address, &s_mpu6050)) {
        return false;
    }

    uint8_t who_am_i = 0;
    if (ReadReg(s_mpu6050, 0x75, &who_am_i, 1) != ESP_OK || (who_am_i != 0x68 && who_am_i != 0x70)) {
        ESP_LOGW(TAG, "Unexpected IMU at 0x%02x, WHO_AM_I=0x%02x", address, who_am_i);
        i2c_master_bus_rm_device(s_mpu6050);
        s_mpu6050 = nullptr;
        return false;
    }

    WriteReg(s_mpu6050, 0x6B, 0x80);  // device reset
    vTaskDelay(pdMS_TO_TICKS(100));

    WriteReg(s_mpu6050, 0x6B, 0x01);  // wake, PLL from gyro X
    WriteReg(s_mpu6050, 0x6C, 0x00);  // enable all axes
    WriteReg(s_mpu6050, 0x1A, 0x03);  // DLPF, accel bandwidth 44 Hz
    WriteReg(s_mpu6050, 0x19, 0x09);  // 100 Hz sample rate
    WriteReg(s_mpu6050, 0x1B, 0x00);  // gyro +/-250 dps
    WriteReg(s_mpu6050, 0x1C, 0x08);  // accel +/-4 g (8192 LSB/g)
    if (who_am_i == 0x70) {
        WriteReg(s_mpu6050, 0x1D, 0x03);  // MPU6500 accel filter 41 Hz
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "MPU6050 ready at 0x%02x (WHO_AM_I=0x%02x)", address, who_am_i);
    return true;
}

bool ReadMpu6050(float* accel_g, float* gyro_dps) {
    if (s_mpu6050 == nullptr) {
        return false;
    }
    uint8_t raw[14] = {0};
    if (ReadReg(s_mpu6050, 0x3B, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }

    for (int i = 0; i < 3; ++i) {
        int16_t value = (int16_t)(((uint16_t)raw[i * 2] << 8) | raw[i * 2 + 1]);
        accel_g[i] = value / 8192.0f;
    }
    for (int i = 0; i < 3; ++i) {
        int16_t value = (int16_t)(((uint16_t)raw[8 + i * 2] << 8) | raw[8 + i * 2 + 1]);
        gyro_dps[i] = value / 131.0f;
    }
    return true;
}

}  // namespace

SensorHub& SensorHub::GetInstance() {
    static SensorHub instance;
    return instance;
}

void SensorHub::SetShakeCallback(std::function<void(ShakeStrength)> callback) {
    std::lock_guard<std::mutex> lock(motion_mutex_);
    shake_callback_ = std::move(callback);
}

void SensorHub::Start() {
    if (started_) {
        return;
    }

    if (!InitI2cBus()) {
        // 保持 started_ 为 false，允许上层在瞬态失败后重试
        return;
    }
    started_ = true;

    bool aht_ok = InitAht20();
    bool bmp_ok = InitBmp280();
    bool mpu_ok = InitMpu6050();

    {
        std::lock_guard<std::mutex> lock(env_mutex_);
        env_.aht_present = aht_ok;
        env_.bmp_present = bmp_ok;
    }
    {
        std::lock_guard<std::mutex> lock(motion_mutex_);
        motion_.present = mpu_ok;
    }

    if (aht_ok || bmp_ok) {
        xTaskCreate([](void* arg) {
            static_cast<SensorHub*>(arg)->EnvironmentTask();
            vTaskDelete(nullptr);
        }, "env_sensor", 4096, this, 3, nullptr);
    }
    if (mpu_ok) {
        xTaskCreate([](void* arg) {
            static_cast<SensorHub*>(arg)->MotionTask();
            vTaskDelete(nullptr);
        }, "motion_sensor", 4096, this, 4, nullptr);
    }
}

EnvironmentReading SensorHub::GetEnvironment() {
    std::lock_guard<std::mutex> lock(env_mutex_);
    EnvironmentReading reading = env_;
    // 传感器失效后不能继续把最后一次读数当成有效值报给用户
    constexpr int64_t kEnvStaleUs = 30 * 1000000;
    const int64_t now = esp_timer_get_time();
    if (now - reading.aht_updated_us > kEnvStaleUs) {
        reading.temperature_valid = false;
        reading.humidity_valid = false;
    }
    if (now - reading.bmp_updated_us > kEnvStaleUs) {
        reading.pressure_valid = false;
        reading.pressure_trend_valid = false;
    }
    return reading;
}

MotionReading SensorHub::GetMotion() {
    std::lock_guard<std::mutex> lock(motion_mutex_);
    return motion_;
}

ShakeStats SensorHub::GetShakeStats() {
    ShakeStats stats;
    stats.total_count = shake_count_.load();
    stats.heavy_count = heavy_shake_count_.load();
    int64_t last_us = last_shake_us_.load();
    if (last_us != 0) {
        stats.has_last = true;
        stats.last_strength = static_cast<ShakeStrength>(last_shake_strength_.load());
        stats.seconds_since_last = (int)((esp_timer_get_time() - last_us) / 1000000);
    }
    return stats;
}

void SensorHub::EnvironmentTask() {
    TickType_t last_wake = xTaskGetTickCount();
    int aht_errors = 0;
    int bmp_errors = 0;

    while (true) {
        float aht_temperature = 0.0f;
        float humidity = 0.0f;
        float bmp_temperature = 0.0f;
        float pressure = 0.0f;
        bool aht_ok = ReadAht20(&aht_temperature, &humidity);
        bool bmp_ok = ReadBmp280(&bmp_temperature, &pressure);

        if (!aht_ok && ++aht_errors % 12 == 1) {
            ESP_LOGW(TAG, "AHT20 read failed (%d)", aht_errors);
        }
        if (!bmp_ok && ++bmp_errors % 12 == 1) {
            ESP_LOGW(TAG, "BMP280 read failed (%d)", bmp_errors);
        }

        {
            std::lock_guard<std::mutex> lock(env_mutex_);
            if (aht_ok) {
                env_.temperature_c = aht_temperature;
                env_.humidity_pct = humidity;
                env_.temperature_valid = true;
                env_.humidity_valid = true;
                env_.aht_updated_us = esp_timer_get_time();
            }
            if (bmp_ok) {
                env_.pressure_hpa = pressure;
                env_.pressure_valid = true;
                env_.bmp_updated_us = esp_timer_get_time();

                s_pressure_history[s_pressure_head] = pressure;
                s_pressure_head = (s_pressure_head + 1) % kPressureHistorySize;
                if (s_pressure_count < kPressureHistorySize) {
                    ++s_pressure_count;
                }
                if (s_pressure_count >= kPressureTrendMinSamples) {
                    // 环形缓冲区里最老的那个点
                    int oldest = (s_pressure_head - s_pressure_count + kPressureHistorySize) %
                                 kPressureHistorySize;
                    env_.pressure_trend_1h_hpa = pressure - s_pressure_history[oldest];
                    env_.pressure_trend_valid = true;
                }
            }
            if (aht_ok || bmp_ok) {
                env_.updated_us = esp_timer_get_time();
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kEnvPeriodMs));
    }
}

void SensorHub::MotionTask() {
    // 陀螺仪零偏标定：开机保持静止约 1 秒
    float gyro_bias[3] = {0.0f, 0.0f, 0.0f};
    float accel[3] = {0.0f, 0.0f, 0.0f};
    float gyro[3] = {0.0f, 0.0f, 0.0f};
    int samples = 0;
    for (int i = 0; i < 100; ++i) {
        if (ReadMpu6050(accel, gyro)) {
            gyro_bias[0] += gyro[0];
            gyro_bias[1] += gyro[1];
            gyro_bias[2] += gyro[2];
            ++samples;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (samples > 0) {
        gyro_bias[0] /= samples;
        gyro_bias[1] /= samples;
        gyro_bias[2] /= samples;
    }
    ESP_LOGI(TAG, "Gyro bias: %.2f %.2f %.2f dps", gyro_bias[0], gyro_bias[1], gyro_bias[2]);

    float gravity[3] = {0.0f, 0.0f, 0.0f};
    bool gravity_ready = false;
    float dynamic_envelope = 0.0f;
    int reversals = 0;
    int last_sign = 0;
    int last_axis = -1;
    int64_t last_reversal_us = 0;
    int64_t last_shake_us = 0;
    float shake_peak_g = 0.0f;
    bool last_shake_heavy = false;
    int64_t last_impact_us = 0;
    int64_t moving_since_us = 0;
    int64_t last_read_us = 0;
    int read_errors = 0;

    // 标定结束后再等 3 秒才开始判定摇一摇，避开开机时手持调整造成的误触发
    int64_t detect_start_us = esp_timer_get_time() + 3000000;

    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        if (!ReadMpu6050(accel, gyro)) {
            if (++read_errors % 100 == 1) {
                ESP_LOGW(TAG, "MPU6050 read failed (%d)", read_errors);
            }
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kMotionPeriodMs));
            continue;
        }
        read_errors = 0;

        for (int i = 0; i < 3; ++i) {
            gyro[i] -= gyro_bias[i];
        }

        if (!gravity_ready) {
            memcpy(gravity, accel, sizeof(gravity));
            gravity_ready = true;
        }

        float delta[3];
        for (int i = 0; i < 3; ++i) {
            delta[i] = accel[i] - gravity[i];
            gravity[i] += 0.03f * (accel[i] - gravity[i]);
        }
        float dynamic_g = sqrtf(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
        dynamic_envelope += 0.08f * (dynamic_g - dynamic_envelope);

        int64_t now_us = esp_timer_get_time();
        last_read_us = now_us;

        // 摇一摇：0.9 秒内出现 3 次方向反转，且幅度足够大
        if (now_us > detect_start_us && dynamic_g > 0.45f) {
            int axis = 0;
            float best = fabsf(delta[0]);
            for (int i = 1; i < 3; ++i) {
                if (fabsf(delta[i]) > best) {
                    best = fabsf(delta[i]);
                    axis = i;
                }
            }
            int sign = delta[axis] >= 0.0f ? 1 : -1;
            if (axis != last_axis) {
                // 主轴变化时跨轴比较符号没有意义，重新起算
                last_axis = axis;
                last_sign = sign;
                last_reversal_us = now_us;
                reversals = 1;
                shake_peak_g = dynamic_g;
            } else if (sign != last_sign) {
                if (last_sign != 0 && now_us - last_reversal_us < 900000) {
                    ++reversals;
                } else {
                    reversals = 1;
                    shake_peak_g = 0.0f;
                }
                last_reversal_us = now_us;
                last_sign = sign;
            }
            if (dynamic_g > shake_peak_g) {
                shake_peak_g = dynamic_g;
            }
        }
        if (now_us - last_reversal_us > 1200000) {
            reversals = 0;
            last_sign = 0;
            last_axis = -1;
            shake_peak_g = 0.0f;
        }

        std::function<void(ShakeStrength)> shake_callback;
        ShakeStrength shake_strength = ShakeStrength::kLight;
        // 轻摇冷却 2 秒，重摇冷却 5 秒，避免连续触发
        int64_t shake_cooldown_us = last_shake_heavy ? 5000000 : 2000000;
        if (reversals >= 3 && now_us - last_shake_us > shake_cooldown_us) {
            int reversal_count = reversals;
            float peak = shake_peak_g;
            // 反转 5 次以上或峰值超过 1.5g 算重摇
            shake_strength =
                (reversal_count >= 5 || peak >= 1.5f) ? ShakeStrength::kHeavy : ShakeStrength::kLight;
            last_shake_us = now_us;
            last_shake_heavy = (shake_strength == ShakeStrength::kHeavy);
            shake_count_.fetch_add(1);
            last_shake_us_.store(now_us);
            last_shake_strength_.store(static_cast<int>(shake_strength));
            if (last_shake_heavy) {
                heavy_shake_count_.fetch_add(1);
            }
            reversals = 0;
            last_sign = 0;
            shake_peak_g = 0.0f;
            ESP_LOGI(TAG, "Shake detected (%s, peak %.2fg, reversals %d)",
                     shake_strength == ShakeStrength::kHeavy ? "heavy" : "light", peak,
                     reversal_count);
            {
                std::lock_guard<std::mutex> lock(motion_mutex_);
                shake_callback = shake_callback_;
            }
        }
        if (shake_callback) {
            shake_callback(shake_strength);
        }

        if (dynamic_g > 1.4f) {
            last_impact_us = now_us;
        }

        std::string state;
        if (now_us - last_impact_us < 2000000) {
            state = "impact";
        } else if (now_us - last_shake_us < 1500000) {
            state = "shaking";
        } else if (dynamic_envelope > 0.055f) {
            state = "walking";
        } else {
            state = "still";
        }

        if ((state == "walking" || state == "shaking" || state == "impact") && moving_since_us == 0) {
            moving_since_us = now_us;
        } else if (state == "still") {
            moving_since_us = 0;
        }

        std::string orientation;
        if (fabsf(accel[2]) > 0.75f) {
            orientation = accel[2] > 0.0f ? "face_up" : "face_down";
        } else if (fabsf(accel[1]) > 0.75f) {
            orientation = accel[1] > 0.0f ? "upright" : "upside_down";
        } else if (fabsf(accel[0]) > 0.75f) {
            orientation = accel[0] > 0.0f ? "landscape_right" : "landscape_left";
        } else {
            orientation = "tilted";
        }

        {
            std::lock_guard<std::mutex> lock(motion_mutex_);
            motion_.valid = true;
            motion_.accel_g[0] = accel[0];
            motion_.accel_g[1] = accel[1];
            motion_.accel_g[2] = accel[2];
            motion_.gyro_dps[0] = gyro[0];
            motion_.gyro_dps[1] = gyro[1];
            motion_.gyro_dps[2] = gyro[2];
            motion_.dynamic_g = dynamic_envelope;
            motion_.state = state;
            motion_.orientation = orientation;
            motion_.face_down = (orientation == "face_down");
            motion_.impact_recent = (now_us - last_impact_us < 2000000);
            motion_.moving_duration_s =
                moving_since_us == 0 ? 0.0f : (float)(now_us - moving_since_us) / 1000000.0f;
            motion_.updated_us = last_read_us;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kMotionPeriodMs));
    }
}
