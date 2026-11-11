#include "wifi_portal.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "pin_store.h"
#include "web_auth.h"
#include "dev_cfg.h"
#include "bt_hid_kbd.h"

static const char *TAG = "PORTAL";

#define SSID_PREFIX         "ESP32-Unlock-"
#define AP_IP_STR           "192.168.4.1"
#define DNS_TASK_STACK      4096
#define DNS_TASK_PRIO       4
#define SESSION_LEN         32
#define PORTAL_IDLE_CLOSE_MS       (60 * 1000)      /* 一直没人连：1 分钟自动关 */
#define PORTAL_JOIN_CLOSE_MS       (5 * 60 * 1000)  /* 有人连上后：最多再开 5 分钟 */
#define PORTAL_WATCH_MS            1000             /* 每秒查一次有没有设备连着 */

/* 网页文件由 CMakeLists 的 EMBED_FILES 打进固件（符号名取文件名） */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static bool s_inited;
static bool s_running;
static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;
static volatile bool s_dns_stop;
static char s_session[SESSION_LEN + 1];
static TaskHandle_t s_watch_task;      /* 热点自动关闭任务 */

/* ---------------- 小工具 ---------------- */

static void new_session(void)
{
    uint32_t rnd[4];
    for (int i = 0; i < 4; i++) {
        rnd[i] = esp_random();
    }
    for (int i = 0; i < SESSION_LEN; i++) {
        uint8_t nib = (uint8_t)((rnd[i / 8] >> ((i % 8) * 4)) & 0xF);
        s_session[i] = "0123456789abcdef"[nib];
    }
    s_session[SESSION_LEN] = '\0';
}

static esp_err_t reply_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/* 读完整请求体（x-www-form-urlencoded），失败返回 -1 */
static int read_body(httpd_req_t *req, char *buf, size_t len)
{
    if (req->content_len == 0 || req->content_len >= len) {
        return -1;
    }
    int total = 0;
    while (total < req->content_len) {
        int r = httpd_req_recv(req, buf + total, req->content_len - total);
        if (r <= 0) {
            return -1;
        }
        total += r;
    }
    buf[total] = '\0';
    return total;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* 从 a=1&b=2 里取出某个字段并做 URL 解码（电脑密码可能含 + % & = 等字符） */
static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;

    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t n = 0;
            while (*p != '\0' && *p != '&') {
                char c = *p++;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%' && hex_val(p[0]) >= 0 && hex_val(p[1]) >= 0) {
                    c = (char)((hex_val(p[0]) << 4) | hex_val(p[1]));
                    p += 2;
                }
                if (n + 1 < out_len) {
                    out[n++] = c;
                }
            }
            out[n] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p != NULL) {
            p++;
        }
    }
    return false;
}

/* JSON 字符串转义（密码里可能有 " 和 \） */
static void json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t n = 0;
    for (; *src != '\0' && n + 2 < dst_len; src++) {
        if (*src == '"' || *src == '\\') {
            dst[n++] = '\\';
            dst[n++] = *src;
        } else {
            dst[n++] = *src;
        }
    }
    dst[n] = '\0';
}

static bool is_authed(httpd_req_t *req)
{
    if (s_session[0] == '\0') {
        return false;
    }
    char cookie[128];
    if (httpd_req_get_hdr_value_len(req, "Cookie") == 0) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }
    const char *p = strstr(cookie, "sid=");
    if (p == NULL) {
        return false;
    }
    p += 4;
    size_t n = strcspn(p, "; ");
    return n == SESSION_LEN && memcmp(p, s_session, SESSION_LEN) == 0;
}

/* ---------------- HTTP 处理 ---------------- */

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t len = (size_t)(index_html_end - index_html_start);
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t login_post(httpd_req_t *req)
{
    char body[128];
    char pwd[64];

    if (read_body(req, body, sizeof(body)) < 0 ||
        !form_get(body, "password", pwd, sizeof(pwd))) {
        return reply_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"请求格式错误\"}");
    }

    if (!web_auth_check(pwd)) {
        ESP_LOGW(TAG, "网页登录失败：密码不对");
        vTaskDelay(pdMS_TO_TICKS(400));       /* 简单限速，防连点试错 */
        return reply_json(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"密码不正确\"}");
    }

    new_session();
    char cookie[80];
    snprintf(cookie, sizeof(cookie),
             "sid=%s; Path=/; Max-Age=3600; HttpOnly", s_session);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);

    bool must_change = web_auth_must_change();
    ESP_LOGI(TAG, "网页登录成功%s", must_change ? "，需要先修改默认密码" : "");
    return reply_json(req, "200 OK",
                      must_change ? "{\"ok\":true,\"must_change\":true}"
                                  : "{\"ok\":true,\"must_change\":false}");
}

/* 已登录且完成首次改密，才允许读写配置 */
static esp_err_t require_ready(httpd_req_t *req)
{
    if (!is_authed(req)) {
        return reply_json(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"未登录\"}");
    }
    if (web_auth_must_change()) {
        return reply_json(req, "403 Forbidden",
                          "{\"ok\":false,\"must_change\":true,\"error\":\"请先修改网页登录密码\"}");
    }
    return ESP_OK;
}

/* 首次登录强制改密；之后也可以用它改网页密码 */
static esp_err_t webpass_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        return reply_json(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"未登录\"}");
    }

    char body[96];
    char pwd[WEB_PWD_MAX_LEN + 1];

    if (read_body(req, body, sizeof(body)) < 0 ||
        !form_get(body, "password", pwd, sizeof(pwd))) {
        return reply_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"请求格式错误\"}");
    }

    esp_err_t err = web_auth_set(pwd);
    if (err == ESP_ERR_INVALID_ARG) {
        return reply_json(req, "400 Bad Request",
                          "{\"ok\":false,\"error\":\"网页密码必须是 4-16 位纯数字\"}");
    }
    if (err != ESP_OK) {
        return reply_json(req, "500 Internal Server Error",
                          "{\"ok\":false,\"error\":\"保存失败\"}");
    }

    return reply_json(req, "200 OK", "{\"ok\":true,\"must_change\":false}");
}

static esp_err_t cred_get(httpd_req_t *req)
{
    esp_err_t chk = require_ready(req);
    if (chk != ESP_OK) {
        return chk;
    }

    char pin[PIN_MAX_LEN + 1];
    char pwd[PWD_MAX_LEN + 1];
    char esc[PWD_MAX_LEN * 2 + 2];
    pin_store_get_pin(pin, sizeof(pin));
    pin_store_get_password(pwd, sizeof(pwd));
    json_escape(pwd, esc, sizeof(esc));

    char json[220];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"mode\":%d,\"pin\":\"%s\",\"password\":\"%s\"}",
             (int)pin_store_get_mode(), pin, esc);
    return reply_json(req, "200 OK", json);
}

/* 保存电脑 PIN / 电脑密码，并指定用哪一种解锁。
 * 表单字段：pin、password、mode（1=PIN，2=密码）；空串表示清空该项。 */
static esp_err_t cred_post(httpd_req_t *req)
{
    esp_err_t chk = require_ready(req);
    if (chk != ESP_OK) {
        return chk;
    }

    char body[320];
    char pin[PIN_MAX_LEN + 1] = {0};
    char pwd[PWD_MAX_LEN + 1] = {0};
    char mode_s[8] = {0};

    if (read_body(req, body, sizeof(body)) < 0) {
        return reply_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"请求格式错误\"}");
    }
    form_get(body, "pin", pin, sizeof(pin));
    form_get(body, "password", pwd, sizeof(pwd));
    if (!form_get(body, "mode", mode_s, sizeof(mode_s))) {
        return reply_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"请选择解锁方式\"}");
    }

    int mode = atoi(mode_s);
    if (pin[0] != '\0' && !pin_store_pin_valid(pin)) {
        return reply_json(req, "400 Bad Request",
                          "{\"ok\":false,\"error\":\"电脑 PIN 必须是 4-16 位纯数字\"}");
    }
    if (pwd[0] != '\0' && !pin_store_pwd_valid(pwd)) {
        return reply_json(req, "400 Bad Request",
                          "{\"ok\":false,\"error\":\"电脑密码只能用 1-32 位英文、数字或符号（不支持中文）\"}");
    }
    if (mode == UNLOCK_MODE_PIN && pin[0] == '\0') {
        return reply_json(req, "400 Bad Request",
                          "{\"ok\":false,\"error\":\"选择了 PIN 解锁，必须填写电脑 PIN\"}");
    }
    if (mode == UNLOCK_MODE_PASSWORD && pwd[0] == '\0') {
        return reply_json(req, "400 Bad Request",
                          "{\"ok\":false,\"error\":\"选择了密码解锁，必须填写电脑密码\"}");
    }
    if (mode != UNLOCK_MODE_PIN && mode != UNLOCK_MODE_PASSWORD) {
        return reply_json(req, "400 Bad Request",
                          "{\"ok\":false,\"error\":\"请先设置电脑 PIN 或电脑密码，并选择其中一种解锁方式\"}");
    }

    esp_err_t err = pin_store_set_credentials(pin, pwd, (unlock_mode_t)mode);
    if (err != ESP_OK) {
        return reply_json(req, "500 Internal Server Error",
                          "{\"ok\":false,\"error\":\"保存失败\"}");
    }

    if (mode == UNLOCK_MODE_PIN) {
        ESP_LOGI(TAG, "解锁方式已设为电脑 PIN（%u 位）", (unsigned)strlen(pin));
    } else {
        ESP_LOGI(TAG, "解锁方式已设为电脑密码（%u 位）", (unsigned)strlen(pwd));
    }
    return reply_json(req, "200 OK", "{\"ok\":true}");
}

/* 清空电脑 PIN、电脑密码和解锁方式，回到“未设置”的初始状态 */
static esp_err_t cred_clear_post(httpd_req_t *req)
{
    esp_err_t chk = require_ready(req);
    if (chk != ESP_OK) {
        return chk;
    }

    esp_err_t err = pin_store_set_credentials("", "", UNLOCK_MODE_NONE);
    if (err != ESP_OK) {
        return reply_json(req, "500 Internal Server Error",
                          "{\"ok\":false,\"error\":\"清空失败\"}");
    }

    ESP_LOGI(TAG, "电脑凭据已清空，短按 BOOT 将不发送任何按键");
    return reply_json(req, "200 OK", "{\"ok\":true}");
}

/* ---------------- 热点自动关闭 ---------------- */

/* 当前是否有设备（手机 / 电脑）连在热点上 */
static bool ap_has_client(void)
{
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        return false;
    }
    return list.num > 0;
}

/* 常驻任务：热点开着时每秒看一眼
 *   一直没人连 → 开满 1 分钟就关；
 *   有人连上过 → 从连上那一刻起最多再开 5 分钟。 */
static void portal_watch_task(void *arg)
{
    (void)arg;

    bool was_running = false;
    bool joined = false;
    TickType_t started = 0;
    TickType_t joined_at = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(PORTAL_WATCH_MS));

        if (!s_running) {
            was_running = false;
            joined = false;
            continue;
        }

        if (!was_running) {
            /* 热点刚打开，重新计时 */
            was_running = true;
            started = xTaskGetTickCount();
            joined = false;
        }

        if (!joined && ap_has_client()) {
            joined = true;
            joined_at = xTaskGetTickCount();
            ESP_LOGI(TAG, "检测到设备连上热点，%d 分钟后自动关闭",
                     PORTAL_JOIN_CLOSE_MS / 60000);
        }

        TickType_t now = xTaskGetTickCount();
        if (joined) {
            if (now - joined_at >= pdMS_TO_TICKS(PORTAL_JOIN_CLOSE_MS)) {
                ESP_LOGI(TAG, "热点已开满 %d 分钟，自动关闭", PORTAL_JOIN_CLOSE_MS / 60000);
                wifi_portal_stop();
            }
        } else if (now - started >= pdMS_TO_TICKS(PORTAL_IDLE_CLOSE_MS)) {
            ESP_LOGI(TAG, "%d 秒内没有设备连接，自动关闭热点", PORTAL_IDLE_CLOSE_MS / 1000);
            wifi_portal_stop();
        }
    }
}

/* ---------------- 通路开关 / 蓝牙配对 ---------------- */

static esp_err_t status_get(httpd_req_t *req)
{
    esp_err_t chk = require_ready(req);
    if (chk != ESP_OK) {
        return chk;
    }

    bt_kbd_status_t bt;
    bt_kbd_get_status(&bt);

    char json[256];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"mode\":%d,\"has_pin\":%s,\"has_pwd\":%s,\"usb\":%s,\"ble\":%s,"
             "\"ble_connected\":%s,\"ble_advertising\":%s,\"ble_address\":\"%s\"}",
             (int)pin_store_get_mode(),
             pin_store_has_pin() ? "true" : "false",
             pin_store_has_password() ? "true" : "false",
             dev_cfg_usb_enabled() ? "true" : "false",
             dev_cfg_ble_enabled() ? "true" : "false",
             bt.connected ? "true" : "false",
             bt.advertising ? "true" : "false",
             bt.address);
    return reply_json(req, "200 OK", json);
}

static esp_err_t route_post(httpd_req_t *req)
{
    esp_err_t chk = require_ready(req);
    if (chk != ESP_OK) {
        return chk;
    }

    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) {
        return reply_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"请求格式错误\"}");
    }

    bool usb = dev_cfg_usb_enabled();
    bool ble = dev_cfg_ble_enabled();
    char val[8];

    if (form_get(body, "usb", val, sizeof(val))) {
        usb = (val[0] == '1');
    }
    if (form_get(body, "ble", val, sizeof(val))) {
        ble = (val[0] == '1');
    }

    if (dev_cfg_set_usb(usb) != ESP_OK || dev_cfg_set_ble(ble) != ESP_OK) {
        return reply_json(req, "500 Internal Server Error",
                          "{\"ok\":false,\"error\":\"保存失败\"}");
    }

    /* 蓝牙开关立即生效：关 = 停广播 + 断开；开 = 重新广播 */
    bt_kbd_set_enabled(ble);

    ESP_LOGI(TAG, "网页设置通路：USB %s，蓝牙 %s",
             usb ? "开" : "关", ble ? "开" : "关");
    return reply_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t forget_post(httpd_req_t *req)
{
    esp_err_t chk = require_ready(req);
    if (chk != ESP_OK) {
        return chk;
    }

    bt_kbd_forget_all();
    ESP_LOGI(TAG, "网页操作：已清除全部蓝牙配对");
    return reply_json(req, "200 OK", "{\"ok\":true}");
}

/* ---------------- 强制门户 DNS（UDP 53，全部指向 192.168.4.1） ---------------- */

static void dns_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket 创建失败");
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS 绑定 53 端口失败");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "强制门户 DNS 已启动");

    char rx[512];
    char tx[512];

    while (!s_dns_stop) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &from_len);
        if (len <= 12) {
            continue;
        }

        /* 跳过问题段（域名标签 + QTYPE/QCLASS），保留原样回显 */
        int q_end = 12;
        while (q_end < len) {
            if (rx[q_end] == 0) {
                q_end += 1;
                break;
            }
            q_end += rx[q_end] + 1;
        }
        q_end += 4;
        if (q_end > len) {
            continue;
        }

        memcpy(tx, rx, q_end);
        tx[2] = (char)0x81;              /* 响应 + RD */
        tx[3] = (char)0x80;              /* RA，RCODE=0 */
        tx[6] = 0; tx[7] = 1;            /* ANCOUNT = 1 */
        tx[8] = 0; tx[9] = 0;            /* NSCOUNT */
        tx[10] = 0; tx[11] = 0;          /* ARCOUNT */

        int p = q_end;
        tx[p++] = (char)0xC0; tx[p++] = 0x0C;   /* 名字指向偏移 12 */
        tx[p++] = 0x00; tx[p++] = 0x01;         /* TYPE A */
        tx[p++] = 0x00; tx[p++] = 0x01;         /* CLASS IN */
        tx[p++] = 0x00; tx[p++] = 0x00; tx[p++] = 0x00; tx[p++] = 0x0A;  /* TTL 10s */
        tx[p++] = 0x00; tx[p++] = 0x04;         /* RDLENGTH */
        tx[p++] = 192; tx[p++] = 168; tx[p++] = 4; tx[p++] = 1;

        sendto(sock, tx, p, 0, (struct sockaddr *)&from, from_len);
    }

    close(sock);
    ESP_LOGI(TAG, "强制门户 DNS 已停止");
    vTaskDelete(NULL);
}

/* ---------------- 启停 ---------------- */

static esp_err_t portal_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        return ESP_FAIL;
    }

    s_inited = true;
    return ESP_OK;
}

esp_err_t wifi_portal_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    esp_err_t err = portal_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "热点初始化失败：%s", esp_err_to_name(err));
        return err;
    }

    s_session[0] = '\0';

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    char ssid[33];
    snprintf(ssid, sizeof(ssid), SSID_PREFIX "%02X%02X", mac[4], mac[5]);

    wifi_config_t wcfg = {0};
    memcpy(wcfg.ap.ssid, ssid, strlen(ssid));
    wcfg.ap.ssid_len = (uint8_t)strlen(ssid);
    wcfg.ap.channel = 1;
    wcfg.ap.max_connection = 2;
    wcfg.ap.authmode = WIFI_AUTH_OPEN;       /* 开放热点，不需要 WiFi 密码 */
    wcfg.ap.password[0] = '\0';
    wcfg.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.server_port = 80;
    hcfg.max_uri_handlers = 10;
    hcfg.lru_purge_enable = true;
    hcfg.uri_match_fn = httpd_uri_match_wildcard;   /* 允许 URI 末尾带通配符 */

    err = httpd_start(&s_server, &hcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败：%s", esp_err_to_name(err));
        esp_wifi_stop();
        s_server = NULL;
        return err;
    }

    /* 具体接口先注册，最后注册的兜底处理器负责强制门户探测 */
    static const httpd_uri_t uri_login = {
        .uri = "/api/login", .method = HTTP_POST, .handler = login_post,
    };
    static const httpd_uri_t uri_webpass = {
        .uri = "/api/webpass", .method = HTTP_POST, .handler = webpass_post,
    };
    static const httpd_uri_t uri_cred_get = {
        .uri = "/api/cred", .method = HTTP_GET, .handler = cred_get,
    };
    static const httpd_uri_t uri_cred_post = {
        .uri = "/api/cred", .method = HTTP_POST, .handler = cred_post,
    };
    static const httpd_uri_t uri_cred_clear = {
        .uri = "/api/cred/clear", .method = HTTP_POST, .handler = cred_clear_post,
    };
    static const httpd_uri_t uri_status = {
        .uri = "/api/status", .method = HTTP_GET, .handler = status_get,
    };
    static const httpd_uri_t uri_route = {
        .uri = "/api/route", .method = HTTP_POST, .handler = route_post,
    };
    static const httpd_uri_t uri_forget = {
        .uri = "/api/bt/forget", .method = HTTP_POST, .handler = forget_post,
    };
    static const httpd_uri_t uri_catchall = {
        .uri = "/*", .method = HTTP_GET, .handler = index_get,
    };

    httpd_register_uri_handler(s_server, &uri_login);
    httpd_register_uri_handler(s_server, &uri_webpass);
    httpd_register_uri_handler(s_server, &uri_cred_get);
    httpd_register_uri_handler(s_server, &uri_cred_post);
    httpd_register_uri_handler(s_server, &uri_cred_clear);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_route);
    httpd_register_uri_handler(s_server, &uri_forget);
    httpd_register_uri_handler(s_server, &uri_catchall);

    s_dns_stop = false;
    if (xTaskCreate(dns_task, "portal_dns", DNS_TASK_STACK, NULL, DNS_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGW(TAG, "DNS 任务创建失败，手机需手动打开 http://%s/", AP_IP_STR);
    }

    if (s_watch_task == NULL) {
        if (xTaskCreate(portal_watch_task, "portal_watch", 3072, NULL, 3, &s_watch_task) != pdPASS) {
            s_watch_task = NULL;
            ESP_LOGW(TAG, "自动关闭任务创建失败，热点不会自动关闭");
        }
    }

    s_running = true;
    ESP_LOGI(TAG, "配置热点已开启：WiFi = %s（开放），网页 = http://%s/ ，网页密码%s",
             ssid, AP_IP_STR,
             web_auth_must_change() ? "为出厂默认 0000（首次登录必须修改）" : "已设置");
    return ESP_OK;
}

esp_err_t wifi_portal_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }

    s_running = false;
    s_dns_stop = true;

    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    vTaskDelay(pdMS_TO_TICKS(1200));       /* 等 DNS 任务从 recvfrom 超时退出 */
    esp_wifi_stop();

    s_session[0] = '\0';
    ESP_LOGI(TAG, "配置热点已关闭");
    return ESP_OK;
}

bool wifi_portal_is_running(void)
{
    return s_running;
}
