#pragma once

// Status shown on the board's status LED. Which LED and which pin is per-chip; see board.h
// (WS2812 on the S3/C3/C6 dev boards, a plain GPIO LED on the classic ESP32).
typedef enum {
    LED_BOOT,            // white dim  — booting
    LED_AP,              // blue pulse — provisioning AP up, waiting for Wi-Fi config
    LED_STA_CONNECTING,  // yellow blink — connecting to Wi-Fi
    LED_STA_CONNECTED,   // green steady — connected (got IP)
    LED_STA_FAILED,      // red slow blink — Wi-Fi connect failed
    LED_ERROR,           // red fast blink — error
    // BLE emulator states (shown once Wi-Fi is connected; driven by ble_emu):
    LED_EMU_DOWN,        // red slow blink — Wi-Fi ok but the BLE host did not come up
    LED_EMU_PAIRING,     // cyan pulse — advertising / waiting for the AC
    LED_EMU_LINK,        // yellow steady — AC connected, not ready yet
    LED_EMU_READY,       // green steady — bonded, can relay
} led_state_t;

void led_status_init(void);
void led_status_set(led_state_t s);
