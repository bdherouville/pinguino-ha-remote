#pragma once
#include <stdbool.h>

// BME280 on I2C_0. SDA/SCL come from the runtime pin config (see pins.h; defaults
// SDA=GPIO2, SCL=GPIO1); the address (0x76/0x77) is auto-detected. A task reads every
// BME_PERIOD_S seconds and forwards the reading to the BLE emulator
// (`env <tC> <h%> <hPa>`) and to Home Assistant over MQTT.
void  bme280_init(void);
bool  bme280_present(void);
// last reading; returns false if no sensor. Units: °C, %RH, hPa.
bool  bme280_get(float *temp_c, float *humidity, float *pressure_hpa);
