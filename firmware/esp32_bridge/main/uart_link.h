#pragma once
#include <stdbool.h>

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
    NRF_BOOT,          // alive, not advertising yet
    NRF_ADVERTISING,   // in pairing mode, visible
    NRF_CONNECTED,     // AC connected, not bonded yet
    NRF_BONDED,        // bonded, HID not subscribed yet
    NRF_READY,         // bonded + HID subscribed -> can relay presses
    NRF_ERROR,         // emulator reported an error
} nrf_state_t;

void uart_link_init(void);
bool uart_link_press(const char *btn);   // returns false if btn invalid
void uart_link_env(float temp_c, float humidity, float pressure_hpa); // -> "env <t> <h> <p>\n"
void uart_link_pairing(bool unpair);     // -> "unpair\n" (clear bond, pairing mode) or "pair\n"

nrf_state_t uart_link_nrf_state(void);   // current effective state
bool        uart_link_alive(void);       // heartbeat or recent UART seen
const char *uart_link_status(void);      // short status string for the UI / MQTT
