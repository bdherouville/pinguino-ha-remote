#pragma once
#include <stdbool.h>
#include <stdint.h>

// Minimum gap the AC will register between two presses (its ~1.5 s capacitive-touch debounce, plus
// margin). One source of truth: the HA worker paces to it and the web handler drops taps under it,
// both keyed off the same last-press timestamp so the two sources never collide below this gap.
#define UART_LINK_PRESS_GAP_MS 1800

// Link to the nRF52840 emulator over UART. For now sends the same text grammar as the
// emulator's serial CLI ("press <btn>\n"); a framed [SOF][LEN][SEQ][TYPE][CRC] protocol with
// ACK is the planned Phase-3 hardening (see firmware/esp32_bridge/README.md).
//
// Status from the nRF comes back two ways:
//   - rich state over UART: the emulator prints "status <token>\n" on every BLE state change
//     (boot|advertising|connected|bonded|ready|error);
//   - hardware liveness: the emulator toggles a heartbeat GPIO (~1 Hz). If it stops toggling
//     (and UART goes quiet) the link is reported offline regardless of the last token.

// Effective nRF link state (rich token gated by hardware liveness).
typedef enum {
    NRF_OFFLINE = 0,   // no heartbeat + no UART  -> emulator absent/dead
    NRF_BOOT,          // alive, not advertising yet; reported as "booting"
    NRF_ADVERTISING,   // in pairing mode, visible; reported as "pairing"
    NRF_CONNECTED,     // AC connected, not bonded yet; reported as "busy"
    NRF_BONDED,        // bonded, HID not subscribed yet
    NRF_READY,         // bonded + HID subscribed -> can relay presses
    NRF_ERROR,         // emulator reported an error
} nrf_state_t;

void uart_link_init(void);
bool uart_link_press(const char *btn);   // returns false if btn invalid
bool uart_link_valid_btn(const char *btn); // true if btn is an accepted button name
void uart_link_mute(int seconds);        // sync window: model-only presses, nothing sent, for N s
int  uart_link_mute_secs(void);          // seconds remaining in the sync window (0 = sending)
int64_t uart_link_since_press_us(void);  // microseconds since the last registered press (web debounce)
bool uart_link_ready(void);              // emulator bonded + HID-ready (a press reaches the AC)
bool uart_link_will_model(void);         // a press would update the model (relay ready OR syncing)
void uart_link_env(float temp_c, float humidity, float pressure_hpa); // -> "env <t> <h> <p>\n"
void uart_link_pairing(bool unpair);     // -> "unpair\n" (clear bond, pairing mode) or "pair\n"

nrf_state_t uart_link_nrf_state(void);   // current effective state
bool        uart_link_alive(void);       // heartbeat or recent UART seen
const char *uart_link_status(void);      // short status string for the UI / MQTT
