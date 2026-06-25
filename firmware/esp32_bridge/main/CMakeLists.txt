set(main_requires
    nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer esp_app_format
    esp_driver_uart esp_driver_i2c esp_driver_gpio esp_driver_spi esp_driver_ledc
    esp_lcd lvgl led_strip bme680
)

idf_component_register(
    SRCS "main.c" "wifi_mgr.c" "led_status.c" "web.c" "uart_link.c" "mqtt_ha.c" "bme280.c" "pins.c" "ld2410.c" "rules.c" "ac_state.c" "ac_cmd.c" "board_config.c" "bridge_buttons.c" "bridge_state.c" "command_queue.c" "ui_lvgl.c"
    INCLUDE_DIRS "."
    EMBED_FILES "www/index.html"
    REQUIRES ${main_requires}
)
