/*
 * Ganymede bridge — ESP32-S3 Wi-Fi front-end.
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

#include "led_status.h"
#include "uart_link.h"
#include "wifi_mgr.h"
#include "web.h"
#include "mqtt_ha.h"
#include "bme280.h"

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

    led_status_init();
    uart_link_init();
    wifi_mgr_init();
    web_start();
    mqtt_ha_init();   // starts the MQTT client (no-op if no broker configured); auto-connects once STA is up
    bme280_init();    // I2C BME280 (SDA=GPIO1, SCL=GPIO2) -> UART env + HA sensors

    ESP_LOGI(TAG, "Ganymede bridge up — AP '%s' / web on :80", wifi_mgr_ap_ssid());
}
