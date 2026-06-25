# Pinguino HA Remote — ESP32 Touchscreen Specification Pack

This pack is intended for Codex or another coding agent.

It defines the target specifications for a new ESP32 touchscreen variant of `pinguino-ha-remote`.

The implementation must preserve the existing architecture:

- ESP32: Wi-Fi, HTTP API, MQTT, Home Assistant discovery, touchscreen UI
- nRF52840: BLE remote emulation for De’Longhi Pinguino
- UART bridge between ESP32 and nRF52840
- BME680: environmental sensor over I2C

No code is included here. These files are specifications, implementation plan, development rules, and test coverage requirements only.

## File index

- `specs/01_product_spec.md` — product and functional specification
- `specs/02_hardware_spec.md` — hardware, wiring, pin abstraction
- `specs/03_touchscreen_ui_spec.md` — LVGL touchscreen UX specification
- `specs/04_api_mqtt_ha_spec.md` — HTTP, MQTT, Home Assistant contracts
- `architecture/01_firmware_architecture.md` — firmware architecture and tasks
- `architecture/02_state_model.md` — state model and command flow
- `plans/01_implementation_plan.md` — phased implementation plan
- `rules/01_development_rules.md` — coding and integration rules
- `rules/02_code_assessment_process.md` — local assessment process and review checklist
- `testing/01_test_plan.md` — test strategy
- `coverage/01_test_coverage_matrix.md` — acceptance and coverage matrix
- `codex/AGENTS.md` — instructions for Codex
