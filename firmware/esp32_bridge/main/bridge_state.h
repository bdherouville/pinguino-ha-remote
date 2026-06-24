#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool wifi_connected;
    char wifi_ssid[33];
    char wifi_ip[16];
    int wifi_rssi;

    bool mqtt_configured;
    bool mqtt_connected;
    char mqtt_host[64];

    bool nrf_available;
    char nrf_status[16];
    uint64_t nrf_last_seen_ms;
    char nrf_last_message[64];

    bool sensor_available;
    char sensor_type[12];
    float temperature_c;
    float humidity_percent;
    float pressure_hpa;
    uint32_t gas_resistance_ohm;
    char air_quality[16];
    uint64_t sensor_last_update_ms;

    char last_button[16];
    uint64_t last_button_ms;
    bool last_button_success;
    char last_error[64];

    bool display_available;
    bool touch_available;
    uint8_t brightness_percent;
    bool backlight_on;
    char current_screen[16];
    char theme[8];

    uint32_t uptime_s;
    uint32_t free_heap;
} bridge_state_t;

void bridge_state_init(void);
void bridge_state_get_snapshot(bridge_state_t *out);

void bridge_state_update_wifi(bool connected, const char *ssid, const char *ip, int rssi);
void bridge_state_update_mqtt(bool configured, bool connected, const char *host);
void bridge_state_update_nrf(bool available, const char *status, uint64_t last_seen_ms,
                             const char *last_message);
void bridge_state_update_sensor(bool available, const char *type, float temperature_c,
                                float humidity_percent, float pressure_hpa,
                                uint32_t gas_resistance_ohm, const char *air_quality,
                                uint64_t last_update_ms);
void bridge_state_update_display(bool display_available, bool touch_available,
                                 uint8_t brightness_percent, bool backlight_on,
                                 const char *current_screen, const char *theme);
void bridge_state_update_last_command(const char *button, uint64_t timestamp_ms,
                                      bool success, const char *error);
