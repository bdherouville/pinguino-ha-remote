idf_component_register(
    SRCS "main.c" "wifi_mgr.c" "led_status.c" "web.c" "uart_link.c" "mqtt_ha.c" "bme280.c" "pins.c" "ld2410.c" "rules.c"
    INCLUDE_DIRS "."
    EMBED_FILES "www/index.html"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer esp_driver_uart esp_driver_i2c esp_driver_gpio led_strip
)
