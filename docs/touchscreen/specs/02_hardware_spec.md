# Hardware Specification

## Target board

Target class:

- ESP32 touchscreen development board
- integrated TFT display
- touch controller
- USB flashing/debug
- 3.3 V GPIO logic
- Wi-Fi enabled

The exact AliExpress board pinout must be verified before final pin assignment.

The implementation must therefore use a board abstraction layer and must not scatter hardcoded pins across the codebase.

## External modules

### nRF52840

Purpose:

- BLE emulation of the original De’Longhi Pinguino remote
- pairing and command transmission to AC

Interface:

- UART 115200 8N1
- optional heartbeat/status GPIO if already supported

Direction:

```txt
ESP32 UART TX -> nRF52840 UART RX
ESP32 UART RX <- nRF52840 UART TX
GND shared
3V3 shared only if power budget is validated
```

### BME680

Purpose:

- ambient temperature
- humidity
- pressure
- gas resistance
- derived indoor air quality indicator

Interface:

- I2C
- address auto-detection: `0x76`, then `0x77`

Wiring:

```txt
ESP32 SDA -> BME680 SDA
ESP32 SCL -> BME680 SCL
ESP32 3V3 -> BME680 VIN/3V3
ESP32 GND -> BME680 GND
```

Use pull-ups only if the BME680 module does not already include them.

## Board abstraction

Create one central file:

```txt
firmware/esp32_bridge/main/board_config.h
```

It must contain all hardware-specific GPIO and display configuration.

Required logical definitions:

```c
#pragma once

#define BOARD_NAME "esp32_touchscreen"

#define NRF_UART_PORT UART_NUM_1
#define NRF_UART_BAUDRATE 115200
#define NRF_UART_TX_GPIO <TBD>
#define NRF_UART_RX_GPIO <TBD>

#define BME680_I2C_PORT I2C_NUM_0
#define BME680_I2C_SDA_GPIO <TBD>
#define BME680_I2C_SCL_GPIO <TBD>
#define BME680_I2C_FREQ_HZ 400000

#define DISPLAY_BACKLIGHT_GPIO <TBD_OR_MINUS_1>
#define STATUS_LED_GPIO <TBD_OR_MINUS_1>

#define TOUCH_DRIVER_NAME "TBD"
#define DISPLAY_DRIVER_NAME "TBD"
```

## Display and touch driver requirements

The board layer must expose:

```c
esp_err_t board_display_init(void);
esp_err_t board_touch_init(void);
esp_err_t board_backlight_set(uint8_t percent);
```

The UI layer must not directly initialize SPI, RGB, I80, I2C touch, or panel-specific details.

## Power requirements

- Confirm that the ESP32 board 3.3 V regulator can power nRF52840 and BME680.
- If not confirmed, power nRF52840 separately with shared ground.
- Do not power external modules from display backlight rails.

## Boot behavior

Required boot behavior:

- if display init fails: keep network/API functionality alive if possible
- if touch init fails: show status-only UI if display works
- if nRF is missing: boot and mark nRF unavailable
- if BME680 is missing: boot and mark sensor unavailable
