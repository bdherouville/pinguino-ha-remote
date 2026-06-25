# Test Plan

## Test levels

Required levels:

1. Unit tests
2. Integration tests
3. Hardware-in-loop tests
4. Home Assistant/MQTT validation
5. Manual touchscreen acceptance tests

Current automated source-contract coverage:

- `python3 tools/test_touchscreen_contracts.py`
- `python3 tools/check_touchscreen_firmware.py`

These gates run in CI and release workflows before ESP-IDF builds. They cover
source-level contracts that do not require attached hardware. Hardware-in-loop
and manual touchscreen acceptance tests remain required once the touchscreen
panel/touch controller pinout, nRF52840 and BME680 are connected.

## Unit tests

Test:

- button name validation
- command queue behavior
- nRF status normalization
- sensor availability timeout
- air quality heuristic
- state snapshot consistency
- `/api/status` JSON serialization

## Integration tests

Test with mocked UART:

- HTTP press -> command queue -> UART line
- MQTT command -> command queue -> UART line
- UI event -> command queue -> UART line
- nRF ready message -> state update
- nRF timeout -> buttons unavailable

Test with mocked BME680:

- successful sensor read
- missing sensor
- stale sensor values
- I2C read errors

## Hardware-in-loop tests

Required hardware:

- ESP32 touchscreen board
- nRF52840 board flashed with existing BLE emulator
- BME680 module
- MQTT broker
- Home Assistant instance or MQTT discovery validator

Test cases:

- clean boot with all hardware
- boot without nRF52840
- boot without BME680
- boot without MQTT broker
- Wi-Fi reconnect
- MQTT reconnect
- UART reconnect/power-cycle nRF
- screen timeout and wake
- brightness persistence

## Manual UI tests

Remote screen:

- Power button works
- Up button works
- Down button works
- Mode button works
- Fan button works
- Silent button works
- Eco button works
- Timer button works
- Flap button works
- disabled state appears when nRF unavailable

Status screen:

- IP shown
- MQTT shown
- nRF shown
- firmware shown
- uptime/free heap shown

Sensor screen:

- temperature shown
- humidity shown
- pressure shown
- gas resistance shown
- air quality shown
- unavailable state shown when sensor disconnected

Settings screen:

- brightness changes display
- brightness survives reboot
- timeout works
- theme survives reboot
- reboot button works
- provisioning reset requires deliberate confirmation

## Regression tests

Existing behavior must remain valid:

- existing web UI loads
- existing `/api/press` works
- existing MQTT button topics work
- existing Home Assistant button discovery works
- nRF UART protocol unchanged
