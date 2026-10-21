#include "gps_receiver.h"

#include "config.h"

#include <driver/uart.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

static const char* TAG = "GpsReceiver";

namespace {

// 超过该时长没有新的有效定位，就认为定位已失效
constexpr int64_t kFixStaleTimeoutUs = 10 * 1000 * 1000;
// 距离最近城市超过该值时不再报城市名，避免在境外或城市表覆盖不到的地方给出错误城市
constexpr float kMaxCityMatchKm = 80.0f;

struct CityEntry {
    const char* name;
    float latitude;
    float longitude;
};

// 离线城市表：省级/地级市中心点，用于把经纬度映射成城市名。
// 只做最近匹配，不保证行政区划完全准确；城市级演示足够。
const CityEntry kCities[] = {
    {"北京", 39.9042f, 116.4074f},   {"天津", 39.3434f, 117.3616f},
    {"石家庄", 38.0428f, 114.5149f}, {"唐山", 39.6305f, 118.1802f},
    {"秦皇岛", 39.9354f, 119.6005f}, {"邯郸", 36.6255f, 114.5391f},
    {"保定", 38.8739f, 115.4646f},   {"张家口", 40.7686f, 114.8869f},
    {"承德", 40.9515f, 117.9634f},   {"沧州", 38.3045f, 116.8387f},
    {"廊坊", 39.5379f, 116.6837f},   {"太原", 37.8706f, 112.5489f},
    {"大同", 40.0903f, 113.3001f},   {"阳泉", 37.8567f, 113.5805f},
    {"长治", 36.1954f, 113.1163f},   {"晋城", 35.4907f, 112.8513f},
    {"朔州", 39.3317f, 112.4329f},   {"晋中", 37.6870f, 112.7527f},
    {"运城", 35.0263f, 111.0069f},   {"忻州", 38.4167f, 112.7341f},
    {"临汾", 36.0880f, 111.5190f},   {"吕梁", 37.5193f, 111.1445f},
    {"呼和浩特", 40.8426f, 111.7492f}, {"包头", 40.6574f, 109.8403f},
    {"乌海", 39.6550f, 106.7942f},   {"赤峰", 42.2586f, 118.8870f},
    {"通辽", 43.6174f, 122.2438f},   {"鄂尔多斯", 39.6086f, 109.7810f},
    {"呼伦贝尔", 49.2116f, 119.7658f}, {"巴彦淖尔", 40.7432f, 107.3877f},
    {"乌兰察布", 40.9945f, 113.1336f}, {"沈阳", 41.8057f, 123.4315f},
    {"大连", 38.9140f, 121.6147f},   {"鞍山", 41.1086f, 122.9946f},
    {"抚顺", 41.8807f, 123.9572f},   {"本溪", 41.2942f, 123.7665f},
    {"丹东", 40.1290f, 124.3547f},   {"锦州", 41.0951f, 121.1270f},
    {"营口", 40.6672f, 122.2355f},   {"阜新", 42.0216f, 121.6703f},
    {"辽阳", 41.2694f, 123.2369f},   {"盘锦", 41.1245f, 122.0707f},
    {"铁岭", 42.2862f, 123.8443f},   {"朝阳", 41.5737f, 120.4509f},
    {"葫芦岛", 40.7111f, 120.8369f}, {"长春", 43.8171f, 125.3235f},
    {"吉林", 43.8378f, 126.5496f},   {"四平", 43.1664f, 124.3504f},
    {"辽源", 42.9027f, 125.1437f},   {"通化", 41.7284f, 125.9398f},
    {"白山", 41.9409f, 126.4239f},   {"松原", 45.1411f, 124.8252f},
    {"白城", 45.6196f, 122.8390f},   {"哈尔滨", 45.8038f, 126.5350f},
    {"齐齐哈尔", 47.3543f, 123.9182f}, {"鸡西", 45.2951f, 130.9695f},
    {"鹤岗", 47.3497f, 130.2977f},   {"双鸭山", 46.6464f, 131.1591f},
    {"大庆", 46.5892f, 125.1041f},   {"伊春", 47.7275f, 128.8407f},
    {"佳木斯", 46.7998f, 130.3189f}, {"七台河", 45.7714f, 131.0035f},
    {"牡丹江", 44.5514f, 129.6329f}, {"黑河", 50.2452f, 127.5287f},
    {"绥化", 46.6535f, 126.9687f},   {"上海", 31.2304f, 121.4737f},
    {"南京", 32.0603f, 118.7969f},   {"无锡", 31.4912f, 120.3119f},
    {"徐州", 34.2044f, 117.2858f},   {"常州", 31.8107f, 119.9741f},
    {"苏州", 31.2989f, 120.5853f},   {"南通", 31.9802f, 120.8943f},
    {"连云港", 34.5967f, 119.2216f}, {"淮安", 33.6103f, 119.0153f},
    {"盐城", 33.3474f, 120.1636f},   {"扬州", 32.3942f, 119.4128f},
    {"镇江", 32.1882f, 119.4249f},   {"泰州", 32.4554f, 119.9231f},
    {"宿迁", 33.9631f, 118.2752f},   {"杭州", 30.2741f, 120.1551f},
    {"宁波", 29.8683f, 121.5440f},   {"温州", 27.9938f, 120.6994f},
    {"嘉兴", 30.7461f, 120.7555f},   {"湖州", 30.8940f, 120.0868f},
    {"绍兴", 30.0303f, 120.5802f},   {"金华", 29.0792f, 119.6474f},
    {"衢州", 28.9700f, 118.8594f},   {"舟山", 29.9853f, 122.2072f},
    {"台州", 28.6564f, 121.4208f},   {"丽水", 28.4676f, 119.9229f},
    {"合肥", 31.8206f, 117.2272f},   {"芜湖", 31.3526f, 118.4331f},
    {"蚌埠", 32.9160f, 117.3890f},   {"淮南", 32.6255f, 116.9997f},
    {"马鞍山", 31.6704f, 118.5069f}, {"淮北", 33.9550f, 116.7985f},
    {"铜陵", 30.9446f, 117.8121f},   {"安庆", 30.5433f, 117.0631f},
    {"黄山", 29.7147f, 118.3376f},   {"滁州", 32.3016f, 118.3171f},
    {"阜阳", 32.8901f, 115.8142f},   {"宿州", 33.6464f, 116.9642f},
    {"六安", 31.7411f, 116.5053f},   {"亳州", 33.8446f, 115.7786f},
    {"池州", 30.6647f, 117.4914f},   {"宣城", 30.9408f, 118.7588f},
    {"福州", 26.0745f, 119.2965f},   {"厦门", 24.4798f, 118.0894f},
    {"莆田", 25.4541f, 119.0077f},   {"三明", 26.2634f, 117.6389f},
    {"泉州", 24.8741f, 118.6757f},   {"漳州", 24.5131f, 117.6471f},
    {"南平", 26.6417f, 118.1777f},   {"龙岩", 25.0751f, 117.0172f},
    {"宁德", 26.6656f, 119.5478f},   {"南昌", 28.6820f, 115.8579f},
    {"景德镇", 29.2687f, 117.1784f}, {"萍乡", 27.6229f, 113.8546f},
    {"九江", 29.7051f, 116.0019f},   {"新余", 27.8174f, 114.9173f},
    {"鹰潭", 28.2602f, 117.0692f},   {"赣州", 25.8311f, 114.9350f},
    {"吉安", 27.1138f, 114.9925f},   {"宜春", 27.8144f, 114.4162f},
    {"抚州", 27.9489f, 116.3582f},   {"上饶", 28.4549f, 117.9434f},
    {"济南", 36.6512f, 117.1201f},   {"青岛", 36.0671f, 120.3826f},
    {"淄博", 36.8131f, 118.0548f},   {"枣庄", 34.8107f, 117.3236f},
    {"东营", 37.4346f, 118.6746f},   {"烟台", 37.4638f, 121.4479f},
    {"潍坊", 36.7069f, 119.1618f},   {"济宁", 35.4149f, 116.5871f},
    {"泰安", 36.1993f, 117.0879f},   {"威海", 37.5131f, 122.1204f},
    {"日照", 35.4164f, 119.5269f},   {"临沂", 35.1047f, 118.3564f},
    {"德州", 37.4355f, 116.3575f},   {"聊城", 36.4560f, 115.9854f},
    {"滨州", 37.3835f, 117.9704f},   {"菏泽", 35.2333f, 115.4809f},
    {"郑州", 34.7466f, 113.6254f},   {"开封", 34.7972f, 114.3076f},
    {"洛阳", 34.6197f, 112.4540f},   {"平顶山", 33.7660f, 113.1926f},
    {"安阳", 36.1034f, 114.3931f},   {"鹤壁", 35.7472f, 114.2972f},
    {"新乡", 35.3030f, 113.9268f},   {"焦作", 35.2159f, 113.2418f},
    {"濮阳", 35.7615f, 115.0290f},   {"许昌", 34.0357f, 113.8523f},
    {"漯河", 33.5815f, 114.0165f},   {"三门峡", 34.7730f, 111.2003f},
    {"南阳", 32.9908f, 112.5283f},   {"商丘", 34.4143f, 115.6564f},
    {"信阳", 32.1470f, 114.0913f},   {"周口", 33.6253f, 114.6496f},
    {"驻马店", 33.0141f, 114.0223f}, {"武汉", 30.5928f, 114.3055f},
    {"黄石", 30.2000f, 115.0389f},   {"十堰", 32.6292f, 110.7980f},
    {"宜昌", 30.6919f, 111.2865f},   {"襄阳", 32.0090f, 112.1220f},
    {"鄂州", 30.3919f, 114.8948f},   {"荆门", 31.0354f, 112.2043f},
    {"孝感", 30.9245f, 113.9169f},   {"荆州", 30.3352f, 112.2397f},
    {"黄冈", 30.4537f, 114.8724f},   {"咸宁", 29.8413f, 114.3225f},
    {"随州", 31.6903f, 113.3828f},   {"长沙", 28.2282f, 112.9388f},
    {"株洲", 27.8274f, 113.1341f},   {"湘潭", 27.8296f, 112.9445f},
    {"衡阳", 26.8938f, 112.5719f},   {"邵阳", 27.2389f, 111.4678f},
    {"岳阳", 29.3570f, 113.1289f},   {"常德", 29.0316f, 111.6984f},
    {"张家界", 29.1170f, 110.4790f}, {"益阳", 28.5539f, 112.3551f},
    {"郴州", 25.7706f, 113.0147f},   {"永州", 26.4204f, 111.6139f},
    {"怀化", 27.5698f, 109.9784f},   {"娄底", 27.7000f, 111.9960f},
    {"广州", 23.1291f, 113.2644f},   {"韶关", 24.8107f, 113.5972f},
    {"深圳", 22.5431f, 114.0579f},   {"珠海", 22.2707f, 113.5767f},
    {"汕头", 23.3541f, 116.6820f},   {"佛山", 23.0218f, 113.1219f},
    {"江门", 22.5789f, 113.0819f},   {"湛江", 21.2707f, 110.3594f},
    {"茂名", 21.6627f, 110.9255f},   {"肇庆", 23.0472f, 112.4653f},
    {"惠州", 23.1115f, 114.4152f},   {"梅州", 24.2886f, 116.1225f},
    {"汕尾", 22.7862f, 115.3751f},   {"河源", 23.7437f, 114.7002f},
    {"阳江", 21.8592f, 111.9822f},   {"清远", 23.6817f, 113.0560f},
    {"东莞", 23.0207f, 113.7518f},   {"中山", 22.5170f, 113.3926f},
    {"潮州", 23.6567f, 116.6226f},   {"揭阳", 23.5498f, 116.3728f},
    {"云浮", 22.9152f, 112.0444f},   {"南宁", 22.8170f, 108.3665f},
    {"柳州", 24.3264f, 109.4281f},   {"桂林", 25.2736f, 110.2900f},
    {"梧州", 23.4769f, 111.2791f},   {"北海", 21.4733f, 109.1194f},
    {"防城港", 21.6869f, 108.3549f}, {"钦州", 21.9797f, 108.6246f},
    {"贵港", 23.1114f, 109.5983f},   {"玉林", 22.6540f, 110.1812f},
    {"百色", 23.9022f, 106.6183f},   {"贺州", 24.4038f, 111.5667f},
    {"河池", 24.6929f, 108.0851f},   {"来宾", 23.7500f, 109.2210f},
    {"崇左", 22.3767f, 107.3649f},   {"海口", 20.0440f, 110.1999f},
    {"三亚", 18.2528f, 109.5119f},   {"重庆", 29.5630f, 106.5516f},
    {"成都", 30.5728f, 104.0668f},   {"自贡", 29.3392f, 104.7784f},
    {"攀枝花", 26.5823f, 101.7186f}, {"泸州", 28.8717f, 105.4433f},
    {"德阳", 31.1280f, 104.3980f},   {"绵阳", 31.4675f, 104.6796f},
    {"广元", 32.4355f, 105.8436f},   {"遂宁", 30.5130f, 105.5929f},
    {"内江", 29.5802f, 105.0584f},   {"乐山", 29.5521f, 103.7654f},
    {"南充", 30.8373f, 106.1107f},   {"眉山", 30.0488f, 103.8318f},
    {"宜宾", 28.7602f, 104.6432f},   {"广安", 30.4564f, 106.6334f},
    {"达州", 31.2096f, 107.4682f},   {"雅安", 29.9802f, 103.0133f},
    {"巴中", 31.8671f, 106.7479f},   {"资阳", 30.1289f, 104.6418f},
    {"贵阳", 26.6470f, 106.6302f},   {"六盘水", 26.5927f, 104.8302f},
    {"遵义", 27.7257f, 106.9274f},   {"安顺", 26.2531f, 105.9326f},
    {"毕节", 27.2985f, 105.3050f},   {"铜仁", 27.7255f, 109.1916f},
    {"昆明", 25.0389f, 102.7183f},   {"曲靖", 25.4900f, 103.7962f},
    {"玉溪", 24.3520f, 102.5465f},   {"保山", 25.1120f, 99.1610f},
    {"昭通", 27.3380f, 103.7170f},   {"丽江", 26.8721f, 100.2299f},
    {"普洱", 22.8250f, 100.9660f},   {"临沧", 23.8870f, 100.0920f},
    {"拉萨", 29.6520f, 91.1721f},    {"西安", 34.3416f, 108.9398f},
    {"铜川", 34.8967f, 108.9451f},   {"宝鸡", 34.3619f, 107.2373f},
    {"咸阳", 34.3296f, 108.7093f},   {"渭南", 34.4994f, 109.5100f},
    {"延安", 36.5853f, 109.4898f},   {"汉中", 33.0676f, 107.0238f},
    {"榆林", 38.2852f, 109.7346f},   {"安康", 32.6850f, 109.0290f},
    {"商洛", 33.8680f, 109.9400f},   {"兰州", 36.0611f, 103.8343f},
    {"嘉峪关", 39.7720f, 98.2900f},  {"金昌", 38.5200f, 102.1880f},
    {"白银", 36.5450f, 104.1380f},   {"天水", 34.5809f, 105.7249f},
    {"武威", 37.9280f, 102.6380f},   {"张掖", 38.9250f, 100.4490f},
    {"平凉", 35.5420f, 106.6650f},   {"酒泉", 39.7320f, 98.4940f},
    {"庆阳", 35.7090f, 107.6380f},   {"定西", 35.5790f, 104.6260f},
    {"陇南", 33.4010f, 104.9290f},   {"西宁", 36.6171f, 101.7782f},
    {"银川", 38.4872f, 106.2309f},   {"乌鲁木齐", 43.8256f, 87.6168f},
    {"克拉玛依", 45.5790f, 84.8890f}, {"香港", 22.3193f, 114.1694f},
    {"澳门", 22.1987f, 113.5439f},   {"台北", 25.0330f, 121.5654f},
};

bool ChecksumOk(const char* line) {
    const char* star = strchr(line, '*');
    if (star == nullptr) {
        return true;  // 某些模组关闭校验和输出
    }
    if (star - line < 2 || star[1] == '\0' || star[2] == '\0') {
        return false;
    }
    uint8_t sum = 0;
    for (const char* p = line + 1; p < star; ++p) {
        sum ^= (uint8_t)(*p);
    }
    char hex[3] = {star[1], star[2], '\0'};
    return (uint8_t)strtoul(hex, nullptr, 16) == sum;
}

bool ParseCoordinate(const char* value, const char* hemisphere, double* out) {
    if (value == nullptr || *value == '\0' || hemisphere == nullptr) {
        return false;
    }
    char* end = nullptr;
    double raw = strtod(value, &end);
    if (end == value || end == nullptr || *end != '\0') {
        return false;
    }
    // NaN 与负数都不满足该比较，可一并排除
    if (!(raw >= 0.0) || raw >= 100000.0) {
        return false;
    }
    int degrees = (int)(raw / 100.0);
    double minutes = raw - degrees * 100.0;
    if (minutes < 0.0 || minutes >= 60.0) {
        return false;
    }
    char hemi = hemisphere[0];
    if (hemi != 'N' && hemi != 'S' && hemi != 'E' && hemi != 'W') {
        return false;
    }
    double result = degrees + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W') {
        result = -result;
    }
    if (result < -180.0 || result > 180.0) {
        return false;
    }
    *out = result;
    return true;
}

void UpdateCity(GpsFix* fix) {
    float latitude = (float)fix->latitude;
    float longitude = (float)fix->longitude;
    float cos_lat = cosf(latitude * 3.14159265f / 180.0f);
    float best_distance = 1.0e9f;
    const char* best_name = nullptr;

    for (const auto& city : kCities) {
        float dlat = (latitude - city.latitude) * 111.32f;
        float dlon = (longitude - city.longitude) * 111.32f * cos_lat;
        float distance = sqrtf(dlat * dlat + dlon * dlon);
        if (distance < best_distance) {
            best_distance = distance;
            best_name = city.name;
        }
    }

    if (best_name != nullptr && best_distance <= kMaxCityMatchKm) {
        fix->city = best_name;
        fix->city_distance_km = best_distance;
    } else {
        // 距离任何已知城市都太远时不报城市名，避免给出错误位置
        fix->city.clear();
        fix->city_distance_km = -1.0f;
    }
}

}  // namespace

GpsReceiver& GpsReceiver::GetInstance() {
    static GpsReceiver instance;
    return instance;
}

esp_err_t GpsReceiver::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return ESP_OK;
    }

#if defined(CONFIG_GOOUUU_GPS_ENABLE) && CONFIG_GOOUUU_GPS_ENABLE
    uart_config_t config = {
        .baud_rate = GPS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install((uart_port_t)GPS_UART_NUM, 2048, 0, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d install failed: %s", GPS_UART_NUM, esp_err_to_name(err));
        return err;
    }
    // GPS 是可选外设，配置失败只降级为“无 GPS”，不能让整个设备 abort
    err = uart_param_config((uart_port_t)GPS_UART_NUM, &config);
    if (err == ESP_OK) {
        err = uart_set_pin((uart_port_t)GPS_UART_NUM, GPS_TX_PIN, GPS_RX_PIN, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d config failed: %s", GPS_UART_NUM, esp_err_to_name(err));
        uart_driver_delete((uart_port_t)GPS_UART_NUM);
        return err;
    }

    if (xTaskCreate([](void* arg) {
            static_cast<GpsReceiver*>(arg)->Task();
            vTaskDelete(nullptr);
        }, "gps_rx", 4096, this, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GPS task");
        uart_driver_delete((uart_port_t)GPS_UART_NUM);
        return ESP_ERR_NO_MEM;
    }

    started_ = true;
    fix_.started = true;

    ESP_LOGI(TAG, "GPS ready: UART%d TX=GPIO%d RX=GPIO%d %d baud", GPS_UART_NUM, GPS_TX_PIN,
             GPS_RX_PIN, GPS_BAUD_RATE);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "GPS disabled at build time; use goouuu-s3-dual-gps build (console on USB-Serial-JTAG)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool GpsReceiver::IsStarted() {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_;
}

GpsFix GpsReceiver::GetFix() {
    std::lock_guard<std::mutex> lock(mutex_);
    // 失锁或长时间没有新定位时，不能继续把旧坐标当成有效位置
    if (fix_.valid && fix_.fix_us != 0 &&
        esp_timer_get_time() - fix_.fix_us > kFixStaleTimeoutUs) {
        fix_.valid = false;
    }
    return fix_;
}

void GpsReceiver::Task() {
    char line[192];
    size_t length = 0;
    bool discarding = false;
    uint8_t chunk[64];

    while (true) {
        int received = uart_read_bytes((uart_port_t)GPS_UART_NUM, chunk, sizeof(chunk),
                                       pdMS_TO_TICKS(200));
        for (int i = 0; i < received; ++i) {
            char c = (char)chunk[i];
            if (c == '\n') {
                line[length] = '\0';
                if (length > 0 && line[length - 1] == '\r') {
                    line[length - 1] = '\0';
                }
                if (!discarding && line[0] == '$') {
                    HandleLine(line);
                }
                length = 0;
                discarding = false;
            } else if (discarding) {
                continue;
            } else if (length < sizeof(line) - 1) {
                line[length++] = c;
            } else {
                // 超长行必须一直丢弃到换行，否则残片会被当成新语句解析
                discarding = true;
                length = 0;
            }
        }
    }
}

void GpsReceiver::HandleLine(char* line) {
    if (!ChecksumOk(line)) {
        return;
    }

    char* fields[24];
    int count = 0;
    char* cursor = line;
    while (count < 24 && cursor != nullptr) {
        fields[count++] = cursor;
        char* comma = strchr(cursor, ',');
        if (comma == nullptr) {
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
    }
    if (count < 2) {
        return;
    }

    bool is_gga = (strcmp(fields[0], "$GNGGA") == 0 || strcmp(fields[0], "$GPGGA") == 0);
    bool is_rmc = (strcmp(fields[0], "$GNRMC") == 0 || strcmp(fields[0], "$GPRMC") == 0);

    if (is_gga && count >= 10) {
        int quality = atoi(fields[6]);
        double latitude = 0.0;
        double longitude = 0.0;
        if (quality > 0 && ParseCoordinate(fields[2], fields[3], &latitude) &&
            ParseCoordinate(fields[4], fields[5], &longitude)) {
            std::lock_guard<std::mutex> lock(mutex_);
            fix_.valid = true;
            fix_.latitude = latitude;
            fix_.longitude = longitude;
            fix_.fix_quality = quality;
            fix_.satellites = atoi(fields[7]);
            fix_.hdop = atof(fields[8]);
            fix_.altitude_m = atof(fields[9]);
            fix_.fix_us = esp_timer_get_time();
            UpdateCity(&fix_);
        } else {
            // quality=0 表示未定位，立即作废旧坐标
            std::lock_guard<std::mutex> lock(mutex_);
            fix_.valid = false;
            fix_.fix_quality = quality;
        }
    } else if (is_rmc && count >= 9) {
        if (fields[2][0] == 'A') {
            double latitude = 0.0;
            double longitude = 0.0;
            if (ParseCoordinate(fields[3], fields[4], &latitude) &&
                ParseCoordinate(fields[5], fields[6], &longitude)) {
                std::lock_guard<std::mutex> lock(mutex_);
                fix_.valid = true;
                fix_.latitude = latitude;
                fix_.longitude = longitude;
                fix_.speed_kmh = (float)atof(fields[7]) * 1.852f;
                fix_.course_deg = atof(fields[8]);
                fix_.fix_us = esp_timer_get_time();
                UpdateCity(&fix_);
            }
        } else {
            // status='V' 表示定位无效
            std::lock_guard<std::mutex> lock(mutex_);
            fix_.valid = false;
        }
    }
}
