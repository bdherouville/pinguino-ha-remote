#pragma once

#include <stdint.h>

// Environmental data source for the emulated Environmental Sensing service (0x181A).
// Phase 2: returns stub values. Later: backed by an I2C BME280 (temp/humidity/pressure).
// See memory mock-remote-hardware-plan. I2C is intentionally NOT wired here yet.

typedef struct {
    int16_t  temp_c_x100;     // 0x2A6E Temperature: sint16, units of 0.01 °C
    uint16_t humidity_x100;   // 0x2A6F Humidity:    uint16, units of 0.01 %
    uint32_t pressure_x10;    // 0x2A6D Pressure:    uint32, units of 0.1 Pa
} env_reading_t;

void env_source_init(void);

// Current reading. Stub implementation returns fixed/synthetic values for now;
// swap the body for a BME280 read later without touching the GATT layer.
env_reading_t env_source_get(void);

// CLI/test override of the stub values (no effect once a real sensor is wired).
void env_source_set_stub(int16_t temp_c_x100, uint16_t humidity_x100, uint32_t pressure_x10);
