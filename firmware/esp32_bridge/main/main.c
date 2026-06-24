/*
 * Ganymede bridge — ESP32 Wi-Fi front-end.
 *  - Wi-Fi STA with a provisioning AP fallback (scan + connect from the web UI).
 *  - RGB status LED (WS2812 GPIO48).
 *  - Web UI showing the De'Longhi remote with clickable buttons -> UART -> nRF emulator.
 *  - Backends (Home Assistant / MQTT) come later.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "pins.h"
#include "led_status.h"
#include "uart_link.h"
#include "wifi_mgr.h"
#include "web.h"
#include "mqtt_ha.h"
#include "bme280.h"
#include "ld2410.h"
#include "rules.h"
#include "ac_state.h"
#include "ac_cmd.h"
#include "board_config.h"
#include "bridge_state.h"
#include "command_queue.h"
#include "ui_lvgl.h"

static const char *TAG = "bridge";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    pins_load();      // resolve configurable GPIOs from NVS before any driver uses them
    bridge_state_init();
    ac_state_init();  // open-loop AC model (load last state from NVS) — before any press
    led_status_init();
    uart_link_init();
    bridge_command_queue_init();
    wifi_mgr_init();
    web_start();
    ac_cmd_init();    // worker that turns HA targets into paced press sequences
    mqtt_ha_init();   // starts the MQTT client (no-op if no broker configured); auto-connects once STA is up
#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
    ui_lvgl_start();   // display/touch shell; failures are logged and do not stop network/API
#endif
    bme280_init();    // headless: BME280, touchscreen: BME680 -> UART env + HA sensors
    ld2410_init();    // LD2410 presence radar on UART2 (pins from config)
    rules_load();     // presence automation: load rules + start the evaluator task

    ESP_LOGI(TAG, "Ganymede bridge up (%s) — AP '%s' / web on :80", BOARD_NAME, wifi_mgr_ap_ssid());
}
