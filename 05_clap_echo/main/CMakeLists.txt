idf_component_register(SRCS "main.c" "mic_level.c" "ws2812.c" "imu.c" "audio_out.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver
                       PRIV_REQUIRES esp_driver_rmt esp_driver_i2s esp_driver_i2c esp_timer)
