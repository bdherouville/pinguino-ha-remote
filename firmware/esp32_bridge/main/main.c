/*
 * Ganymede bridge — single-board ESP32-S3: Wi-Fi front-end + BLE remote emulator.
 *  - Wi-Fi STA with a provisioning AP fallback (scan + connect from the web UI).
 *  - Status LED (kind and pin per chip; see board.h).
 *  - Web UI showing the De'Longhi remote with clickable buttons.
 *  - The Ganymede BLE remote emulator itself, on this chip's own radio (ble_emu.c) — the AC
 *    bonds with us directly, so a press is a GATT notification, not a UART line to a second board.
 *  - Home Assistant over MQTT auto-discovery.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "pins.h"
#include "led_status.h"
#include "ble_emu.h"
#include "wifi_mgr.h"
#include "web.h"
#include "mqtt_ha.h"
#include "bme280.h"
#include "ld2410.h"
#include "rules.h"
#include "ac_state.h"
#include "ac_cmd.h"

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
    ac_state_init();  // open-loop AC model (load last state from NVS) — before any press
    led_status_init();
    ble_emu_init();
    wifi_mgr_init();
    web_start();
    ac_cmd_init();    // worker that turns HA targets into paced press sequences
    mqtt_ha_init();   // starts the MQTT client (no-op if no broker configured); auto-connects once STA is up
    bme280_init();    // I2C BME280 (pins from config) -> Env Sensing chars + HA sensors
    ld2410_init();    // LD2410 presence radar on UART1 (pins from config)
    rules_load();     // presence automation: load rules + start the evaluator task

    ESP_LOGI(TAG, "Ganymede bridge up — AP '%s' / web on :80", wifi_mgr_ap_ssid());
}
