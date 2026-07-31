#pragma once
#include <stdbool.h>
#include <stdint.h>

// In-process Ganymede remote emulator (NimBLE, on this ESP32-S3's own radio).
//
// This replaces the former UART link to a separate nRF52840 board: the BLE peripheral that
// the AC bonds with now runs here, so a press goes straight from the web/MQTT handler into a
// GATT notification. The API is deliberately the same shape as the old uart_link.h so the
// callers (ac_cmd, web, mqtt_ha, rules, bme280) did not have to change their logic — only the
// names. See docs/ganymede_protocol.md for what the emulator has to look like on air.

// Minimum gap the AC will register between two presses (its ~1.5 s capacitive-touch debounce,
// plus margin). One source of truth: the HA worker paces to it and the web handler drops taps
// under it, both keyed off the same last-press timestamp so the two sources never collide.
#define BLE_EMU_PRESS_GAP_MS 1800

// Emulator link state. Same ladder as the old nRF token stream, with two differences:
//   - OFFLINE no longer means "the other board is missing" (there isn't one) — it means the
//     BLE host has not come up.
//   - the old BONDED rung is gone. It used to mean "bonded but the AC has not subscribed to
//     our HID report yet", which on the nRF needed a force-subscribe hack to escape. Here a
//     notification does not require a subscription, so bonded *is* relay-capable and collapses
//     straight into READY. Keeping a separate rung would leave the LED yellow while control
//     actually works.
typedef enum {
    EMU_OFFLINE = 0,   // BLE host not up (still syncing, or bt init failed)
    EMU_BOOT,          // host up, not advertising yet
    EMU_ADVERTISING,   // in pairing mode (no bond) or reconnect mode (bonded), visible
    EMU_CONNECTED,     // AC connected, link not encrypted yet
    EMU_READY,         // relay-capable: bonded (or subscribed), a press reaches the AC
    EMU_ERROR,         // advertising or host start failed
} emu_state_t;

void ble_emu_init(void);

bool    ble_emu_press(const char *btn);     // false if btn invalid, coalesced, or not relayable
bool    ble_emu_valid_btn(const char *btn); // true if btn is an accepted button name
void    ble_emu_mute(int seconds);          // sync window: model-only presses, nothing sent, for N s
int     ble_emu_mute_secs(void);            // seconds remaining in the sync window (0 = sending)
int64_t ble_emu_since_press_us(void);       // microseconds since the last registered press
bool    ble_emu_ready(void);                // a press reaches the AC
bool    ble_emu_will_model(void);           // a press would update the model (ready OR syncing)
void    ble_emu_env(float temp_c, float humidity, float pressure_hpa); // -> Env Sensing chars
void    ble_emu_pairing(bool unpair);       // unpair: drop the bond and re-enter pairing mode

emu_state_t ble_emu_state(void);            // current state
bool        ble_emu_alive(void);            // BLE host is up
const char *ble_emu_status(void);           // short status string for the UI / MQTT
