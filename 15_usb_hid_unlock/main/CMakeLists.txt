idf_component_register(
    SRCS "usb_hid_unlock.c" "pin_store.c" "web_auth.c" "wifi_portal.c" "bt_hid_kbd.c" "dev_cfg.c"
    INCLUDE_DIRS "."
    # 配置网页，符号为 _binary_index_html_start / _binary_index_html_end
    EMBED_FILES "www/index.html"
    PRIV_REQUIRES esp_driver_gpio
    REQUIRES esp_wifi esp_http_server esp_netif nvs_flash esp_event lwip esp_hid bt
)
