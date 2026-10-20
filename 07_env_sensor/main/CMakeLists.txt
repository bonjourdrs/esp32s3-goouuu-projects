idf_component_register(
    SRCS
        "main.c"
        "drivers/i2c_bus.c"
        "drivers/aht20.c"
        "drivers/bmp280.c"
        "drivers/ws2812.c"
    INCLUDE_DIRS
        "."
        "config"
        "drivers"
    REQUIRES driver
    PRIV_REQUIRES esp_driver_i2c esp_driver_rmt
)
