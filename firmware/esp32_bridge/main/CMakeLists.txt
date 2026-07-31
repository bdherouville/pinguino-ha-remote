idf_component_register(
    SRCS "main.c" "wifi_mgr.c" "led_status.c" "web.c" "ble_emu.c" "mqtt_ha.c" "bme280.c" "pins.c" "ld2410.c" "rules.c" "ac_state.c" "ac_cmd.c"
    INCLUDE_DIRS "."
    EMBED_FILES "www/index.html"
    # "bt" brings in NimBLE for ble_emu.c (the Ganymede remote emulator). esp_driver_uart is
    # still needed for the LD2410 presence sensor on UART2 — the nRF link that used UART1 is gone.
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer esp_driver_uart esp_driver_i2c esp_driver_gpio bt led_strip
)
