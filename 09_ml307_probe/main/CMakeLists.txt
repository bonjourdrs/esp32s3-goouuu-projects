idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS
        "."
        "config"
    REQUIRES
        78__esp-ml307
    PRIV_REQUIRES
        esp_driver_uart
        esp_driver_gpio
        esp_pm
)
