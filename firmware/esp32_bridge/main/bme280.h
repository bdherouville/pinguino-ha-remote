#pragma once
#include <stdbool.h>

// BME280 on I2C (ESP32-S3 super-mini default bus: SDA=GPIO8, SCL=GPIO9; addr 0x76 or 0x77,
// auto-detected). A task reads every BME_PERIOD_S seconds and forwards the reading to the nRF
// emulator over UART (`env <tC> <h%> <hPa>`) and to Home Assistant over MQTT.
void  bme280_init(void);
bool  bme280_present(void);
// last reading; returns false if no sensor. Units: °C, %RH, hPa.
bool  bme280_get(float *temp_c, float *humidity, float *pressure_hpa);
