# AGENTS.md — Codex Instructions

## Mission

Implement a new ESP32 touchscreen variant for `pinguino-ha-remote` using the specifications in this directory.

Do not start by writing UI code. First inspect the existing repository and preserve the current command, API, MQTT and Home Assistant behavior.

## Required order of work

1. Inspect existing `firmware/esp32_bridge`.
2. Identify current command path from HTTP/MQTT to UART.
3. Introduce board abstraction without changing behavior.
4. Introduce central state model.
5. Introduce command queue.
6. Add BME680 support.
7. Add LVGL display/touch initialization.
8. Add touchscreen screens.
9. Extend MQTT and HA discovery.
10. Add tests and update documentation.

## Hard constraints

- Do not use Arduino framework.
- Do not break existing MQTT topics.
- Do not break existing HTTP endpoints.
- Do not change nRF52840 UART protocol unless explicitly required.
- Do not put GPIO numbers outside `board_config.h`.
- Do not duplicate command strings in multiple modules.
- Do not write directly to UART from UI, HTTP or MQTT handlers.
- Do not expose secrets in logs or `/api/status`.

## Preferred implementation pattern

Use this flow for commands:

```txt
UI / HTTP / MQTT -> bridge_press_button() -> command queue -> UART worker -> nRF52840
```

Use this flow for state:

```txt
Subsystem -> bridge_state_update_*() -> bridge_state_get_snapshot() -> UI / HTTP / MQTT
```

## Definition of done

The implementation is done only when:

- firmware builds for ESP32
- existing API still works
- existing MQTT commands still work
- touchscreen buttons work
- nRF unavailable state is handled cleanly
- BME680 unavailable state is handled cleanly
- Home Assistant discovery exposes buttons and sensors
- tests cover command flow, state, sensor absence, nRF timeout and API schema
