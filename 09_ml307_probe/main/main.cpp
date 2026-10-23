#include <algorithm>
#include <memory>
#include <string>

#include "at_modem.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ML307";

static const char *network_status_name(NetworkStatus status)
{
    switch (status) {
    case NetworkStatus::Ready:
        return "ready";
    case NetworkStatus::ErrorInsertPin:
        return "SIM missing or PIN error";
    case NetworkStatus::ErrorRegistrationDenied:
        return "registration denied";
    case NetworkStatus::ErrorTimeout:
        return "attach timeout";
    default:
        return "error";
    }
}

/* Every AT query is printed raw. Without a SIM most of them return an error
 * code, and that error code is the evidence that the module is talking. */
static void query(const std::shared_ptr<AtUart> &uart, const char *command)
{
    if (auto result = uart->SendCommand(command, 1500); result) {
        ESP_LOGI(TAG, "%-9s OK   %s", command, uart->GetResponse().c_str());
    } else {
        ESP_LOGW(TAG, "%-9s FAIL %s", command, result.error().ToString().c_str());
    }
}

struct PinOrder {
    gpio_num_t tx;
    gpio_num_t rx;
    const char *label;
};

/* The measured order comes first; the other one stays as a cheap fallback in
 * case the jumper leads are ever re-seated the other way round. */
static const PinOrder kOrders[] = {
    { PIN_ML307_TX, PIN_ML307_RX, "measured   TX=GPIO12 RX=GPIO11" },
    { PIN_ML307_RX, PIN_ML307_TX, "swapped    TX=GPIO11 RX=GPIO12" },
};

/* Only the last 4 digits are logged; ICCID/IMEI are subscriber identifiers. */
static std::string tail4(const std::string &value)
{
    if (value.size() <= 4) {
        return value;
    }
    return "****" + value.substr(value.size() - 4);
}

/* A real end-to-end check: if this returns 200/30x the PDP context is up and
 * the module can actually move data, not just register on the network. */
static void test_http(AtModem &modem)
{
    auto http = modem.CreateHttp();
    http->SetTimeout(20000);
    http->SetHeader("User-Agent", "ml307-probe");

    const char *url = "http://www.baidu.com/";
    ESP_LOGI(TAG, "HTTP GET        : %s", url);
    if (auto opened = http->Open("GET", url); !opened) {
        ESP_LOGE(TAG, "HTTP open       : FAIL %s", opened.error().ToString().c_str());
        return;
    }

    if (auto code = http->GetStatusCode(); code) {
        ESP_LOGI(TAG, "HTTP status     : %d", *code);
    } else {
        ESP_LOGE(TAG, "HTTP status     : FAIL %s", code.error().ToString().c_str());
    }

    std::string body = http->ReadAll();
    ESP_LOGI(TAG, "HTTP body bytes : %d", static_cast<int>(body.size()));
    if (!body.empty()) {
        std::string head = body.substr(0, std::min<size_t>(body.size(), 120));
        ESP_LOGI(TAG, "HTTP head       : %s", head.c_str());
    }
    http->Close();
}

extern "C" void app_main(void)
{
    std::unique_ptr<AtModem> modem;

    for (const auto &order : kOrders) {
        ESP_LOGI(TAG, "probing with %s", order.label);
        auto detected = AtModem::Detect(order.tx, order.rx, GPIO_NUM_NC,
                                        ML307_BAUD, ML307_DETECT_TIMEOUT_MS);
        if (detected) {
            modem = std::move(*detected);
            ESP_LOGI(TAG, "answered on %s", order.label);
            break;
        }
        ESP_LOGW(TAG, "no answer: %s", detected.error().ToString().c_str());
    }

    if (!modem) {
        ESP_LOGE(TAG, "both pin orders failed - nothing is driving the UART");
        ESP_LOGE(TAG, "check: module seated, 5V on VCC, EN tied to 5VIN, GND common");
        return;
    }

    auto uart = modem->GetAtUart();

    /* Dump every AT line the module sends. Needed to tell a module reboot
     * (+MATREADY / RDY) apart from a network-side detach (+CEREG: 0). */
    uart->SetDebug(true);

    ESP_LOGI(TAG, "module revision : %s", modem->GetModuleRevision().c_str());
    ESP_LOGI(TAG, "active baud rate: %d", uart->GetBaudRate());
    ESP_LOGI(TAG, "IMEI            : %s", tail4(modem->GetImei()).c_str());

    query(uart, "AT+CPIN?");
    query(uart, "AT+MCCID");
    query(uart, "AT+CSQ");
    query(uart, "AT+CEREG?");
    query(uart, "AT+COPS?");

    NetworkStatus status = modem->WaitForNetworkReady(ML307_ATTACH_TIMEOUT_MS);
    if (status == NetworkStatus::Ready) {
        ESP_LOGI(TAG, "network attach  : ready");
        ESP_LOGI(TAG, "carrier         : %s", modem->GetCarrierName().c_str());
        ESP_LOGI(TAG, "registration    : %s", modem->GetRegistrationState().ToString().c_str());
        ESP_LOGI(TAG, "CSQ             : %d", modem->GetCsq());
        ESP_LOGI(TAG, "ICCID           : %s", tail4(modem->GetIccid()).c_str());
        test_http(*modem);
    } else {
        ESP_LOGE(TAG, "network attach  : %s", network_status_name(status));
        ESP_LOGE(TAG, "no data test - check SIM type / card lock / APN");
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        ESP_LOGI(TAG, "alive  pin_ready=%d  network_ready=%d  CSQ=%d",
                 modem->pin_ready(), modem->network_ready(), modem->GetCsq());
    }
}
