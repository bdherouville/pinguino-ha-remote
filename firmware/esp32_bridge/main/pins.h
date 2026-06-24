#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "board_config.h"

// Configurable GPIO assignments for the bridge's peripherals, persisted in NVS
// (namespace "pins"). Read once at boot by each *_init(); changes apply on the next
// reboot. Any key not present in NVS falls back to the compiled default below, so a
// fresh device behaves exactly like the old hard-wired build.
typedef struct {
    int8_t i2c_sda;   // BME/BME680 I2C SDA
    int8_t i2c_scl;   // BME/BME680 I2C SCL
    int8_t nrf_tx;    // ESP TX  -> nRF RX   (UART1)
    int8_t nrf_rx;    // ESP RX  <- nRF TX   (UART1)
    int8_t nrf_hb;    // optional nRF heartbeat input; -1 disables heartbeat liveness
    int8_t ld_tx;     // ESP TX  -> LD2410 RX (UART2)  [consumed in the presence feature]
    int8_t ld_rx;     // ESP RX  <- LD2410 TX (UART2)  [consumed in the presence feature]
} device_pins_t;

// Compiled defaults = the selected board pinout, plus UART2 pins for the LD2410.
#define PIN_DEF_I2C_SDA   BME_I2C_SDA_GPIO
#define PIN_DEF_I2C_SCL   BME_I2C_SCL_GPIO
#define PIN_DEF_NRF_TX    NRF_UART_TX_GPIO
#define PIN_DEF_NRF_RX    NRF_UART_RX_GPIO
#define PIN_DEF_NRF_HB    NRF_HEARTBEAT_GPIO
#define PIN_DEF_LD_TX     LD2410_UART_TX_GPIO
#define PIN_DEF_LD_RX     LD2410_UART_RX_GPIO

// Load saved pins from NVS (defaults where unset). Call once, before the *_init()s.
void                 pins_load(void);
// Cached config (valid after pins_load()).
const device_pins_t *pins_get(void);
// Validate every field then persist; updates the cache on success. Returns false (and
// changes nothing) if any pin is not a usable GPIO for the selected ESP-IDF target.
bool                 pins_save(const device_pins_t *p);
// True if g is a GPIO that can be bonded out / used for these peripherals.
bool                 pins_valid_gpio(int g);
