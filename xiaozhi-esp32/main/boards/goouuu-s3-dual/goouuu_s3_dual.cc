#include "dual_network_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "sensor_hub.h"
#include "gps_receiver.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <wifi_manager.h>

#include <cstdio>
#include <cstring>
#include <string>

#define TAG "GoouuuS3Dual"

namespace {

std::string FormatFloat(const char* format, float value) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), format, value);
    return buffer;
}

const char* MotionStateText(const std::string& state) {
    if (state == "still") {
        return "静止";
    }
    if (state == "walking") {
        return "移动中";
    }
    if (state == "shaking") {
        return "摇晃中";
    }
    if (state == "impact") {
        return "受到撞击";
    }
    return "未知";
}

const char* OrientationText(const std::string& orientation) {
    if (orientation == "face_up") {
        return "屏幕朝上";
    }
    if (orientation == "face_down") {
        return "屏幕朝下";
    }
    if (orientation == "upright") {
        return "竖立";
    }
    if (orientation == "upside_down") {
        return "倒置";
    }
    if (orientation == "landscape_right") {
        return "横放（右）";
    }
    if (orientation == "landscape_left") {
        return "横放（左）";
    }
    if (orientation == "tilted") {
        return "倾斜放置";
    }
    return "未知";
}

const std::string_view RandomComplainSound() {
    static const std::string_view kComplaints[] = {
        Lang::Sounds::OGG_SHAKE_COMPLAIN_1, Lang::Sounds::OGG_SHAKE_COMPLAIN_2,
        Lang::Sounds::OGG_SHAKE_COMPLAIN_3, Lang::Sounds::OGG_SHAKE_COMPLAIN_4,
        Lang::Sounds::OGG_SHAKE_COMPLAIN_5, Lang::Sounds::OGG_SHAKE_COMPLAIN_6,
        Lang::Sounds::OGG_SHAKE_COMPLAIN_7, Lang::Sounds::OGG_SHAKE_COMPLAIN_8,
        Lang::Sounds::OGG_SHAKE_COMPLAIN_9, Lang::Sounds::OGG_SHAKE_COMPLAIN_10,
    };
    constexpr size_t kCount = sizeof(kComplaints) / sizeof(kComplaints[0]);
    return kComplaints[esp_random() % kCount];
}

}  // namespace

class GoouuuS3DualBoard : public DualNetworkBoard {
private:
    Button boot_button_;
    LcdDisplay* display_ = nullptr;
    esp_timer_handle_t info_timer_ = nullptr;
    esp_timer_handle_t gesture_timer_ = nullptr;
    // 用绝对时间戳而不是定时器回调次数，避免系统繁忙时“60 秒”被拉长
    int64_t still_since_us_ = 0;
    bool screen_dimmed_ = false;
    bool screen_off_ = false;

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X,
                                     DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                                     DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            ESP_LOGI("GoouuuS3Dual", "BOOT click, state=%d", (int)app.GetDeviceState());
            if (app.GetDeviceState() == kDeviceStateStarting && IsWifiActive()) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // 双击 BOOT：自动 -> 仅 WiFi -> 仅 4G -> 自动
        boot_button_.OnDoubleClick([this]() {
            auto state = Application::GetInstance().GetDeviceState();
            if (state == kDeviceStateIdle || state == kDeviceStateStarting ||
                state == kDeviceStateWifiConfiguring) {
                CycleNetworkMode();
            } else {
                display_->ShowNotification("请结束当前对话后再切换网络", 3000);
            }
        });

        // 长按 BOOT：进入 WiFi 配网模式
        boot_button_.OnLongPress([this]() {
            EnterWifiConfigMode();
        });
    }

    void InitializeSensors() {
        SensorHub::GetInstance().SetShakeCallback([this](ShakeStrength strength) {
            OnShake(strength);
        });
        SensorHub::GetInstance().Start();

        // GPS 未在编译期启用时 Start() 只打印一条日志，不影响其他功能
        GpsReceiver::GetInstance().Start();
    }

    void OnShake(ShakeStrength strength) {
        // 摇一摇回调来自 50Hz 运动任务，切到主任务再动界面和状态机
        Application::GetInstance().Schedule([this, strength]() {
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            // 连接、激活、配网阶段不响应，避免干扰状态机
            if (state != kDeviceStateIdle && state != kDeviceStateListening &&
                state != kDeviceStateSpeaking) {
                return;
            }

            if (strength == ShakeStrength::kLight) {
                // 轻摇只给一点语气，不打断当前对话
                ESP_LOGI(TAG, "Light shake");
                if (display_ != nullptr) {
                    display_->SetEmotion("confused");
                }
                // 只在待机时出声：聆听时会被麦克风采集上传，说话时会被 TTS 排队，都起不到即时反馈
                if (state == kDeviceStateIdle) {
                    app.PlaySound(Lang::Sounds::OGG_SHAKE_LIGHT);
                }
                return;
            }

            // 重摇：本地语音包随机播放一句抱怨，不依赖服务端文本能力
            ESP_LOGI(TAG, "Heavy shake, play local complaint");
            app.PlaySoundInterrupting(RandomComplainSound());
            // 打断会切到 idle 并把表情重置为 neutral，所以表情放在状态切换之后
            if (display_ != nullptr) {
                display_->SetEmotion("annoyed");
            }
        });
    }

    std::string BuildInfoText() {
        std::string text;
        switch (GetNetworkMode()) {
            case NetworkMode::AUTO:
                text = "自动";
                break;
            case NetworkMode::WIFI_ONLY:
                text = "仅WiFi";
                break;
            default:
                text = "仅4G";
                break;
        }
        text += IsWifiActive() ? "·WiFi" : "·4G";
        text += IsNetworkConnected() ? "在线" : "离线";

        auto env = SensorHub::GetInstance().GetEnvironment();
        if (env.temperature_valid) {
            text += " ·" + FormatFloat("%.1f°C", env.temperature_c);
        }
        if (env.humidity_valid) {
            text += " " + FormatFloat("%.0f%%", env.humidity_pct);
        }
        if (env.pressure_valid) {
            text += " ·" + FormatFloat("%.0fhPa", env.pressure_hpa);
        }

        auto motion = SensorHub::GetInstance().GetMotion();
        if (motion.valid) {
            text += " ·";
            text += MotionStateText(motion.state);
        }

        auto fix = GpsReceiver::GetInstance().GetFix();
        if (fix.valid && !fix.city.empty()) {
            text += " ·" + fix.city;
        }
        return text;
    }

    void UpdateInfoBar() {
        if (display_ == nullptr) {
            return;
        }
        // SetInfoText 内部已加显示锁，这里不能再套一层，避免重复加锁
        display_->SetInfoText(BuildInfoText().c_str());
    }

    void StartInfoTimer() {
        esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    static_cast<GoouuuS3DualBoard*>(arg)->UpdateInfoBar();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "info_bar",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&timer_args, &info_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create info bar timer");
            info_timer_ = nullptr;
            return;
        }
        if (esp_timer_start_periodic(info_timer_, 5000 * 1000ULL) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start info bar timer");
        }
    }

    // 手势：拿起亮屏、静止 60 秒降亮、静止 5 分钟熄屏
    void RestoreScreen() {
        if (!screen_dimmed_ && !screen_off_) {
            return;
        }
        screen_dimmed_ = false;
        screen_off_ = false;
        if (auto backlight = GetBacklight()) {
            backlight->RestoreBrightness();
        }
    }

    void UpdateGestures() {
        auto motion = SensorHub::GetInstance().GetMotion();
        if (!motion.present || !motion.valid) {
            return;
        }

        // 对话、配网、激活过程中不动屏幕，避免打断用户
        bool busy = Application::GetInstance().GetDeviceState() != kDeviceStateIdle;
        bool moving = motion.state == "walking" || motion.state == "shaking" ||
                      motion.state == "impact";

        if (busy || moving) {
            still_since_us_ = 0;
            RestoreScreen();
            return;
        }

        const int64_t now = esp_timer_get_time();
        if (still_since_us_ == 0) {
            still_since_us_ = now;
        }
        const int64_t still_seconds = (now - still_since_us_) / 1000000;

        auto backlight = GetBacklight();
        if (backlight == nullptr) {
            return;
        }
        if (still_seconds >= 300 && !screen_off_) {
            backlight->SetBrightness(0);
            screen_dimmed_ = false;
            screen_off_ = true;
        } else if (still_seconds >= 60 && !screen_dimmed_ && !screen_off_) {
            backlight->SetBrightness(20);
            screen_dimmed_ = true;
        }
    }

    void StartGestureTimer() {
        esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    static_cast<GoouuuS3DualBoard*>(arg)->UpdateGestures();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "gesture",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&timer_args, &gesture_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create gesture timer");
            gesture_timer_ = nullptr;
            return;
        }
        if (esp_timer_start_periodic(gesture_timer_, 1000 * 1000ULL) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start gesture timer");
        }
    }

    // 环境温湿度/气压查询
    void AddEnvironmentTool() {
        McpServer::GetInstance().AddTool(
            "self.sensor.environment",
            "读取设备所在环境的温度、湿度和大气压。用户询问现在多少度、湿度多少、气压多少时调用。",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                auto env = SensorHub::GetInstance().GetEnvironment();
                auto root = cJSON_CreateObject();
                cJSON_AddBoolToObject(root, "sensor_present", env.aht_present || env.bmp_present);
                cJSON_AddBoolToObject(root, "temperature_valid", env.temperature_valid);
                cJSON_AddBoolToObject(root, "humidity_valid", env.humidity_valid);
                cJSON_AddBoolToObject(root, "pressure_valid", env.pressure_valid);
                if (env.temperature_valid) {
                    cJSON_AddNumberToObject(root, "temperature_celsius", env.temperature_c);
                }
                if (env.humidity_valid) {
                    cJSON_AddNumberToObject(root, "humidity_percent", env.humidity_pct);
                }
                if (env.pressure_valid) {
                    cJSON_AddNumberToObject(root, "pressure_hpa", env.pressure_hpa);
                }
                if (env.pressure_trend_valid) {
                    cJSON_AddNumberToObject(root, "pressure_trend_1h_hpa", env.pressure_trend_1h_hpa);
                }
                if (env.updated_us != 0) {
                    cJSON_AddNumberToObject(root, "updated_s_ago",
                                            (double)(esp_timer_get_time() - env.updated_us) / 1e6);
                }

                std::string summary;
                if (env.updated_us == 0) {
                    summary = "传感器还没有读到有效数据，请稍后再试。";
                } else {
                    summary = "当前环境：";
                    if (env.temperature_valid) {
                        summary += "温度 " + FormatFloat("%.1f", env.temperature_c) + " 摄氏度，";
                    }
                    if (env.humidity_valid) {
                        summary += "湿度 " + FormatFloat("%.0f", env.humidity_pct) + "%，";
                    }
                    if (env.pressure_valid) {
                        summary += "气压 " + FormatFloat("%.0f", env.pressure_hpa) + " 百帕，";
                    }
                    summary.pop_back();
                    summary += "。";
                    if (env.pressure_trend_valid) {
                        float trend = env.pressure_trend_1h_hpa;
                        summary += "过去一小时气压";
                        if (trend > 0.5f) {
                            summary += "上升约 " + FormatFloat("%.1f", trend) + " 百帕。";
                        } else if (trend < -0.5f) {
                            summary += "下降约 " + FormatFloat("%.1f", -trend) + " 百帕。";
                        } else {
                            summary += "基本持平。";
                        }
                    }
                    double age_s = (double)(esp_timer_get_time() - env.updated_us) / 1e6;
                    if (age_s > 60.0) {
                        summary += "注意数据是 " + std::to_string((int)age_s) + " 秒前采集的。";
                    }
                }
                cJSON_AddStringToObject(root, "summary", summary.c_str());
                return root;
            });
    }

    // 六轴姿态/运动状态查询
    void AddMotionTool() {
        McpServer::GetInstance().AddTool(
            "self.sensor.motion",
            "读取设备自身的六轴姿态和运动状态，可判断是否被拿起、摇晃、翻转。用户询问设备姿态、"
            "是否在移动、是否被摇晃时调用。",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                auto motion = SensorHub::GetInstance().GetMotion();
                auto root = cJSON_CreateObject();
                cJSON_AddBoolToObject(root, "sensor_present", motion.present);
                cJSON_AddBoolToObject(root, "valid", motion.valid);
                cJSON_AddStringToObject(root, "state", motion.state.c_str());
                cJSON_AddStringToObject(root, "orientation", motion.orientation.c_str());
                cJSON_AddNumberToObject(root, "moving_seconds", motion.moving_duration_s);
                cJSON* accel = cJSON_CreateArray();
                cJSON* gyro = cJSON_CreateArray();
                for (int i = 0; i < 3; ++i) {
                    cJSON_AddItemToArray(accel, cJSON_CreateNumber(motion.accel_g[i]));
                    cJSON_AddItemToArray(gyro, cJSON_CreateNumber(motion.gyro_dps[i]));
                }
                cJSON_AddItemToObject(root, "accel_g", accel);
                cJSON_AddItemToObject(root, "gyro_dps", gyro);

                std::string summary;
                if (!motion.present) {
                    summary = "没有检测到六轴传感器。";
                } else if (!motion.valid) {
                    summary = "六轴传感器正在初始化，请稍后再试。";
                } else {
                    summary = "设备当前";
                    summary += MotionStateText(motion.state);
                    summary += "，摆放姿态为";
                    summary += OrientationText(motion.orientation);
                    summary += "。";
                }
                cJSON_AddStringToObject(root, "summary", summary.c_str());
                return root;
            });
    }

    // 定位查询：先经纬度，再城市名
    void AddLocationTool() {
        McpServer::GetInstance().AddTool(
            "self.location.get_position",
            "获取设备当前位置。回答用户时必须先报纬度和经度，再报城市名称。",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                auto fix = GpsReceiver::GetInstance().GetFix();
                auto root = cJSON_CreateObject();
                cJSON_AddBoolToObject(root, "gps_enabled", fix.started);
                cJSON_AddBoolToObject(root, "valid", fix.valid);
                if (fix.valid) {
                    cJSON_AddNumberToObject(root, "latitude", fix.latitude);
                    cJSON_AddNumberToObject(root, "longitude", fix.longitude);
                    cJSON_AddNumberToObject(root, "altitude_m", fix.altitude_m);
                    cJSON_AddNumberToObject(root, "speed_kmh", fix.speed_kmh);
                    cJSON_AddNumberToObject(root, "course_deg", fix.course_deg);
                    cJSON_AddNumberToObject(root, "hdop", fix.hdop);
                    cJSON_AddNumberToObject(root, "satellites", fix.satellites);
                    if (fix.fix_us != 0) {
                        cJSON_AddNumberToObject(root, "fix_age_s",
                                                (double)(esp_timer_get_time() - fix.fix_us) / 1e6);
                    }
                }
                if (!fix.city.empty()) {
                    cJSON_AddStringToObject(root, "city", fix.city.c_str());
                    cJSON_AddNumberToObject(root, "city_distance_km", fix.city_distance_km);
                }

                std::string summary;
                if (!fix.started) {
                    summary = "当前固件没有启用 GPS，无法定位。";
                } else if (!fix.valid) {
                    summary = "GPS 还没有定位成功，请到室外空旷处等待一到两分钟。";
                } else {
                    char coordinates[96];
                    snprintf(coordinates, sizeof(coordinates), "纬度 %.5f，经度 %.5f。",
                             fix.latitude, fix.longitude);
                    summary = coordinates;
                    if (!fix.city.empty()) {
                        summary += "位置在" + fix.city;
                        if (fix.city_distance_km >= 0.0f) {
                            summary += "，距离市中心约 " + FormatFloat("%.1f", fix.city_distance_km) +
                                       " 公里";
                        }
                        summary += "。";
                    }
                    summary += "卫星数 " + std::to_string(fix.satellites) + "。";
                }
                cJSON_AddStringToObject(root, "summary", summary.c_str());
                return root;
            });
    }

    // 网络状态查询
    void AddNetworkTools() {
        McpServer::GetInstance().AddTool(
            "self.network.get_status",
            "查询当前网络模式（自动/仅WiFi/仅4G）、实际使用的链路以及是否已联网。",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                auto root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "mode", GetNetworkModeName());
                cJSON_AddStringToObject(root, "active_link", IsWifiActive() ? "wifi" : "cellular");
                cJSON_AddBoolToObject(root, "connected", IsNetworkConnected());

                std::string summary;
                summary = std::string("当前网络模式为") +
                          (GetNetworkMode() == NetworkMode::AUTO
                               ? "自动（WiFi 优先）"
                               : (GetNetworkMode() == NetworkMode::WIFI_ONLY ? "仅 WiFi" : "仅 4G"));

                if (IsWifiActive()) {
                    auto& wifi = WifiManager::GetInstance();
                    if (wifi.IsConnected()) {
                        cJSON_AddStringToObject(root, "ssid", wifi.GetSsid().c_str());
                        cJSON_AddStringToObject(root, "ip", wifi.GetIpAddress().c_str());
                        cJSON_AddNumberToObject(root, "rssi", wifi.GetRssi());
                        summary += "，已通过 WiFi 联网，名称 " + wifi.GetSsid() + "。";
                    } else {
                        summary += "，WiFi 尚未连接。";
                    }
                } else {
                    auto* ml307 = dynamic_cast<Ml307Board*>(&GetCurrentBoard());
                    if (ml307 != nullptr) {
                        cJSON_AddNumberToObject(root, "csq", ml307->GetCsq());
                        summary += ml307->IsNetworkReady() ? "，4G 已联网。" : "，4G 尚未联网。";
                    } else {
                        summary += "，4G 尚未联网。";
                    }
                }
                cJSON_AddStringToObject(root, "summary", summary.c_str());
                return root;
            });

        McpServer::GetInstance().AddTool(
            "self.network.set_mode",
            "切换网络模式。mode 取值：auto（WiFi 优先，连不上自动用 4G）、wifi（仅 WiFi）、"
            "4g（仅 4G）。切换后设备会重启生效。",
            PropertyList({Property("mode", kPropertyTypeString)}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string mode = properties["mode"].value<std::string>();
                NetworkMode target;
                const char* text = nullptr;
                if (mode == "auto") {
                    target = NetworkMode::AUTO;
                    text = "已切换为自动模式（WiFi 优先，连不上自动用 4G）";
                } else if (mode == "wifi") {
                    target = NetworkMode::WIFI_ONLY;
                    text = "已切换为仅 WiFi 模式";
                } else if (mode == "4g" || mode == "cellular" || mode == "ml307") {
                    target = NetworkMode::ML307_ONLY;
                    text = "已切换为仅 4G 模式";
                } else {
                    return std::string("不支持的网络模式：") + mode +
                           "，可选 auto、wifi、4g。";
                }

                SetNetworkMode(target);
                // 延迟重启放到独立小任务里，避免 vTaskDelay 阻塞主事件循环
                xTaskCreate([](void*) {
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    Application::GetInstance().Schedule([]() {
                        Application::GetInstance().Reboot();
                    });
                    vTaskDelete(nullptr);
                }, "mode_reboot", 2048, nullptr, 2, nullptr);
                return std::string(text) + "，设备将在两秒后重启生效。";
            });
    }

    void AddDeviceTool() {
        McpServer::GetInstance().AddTool(
            "self.device.get_status",
            "查询设备整体状态，包括网络、传感器是否在线、音量和屏幕亮度。",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                auto env = SensorHub::GetInstance().GetEnvironment();
                auto motion = SensorHub::GetInstance().GetMotion();
                auto fix = GpsReceiver::GetInstance().GetFix();

                auto root = cJSON_CreateObject();
                // GetBoardType() 返回的是当前网络子板（wifi/ml307），对外报告要用真实板名
                cJSON_AddStringToObject(root, "board", BOARD_NAME);
                cJSON_AddStringToObject(root, "network_board", GetBoardType().c_str());
                cJSON_AddStringToObject(root, "network_mode", GetNetworkModeName());
                cJSON_AddBoolToObject(root, "network_connected", IsNetworkConnected());

                auto sensors = cJSON_CreateObject();
                cJSON_AddBoolToObject(sensors, "aht20", env.aht_present);
                cJSON_AddBoolToObject(sensors, "bmp280", env.bmp_present);
                cJSON_AddBoolToObject(sensors, "mpu6050", motion.present);
                cJSON_AddBoolToObject(sensors, "gps", fix.started);
                cJSON_AddItemToObject(root, "sensors", sensors);

                if (auto codec = GetAudioCodec()) {
                    cJSON_AddNumberToObject(root, "volume", codec->output_volume());
                }
                if (auto backlight = GetBacklight()) {
                    cJSON_AddNumberToObject(root, "brightness", backlight->brightness());
                }

                std::string summary = "设备状态：网络";
                summary += IsNetworkConnected() ? "已连接" : "未连接";
                summary += "，温湿度传感器";
                summary += env.aht_present ? "正常" : "未检测到";
                summary += "，气压传感器";
                summary += env.bmp_present ? "正常" : "未检测到";
                summary += "，六轴传感器";
                summary += motion.present ? "正常" : "未检测到";
                summary += "，GPS";
                summary += fix.started ? "已启用" : "未启用";
                summary += "。";
                cJSON_AddStringToObject(root, "summary", summary.c_str());
                return root;
            });
    }

    // 摇晃记录：供 AI 在下一轮对话里接着吐槽
    void AddShakeTool() {
        McpServer::GetInstance().AddTool(
            "self.device.shake_status",
            "查询设备被摇晃的记录：累计摇晃次数、重摇次数、最近一次摇晃在多少秒前。"
            "用户提到刚才摇了设备、或需要就摇晃这件事吐槽时调用。",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                auto stats = SensorHub::GetInstance().GetShakeStats();
                auto root = cJSON_CreateObject();
                cJSON_AddNumberToObject(root, "total_count", stats.total_count);
                cJSON_AddNumberToObject(root, "heavy_count", stats.heavy_count);
                cJSON_AddBoolToObject(root, "has_last", stats.has_last);
                if (stats.has_last) {
                    cJSON_AddNumberToObject(root, "seconds_since_last", stats.seconds_since_last);
                    cJSON_AddStringToObject(root, "last_strength",
                                            stats.last_strength == ShakeStrength::kHeavy ? "heavy"
                                                                                         : "light");
                }

                std::string summary;
                if (!stats.has_last) {
                    summary = "设备还没有被摇晃过。";
                } else {
                    summary = "设备最近一次被";
                    summary += stats.last_strength == ShakeStrength::kHeavy ? "用力摇晃" : "轻轻摇晃";
                    summary += "是在 " + std::to_string(stats.seconds_since_last) + " 秒前";
                    summary += "，累计摇晃 " + std::to_string(stats.total_count) + " 次，其中重摇 " +
                               std::to_string(stats.heavy_count) + " 次。";
                }
                cJSON_AddStringToObject(root, "summary", summary.c_str());
                return root;
            });
    }

    void InitializeTools() {
        AddEnvironmentTool();
        AddMotionTool();
        AddLocationTool();
        AddNetworkTools();
        AddDeviceTool();
        AddShakeTool();
    }

public:
    GoouuuS3DualBoard() :
        DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, 2, ML307_BAUD_RATE),
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeSensors();
        InitializeTools();
        StartInfoTimer();
        StartGestureTimer();
        UpdateInfoBar();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
    }

    virtual ~GoouuuS3DualBoard() {
        if (info_timer_ != nullptr) {
            esp_timer_stop(info_timer_);
            esp_timer_delete(info_timer_);
        }
        if (gesture_timer_ != nullptr) {
            esp_timer_stop(gesture_timer_);
            esp_timer_delete(gesture_timer_);
        }
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }
};

DECLARE_BOARD(GoouuuS3DualBoard);
