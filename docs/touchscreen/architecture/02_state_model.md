# State Model

## Central state

Create a central state object shared by UI, HTTP, MQTT, and diagnostics.

```c
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
    int64_t nrf_last_seen_ms;
    char nrf_last_message[64];

    bool sensor_available;
    float temperature_c;
    float humidity_percent;
    float pressure_hpa;
    uint32_t gas_resistance_ohm;
    char air_quality[16];
    int64_t sensor_last_update_ms;

    char last_button[16];
    int64_t last_button_ms;
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
```

## State ownership

Only these modules may write state directly:

- Wi-Fi manager writes Wi-Fi state
- MQTT module writes MQTT state
- UART nRF module writes nRF state
- BME680 module writes sensor state
- UI/display module writes display state
- command worker writes last command state

Other modules must use helper functions.

## Required helpers

```c
void bridge_state_init(void);
void bridge_state_get_snapshot(bridge_state_t *out);
void bridge_state_update_wifi(...);
void bridge_state_update_mqtt(...);
void bridge_state_update_nrf(...);
void bridge_state_update_sensor(...);
void bridge_state_update_display(...);
void bridge_state_update_last_command(...);
```

## nRF status values

Normalize incoming nRF status to:

```txt
unknown
booting
pairing
bonded
ready
busy
offline
error
```

Only `bonded` and `ready` allow user commands.

## Sensor availability

Sensor values must be considered unavailable if:

- sensor init failed
- last update is older than 120 seconds
- read error count exceeds threshold

## Air quality derivation

Use a simple local heuristic from gas resistance trend.

Initial implementation may use:

```txt
Unknown: no baseline
Good: gas resistance >= baseline * 0.8
Average: gas resistance >= baseline * 0.5
Poor: gas resistance < baseline * 0.5
```

Do not present this as an official IAQ index.
