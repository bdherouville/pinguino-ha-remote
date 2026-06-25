# Implementation Plan

## Phase 0 — Repository assessment

- Inspect current `firmware/esp32_bridge` structure.
- Identify current HTTP, MQTT, UART, Wi-Fi and sensor modules.
- Identify current build system and component style.
- Document current command path before changing it.

Deliverable:

- short note in PR description listing reused files and replaced files.

## Phase 1 — Board abstraction

- Add `board_config.h`.
- Move all GPIO definitions into board config.
- Add display and touch abstraction stubs.
- Build without enabling LVGL UI yet.

Acceptance:

- existing bridge firmware still builds.
- no command behavior change.

## Phase 2 — Central state and command queue

- Add `bridge_state` module.
- Add `command_queue` module.
- Route HTTP and MQTT commands through the queue.
- Preserve UART output format.

Acceptance:

- all existing commands still work.
- unavailable nRF returns clean error.

## Phase 3 — BME680

- Replace or extend current BME280/BMP280 support with BME680.
- Detect `0x76` and `0x77`.
- Publish temperature, humidity, pressure, gas resistance and air quality.
- Extend `/api/status`.

Acceptance:

- boots with sensor.
- boots without sensor.
- values appear in HTTP and MQTT.

## Phase 4 — LVGL display bring-up

- Add LVGL component.
- Add display driver integration.
- Add touch driver integration.
- Render a static boot/status screen first.

Acceptance:

- display initializes.
- touch coordinates are calibrated.
- UI task does not block network services.

## Phase 5 — Remote touchscreen UI

- Build Remote screen.
- Connect buttons to `bridge_press_button()`.
- Add disabled state based on nRF availability.
- Add touch feedback.

Acceptance:

- every UI button sends the correct UART command.
- no direct UART calls from UI code.

## Phase 6 — Status, Sensor and Settings screens

- Add navigation.
- Add status screen.
- Add sensor screen.
- Add settings screen.
- Persist brightness, timeout and theme in NVS.

Acceptance:

- settings survive reboot.
- screen timeout works.
- touch wakes display.

## Phase 7 — Home Assistant discovery update

- Add BME680 discovery entities.
- Add diagnostics.
- Add display brightness entity if appropriate.
- Preserve existing button discovery.

Acceptance:

- HA entities appear after MQTT discovery.
- no duplicate/broken entities from existing install.

## Phase 8 — Regression and coverage

- Run unit tests.
- Run hardware-in-loop tests.
- Validate missing-module cases.
- Validate HA discovery.
- Validate UI responsiveness under MQTT reconnect and Wi-Fi reconnect.
