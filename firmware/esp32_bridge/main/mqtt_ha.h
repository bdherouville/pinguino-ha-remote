#pragma once
#include <stdbool.h>
#include "ac_state.h"

// MQTT client + Home Assistant MQTT-Discovery. Exposes the 9 remote keys as HA `button`
// entities; pressing one publishes to a command topic that this bridge turns into a
// a button press on the in-process BLE emulator. Broker config is stored in NVS (web UI).
void        mqtt_ha_init(void);                              // load NVS cfg + start (no-op if no host)
bool        mqtt_ha_save(const char *host, int port,
                         const char *user, const char *pass); // save cfg + (re)start client
bool        mqtt_ha_connected(void);
const char *mqtt_ha_host(void);                              // configured broker host ("" if none)
void        mqtt_ha_publish_env(float temp_c, float humidity, float pressure_hpa); // sensor states
void        mqtt_ha_publish_link(const char *state); // emulator state -> diagnostic sensor
void        mqtt_ha_publish_presence(bool present); // LD2410 occupancy -> binary_sensor (marks it available)
void        mqtt_ha_presence_unavailable(void);     // LD2410 offline -> mark the entity unavailable
void        mqtt_ha_publish_ac(const ac_state_t *s); // open-loop AC model -> climate/switch state
