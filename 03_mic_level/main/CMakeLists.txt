idf_component_register(SRCS "main.c" "mic_level.c" "ws2812.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver
                       PRIV_REQUIRES esp_driver_rmt esp_driver_i2s)
