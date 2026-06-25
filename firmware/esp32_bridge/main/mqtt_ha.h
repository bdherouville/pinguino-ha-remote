#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ac_state.h"

// MQTT client + Home Assistant MQTT-Discovery. Exposes the 9 remote keys as HA `button`
// entities; pressing one publishes to a command topic that this bridge turns into a
// UART `press <btn>` to the nRF emulator. Broker config is stored in NVS (web UI).
void        mqtt_ha_init(void);                              // load NVS cfg + start (no-op if no host)
bool        mqtt_ha_save(const char *host, int port,
                         const char *user, const char *pass); // save cfg + (re)start client
bool        mqtt_ha_connected(void);
const char *mqtt_ha_host(void);                              // configured broker host ("" if none)
void        mqtt_ha_publish_env(float temp_c, float humidity, float pressure_hpa); // sensor states
void        mqtt_ha_publish_env_ext(float temp_c, float humidity, float pressure_hpa,
                                    bool gas_valid, float gas_resistance_ohm,
                                    const char *air_quality);
void        mqtt_ha_publish_nrf(const char *state); // nRF link state -> diagnostic sensor
void        mqtt_ha_publish_last_button(const char *button_name);
void        mqtt_ha_publish_display_brightness(uint8_t percent);
void        mqtt_ha_publish_display_timeout(uint16_t seconds);
void        mqtt_ha_publish_display_theme(const char *theme);
void        mqtt_ha_publish_wifi_rssi(int rssi);
void        mqtt_ha_publish_presence(bool present); // LD2410 occupancy -> binary_sensor (marks it available)
void        mqtt_ha_presence_unavailable(void);     // LD2410 offline -> mark the entity unavailable
void        mqtt_ha_publish_ac(const ac_state_t *s); // open-loop AC model -> climate/switch state
