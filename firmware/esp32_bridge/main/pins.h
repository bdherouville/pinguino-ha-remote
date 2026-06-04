#pragma once
#include <stdint.h>
#include <stdbool.h>

// Configurable GPIO assignments for the bridge's peripherals, persisted in NVS
// (namespace "pins"). Read once at boot by each *_init(); changes apply on the next
// reboot. Any key not present in NVS falls back to the compiled default below, so a
// fresh device behaves exactly like the old hard-wired build.
typedef struct {
    uint8_t i2c_sda;   // BME280 I2C SDA
    uint8_t i2c_scl;   // BME280 I2C SCL
    uint8_t nrf_tx;    // ESP TX  -> nRF RX   (UART1)
    uint8_t nrf_rx;    // ESP RX  <- nRF TX   (UART1)
    uint8_t nrf_hb;    // nRF heartbeat input
    uint8_t ld_tx;     // ESP TX  -> LD2410 RX (UART2)  [consumed in the presence feature]
    uint8_t ld_rx;     // ESP RX  <- LD2410 TX (UART2)  [consumed in the presence feature]
} device_pins_t;

// Compiled defaults = the current hard-wired pinout, plus free UART2 pins for the LD2410.
#define PIN_DEF_I2C_SDA   2
#define PIN_DEF_I2C_SCL   1
#define PIN_DEF_NRF_TX    4
#define PIN_DEF_NRF_RX    5
#define PIN_DEF_NRF_HB    6
#define PIN_DEF_LD_TX     17
#define PIN_DEF_LD_RX     18

// Load saved pins from NVS (defaults where unset). Call once, before the *_init()s.
void                 pins_load(void);
// Cached config (valid after pins_load()).
const device_pins_t *pins_get(void);
// Validate every field then persist; updates the cache on success. Returns false (and
// changes nothing) if any pin is not a usable ESP32-S3 GPIO.
bool                 pins_save(const device_pins_t *p);
// True if g is a GPIO that can be bonded out / used for I/O on the ESP32-S3.
bool                 pins_valid_gpio(int g);
