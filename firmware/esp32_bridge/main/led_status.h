#pragma once

// Status shown on the ESP32-S3 super-mini RGB LED (WS2812 on GPIO48).
typedef enum {
    LED_BOOT,            // white dim  — booting
    LED_AP,              // blue pulse — provisioning AP up, waiting for Wi-Fi config
    LED_STA_CONNECTING,  // yellow blink — connecting to Wi-Fi
    LED_STA_CONNECTED,   // green steady — connected (got IP)
    LED_STA_FAILED,      // red slow blink — Wi-Fi connect failed
    LED_ERROR,           // red fast blink — error
    // nRF link states (shown once Wi-Fi is connected; driven by uart_link):
    LED_NRF_DOWN,        // red slow blink — Wi-Fi ok but nRF emulator not alive
    LED_NRF_PAIRING,     // cyan pulse — nRF advertising / waiting for the AC
    LED_NRF_LINK,        // yellow steady — AC connected/bonded, not ready yet
    LED_NRF_READY,       // green steady — bonded + HID subscribed, can relay
} led_state_t;

void led_status_init(void);
void led_status_set(led_state_t s);
