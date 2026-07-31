#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "board.h"

// Configurable GPIO assignments for the bridge's peripherals, persisted in NVS
// (namespace "pins"). Read once at boot by each *_init(); changes apply on the next
// reboot. Any key not present in NVS falls back to the compiled default below, so a
// fresh device behaves exactly like the old hard-wired build.
typedef struct {
    uint8_t i2c_sda;   // BME280 I2C SDA
    uint8_t i2c_scl;   // BME280 I2C SCL
    uint8_t ld_tx;     // ESP TX  -> LD2410 RX (UART1)  [consumed in the presence feature]
    uint8_t ld_rx;     // ESP RX  <- LD2410 TX (UART1)  [consumed in the presence feature]
} device_pins_t;

// Compiled defaults come from board.h, which picks a valid set per chip — the numbers differ
// between the S3, C3, C6 and the original ESP32. (The former nrf_tx/nrf_rx/nrf_hb entries are
// gone: the BLE emulator runs on this chip's own radio, so there is no second board to wire.)
#define PIN_DEF_I2C_SDA   BOARD_PIN_I2C_SDA
#define PIN_DEF_I2C_SCL   BOARD_PIN_I2C_SCL
#define PIN_DEF_LD_TX     BOARD_PIN_LD_TX
#define PIN_DEF_LD_RX     BOARD_PIN_LD_RX

// Load saved pins from NVS (defaults where unset). Call once, before the *_init()s.
void                 pins_load(void);
// Cached config (valid after pins_load()).
const device_pins_t *pins_get(void);
// Validate every field then persist; updates the cache on success. Returns false (and
// changes nothing) if any pin is not assignable on this chip.
bool                 pins_save(const device_pins_t *p);
// True if g is a GPIO that may be assigned to a peripheral on this chip (see board.h).
bool                 pins_valid_gpio(int g);
