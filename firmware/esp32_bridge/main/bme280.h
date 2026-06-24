#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool available;
    const char *type;
    float temperature_c;
    float humidity_percent;
    float pressure_hpa;
    bool gas_valid;
    float gas_resistance_ohm;
    const char *air_quality;
    uint64_t last_update_ms;
} env_sensor_sample_t;

// Headless builds keep the existing BME280 behavior. Touchscreen builds use BME680
// on the same logical I2C pins and expose gas resistance in the richer sample API.
void  bme280_init(void);
bool  bme280_present(void);
// last reading; returns false if no sensor. Units: °C, %RH, hPa.
bool  bme280_get(float *temp_c, float *humidity, float *pressure_hpa);
bool  bme280_get_sample(env_sensor_sample_t *sample);
const char *bme280_sensor_type(void);
