#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Minimal WS2812 (addressable RGB) driver built on the RMT peripheral.
 * No external component dependency - uses only the ESP-IDF RMT driver. */

esp_err_t ws2812_init(int gpio_num);
void ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b);
