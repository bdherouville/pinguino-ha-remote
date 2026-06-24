# Development Rules

## General rules

- Preserve existing public API unless explicitly specified.
- Do not break existing MQTT topics.
- Do not change nRF52840 firmware unless unavoidable.
- Do not introduce cloud dependencies.
- Do not use Arduino framework.
- Use ESP-IDF style consistently.
- Keep hardware-specific details isolated in board abstraction.

## Command rules

- All command sources must use `bridge_press_button()`.
- UI must never write directly to UART.
- HTTP must never write directly to UART.
- MQTT must never write directly to UART.
- Only the command worker writes UART commands.

## State rules

- One central state object.
- Use mutex-protected snapshots.
- Do not hold state mutex during I/O.
- Do not let stale sensor values appear as fresh.

## UI rules

- LVGL UI must be non-blocking.
- Touchscreen must remain responsive during Wi-Fi/MQTT reconnect.
- Buttons must visibly acknowledge touches.
- Buttons must visibly disable when nRF unavailable.
- Minimum touch target: 44 x 44 px.

## Error handling rules

- Missing BME680 is not fatal.
- Missing nRF52840 is not fatal.
- MQTT offline is not fatal.
- Wi-Fi offline is not fatal for local touchscreen use.
- Display failure should not prevent serial/network boot when possible.

## Logging rules

Log clearly:

- boot phase
- board config
- display init result
- touch init result
- BME680 detection result
- nRF status changes
- MQTT connection changes
- command queue errors

Avoid noisy logs in steady state.

## Security rules

- Do not log Wi-Fi password.
- Do not log MQTT password.
- Do not expose secrets through `/api/status`.
- Reset endpoints must require deliberate action, not accidental GET.

## Style rules

- Small modules.
- Explicit names.
- No magic GPIO numbers outside `board_config.h`.
- No command strings duplicated across multiple modules.
- Central list of valid buttons.
- Return `esp_err_t` for hardware initialization functions.
