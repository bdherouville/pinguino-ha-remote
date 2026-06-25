# Product Specification — ESP32 Touchscreen Pinguino Remote

## Objective

Create a new ESP32 touchscreen firmware variant for `pinguino-ha-remote`.

The device replaces the handheld De’Longhi Pinguino BLE remote with a local-first connected remote:

- local touchscreen control
- HTTP API
- MQTT control
- Home Assistant MQTT discovery
- existing nRF52840 BLE emulation retained
- BME680 environmental sensing added

The ESP32 must not emulate BLE directly. BLE remains the responsibility of the nRF52840.

## Non-goals

Do not implement:

- cloud connectivity
- IR control
- direct ESP32 BLE pairing to the AC
- Home Assistant `climate` entity unless reliable AC state feedback exists
- touchscreen text entry for Wi-Fi or MQTT credentials unless trivial and robust
- mobile app dependency

## Existing behavior to preserve

The new firmware must preserve existing bridge behavior:

- Wi-Fi provisioning AP
- web UI
- HTTP command API
- MQTT command topics
- Home Assistant discovery
- UART command forwarding to nRF52840
- nRF heartbeat and status handling
- stateless button press model

Existing supported commands:

```txt
power
up
down
mode
eco
timer
fan
silent
flap
```

## New behavior

Add:

- integrated touchscreen UI
- LVGL rendering
- touch input
- screen timeout and wake-on-touch
- brightness control
- BME680 sensor support
- sensor display on screen
- sensor exposure through HTTP, MQTT, and HA discovery
- diagnostic screen for Wi-Fi, MQTT, nRF, firmware, and sensor status

## User-facing screens

Required screens:

1. Remote control
2. Device status
3. Environmental sensors
4. Settings

The remote control screen is the default after boot.

## Functional requirements

### Local control

Touchscreen buttons must call the same internal command path as HTTP and MQTT.

Required internal API:

```c
esp_err_t bridge_press_button(const char *button_name);
```

No duplicate command logic is allowed in UI code.

### Availability

The touchscreen must remain usable even when:

- Wi-Fi is disconnected
- MQTT is disconnected
- BME680 is missing

Remote buttons must be disabled when the nRF52840 is unavailable.

### Persistence

Store in NVS:

- Wi-Fi credentials
- MQTT settings
- display brightness
- screen timeout
- UI theme

## Acceptance criteria

- Firmware builds for ESP32.
- Firmware boots without nRF52840 connected.
- Firmware boots without BME680 connected.
- Touchscreen renders the main remote screen.
- Touch buttons trigger UART commands to nRF52840.
- HTTP and MQTT commands still work.
- Home Assistant discovery still works.
- BME680 data is visible on screen, HTTP API, MQTT, and HA.
- `/api/status` exposes Wi-Fi, MQTT, nRF, sensor, and display state.
- No regression in existing command names or topics.
