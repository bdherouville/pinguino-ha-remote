#include "bridge_state.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define SENSOR_STALE_MS 120000ULL

static bridge_state_t s_state;
static SemaphoreHandle_t s_mutex;
static int64_t s_boot_us;

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (!dst || !dst_len) return;
    strlcpy(dst, src ? src : "", dst_len);
}

static void lock_state(void)
{
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock_state(void)
{
    if (s_mutex) xSemaphoreGive(s_mutex);
}

void bridge_state_init(void)
{
    if (s_mutex) return;
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_state, 0, sizeof(s_state));
    copy_str(s_state.nrf_status, sizeof(s_state.nrf_status), "offline");
    copy_str(s_state.sensor_type, sizeof(s_state.sensor_type), "unknown");
    copy_str(s_state.air_quality, sizeof(s_state.air_quality), "Unknown");
    copy_str(s_state.current_screen, sizeof(s_state.current_screen), "remote");
    copy_str(s_state.theme, sizeof(s_state.theme), "dark");
    s_state.brightness_percent = 80;
    s_boot_us = esp_timer_get_time();
}

void bridge_state_get_snapshot(bridge_state_t *out)
{
    if (!out) return;
    lock_state();
    *out = s_state;
    unlock_state();

    int64_t now_us = esp_timer_get_time();
    out->uptime_s = (uint32_t)((now_us - s_boot_us) / 1000000LL);
    out->free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);

    if (out->sensor_available && out->sensor_last_update_ms) {
        uint64_t now_ms = now_us / 1000ULL;
        if (now_ms < out->sensor_last_update_ms ||
            now_ms - out->sensor_last_update_ms > SENSOR_STALE_MS) {
            out->sensor_available = false;
            copy_str(out->air_quality, sizeof(out->air_quality), "Unknown");
        }
    }
}

void bridge_state_update_wifi(bool connected, const char *ssid, const char *ip, int rssi)
{
    lock_state();
    s_state.wifi_connected = connected;
    copy_str(s_state.wifi_ssid, sizeof(s_state.wifi_ssid), ssid);
    copy_str(s_state.wifi_ip, sizeof(s_state.wifi_ip), connected ? ip : "");
    s_state.wifi_rssi = connected ? rssi : 0;
    unlock_state();
}

void bridge_state_update_mqtt(bool configured, bool connected, const char *host)
{
    lock_state();
    s_state.mqtt_configured = configured;
    s_state.mqtt_connected = connected;
    copy_str(s_state.mqtt_host, sizeof(s_state.mqtt_host), host);
    unlock_state();
}

void bridge_state_update_nrf(bool available, const char *status, uint64_t last_seen_ms,
                             const char *last_message)
{
    lock_state();
    s_state.nrf_available = available;
    copy_str(s_state.nrf_status, sizeof(s_state.nrf_status), status);
    s_state.nrf_last_seen_ms = last_seen_ms;
    copy_str(s_state.nrf_last_message, sizeof(s_state.nrf_last_message), last_message);
    unlock_state();
}

void bridge_state_update_sensor(bool available, const char *type, float temperature_c,
                                float humidity_percent, float pressure_hpa,
                                uint32_t gas_resistance_ohm, const char *air_quality,
                                uint64_t last_update_ms)
{
    lock_state();
    s_state.sensor_available = available;
    copy_str(s_state.sensor_type, sizeof(s_state.sensor_type), type);
    s_state.temperature_c = temperature_c;
    s_state.humidity_percent = humidity_percent;
    s_state.pressure_hpa = pressure_hpa;
    s_state.gas_resistance_ohm = gas_resistance_ohm;
    copy_str(s_state.air_quality, sizeof(s_state.air_quality), air_quality ? air_quality : "Unknown");
    s_state.sensor_last_update_ms = last_update_ms;
    unlock_state();
}

void bridge_state_update_display(bool display_available, bool touch_available,
                                 uint8_t brightness_percent, bool backlight_on,
                                 const char *current_screen, const char *theme)
{
    lock_state();
    s_state.display_available = display_available;
    s_state.touch_available = touch_available;
    s_state.brightness_percent = brightness_percent;
    s_state.backlight_on = backlight_on;
    copy_str(s_state.current_screen, sizeof(s_state.current_screen), current_screen);
    copy_str(s_state.theme, sizeof(s_state.theme), theme);
    unlock_state();
}

void bridge_state_update_last_command(const char *button, uint64_t timestamp_ms,
                                      bool success, const char *error)
{
    lock_state();
    copy_str(s_state.last_button, sizeof(s_state.last_button), button);
    s_state.last_button_ms = timestamp_ms;
    s_state.last_button_success = success;
    copy_str(s_state.last_error, sizeof(s_state.last_error), error);
    unlock_state();
}
