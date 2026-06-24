# Firmware Architecture

## Modules

Required modules:

```txt
main.c
board_config.h
bridge_state.c/.h
command_queue.c/.h
uart_nrf.c/.h
bme680_sensor.c/.h
display.c/.h
touch.c/.h
ui_lvgl.c/.h
http_api.c/.h
mqtt_client.c/.h
ha_discovery.c/.h
wifi_manager.c/.h
nvs_settings.c/.h
```

Existing files may be reused or renamed, but responsibilities must remain separated.

## Core principle

All command sources must converge into one command queue.

Sources:

- touchscreen
- HTTP API
- MQTT
- future automation code

Flow:

```txt
Input source -> bridge_press_button() -> command queue -> UART worker -> nRF52840
```

## FreeRTOS tasks

Required logical tasks:

### UI task

- LVGL tick/render
- touch event dispatch
- UI state refresh

### UART nRF task

- read UART lines
- parse nRF status
- update bridge state

### Command worker task

- consume command queue
- validate nRF availability
- write UART command
- update last command state

### BME680 task

- detect sensor
- read periodically
- update bridge state
- trigger MQTT publish

### MQTT task

- maintain connection
- subscribe to commands
- publish state
- publish discovery

### HTTP task

- serve web UI
- expose API

### Wi-Fi task

- provisioning
- reconnect
- status updates

## Locking

Use a mutex around `bridge_state_t`.

Rules:

- never hold state mutex while doing network I/O
- never hold state mutex while writing UART
- copy state snapshot for UI rendering
- copy state snapshot for `/api/status`

## Eventing

Use event groups or queues for:

- Wi-Fi connected/disconnected
- MQTT connected/disconnected
- nRF status changed
- sensor updated
- display settings changed

## Failure isolation

A failure in one subsystem must not crash the device.

Examples:

- BME680 missing: sensor unavailable only
- MQTT broker down: local UI and HTTP still work
- nRF missing: diagnostics still work
- touch missing: display status-only mode
