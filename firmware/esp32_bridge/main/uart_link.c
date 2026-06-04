#include "uart_link.h"
#include "led_status.h"
#include "wifi_mgr.h"
#include "mqtt_ha.h"
#include "pins.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#define LINK_UART   UART_NUM_1
// TX/RX/heartbeat GPIOs come from the runtime pin config (see pins.h). Defaults are plain
// GPIOs on the SuperMini's main castellated edge (P3, no strapping/special function) —
// chosen over the U0TXD/U0RXD (43/44) pads, which carry the ROM boot UART.
#define LINK_BAUD    115200

// Hardware liveness: the nRF toggles the heartbeat line (~1 Hz). Input + pulldown, so a
// missing/dead emulator reads a steady 0 and is correctly reported offline.
#define HB_TIMEOUT_MS 3000     // no edge AND no UART for this long -> offline
#define LINK_POLL_MS  100      // RX read timeout = heartbeat sampling period

static uint8_t s_hb_gpio = PIN_DEF_NRF_HB;   // resolved from pins at init

static const char *TAG = "uart";

// Buttons accepted (must match the emulator's table / docs/ganymede_protocol.md).
static const char *VALID[] = {"power","down","up","mode","eco","timer","fan","silent","flap"};
static const int   NVALID  = sizeof(VALID)/sizeof(VALID[0]);

// nRF token -> rich state. "boot" is the alive-but-idle default.
static const struct { const char *tok; nrf_state_t st; } STATES[] = {
    {"boot",        NRF_BOOT},
    {"advertising", NRF_ADVERTISING},
    {"adv",         NRF_ADVERTISING},
    {"pairing",     NRF_ADVERTISING},
    {"connected",   NRF_CONNECTED},
    {"conn",        NRF_CONNECTED},
    {"bonded",      NRF_BONDED},
    {"ready",       NRF_READY},
    {"error",       NRF_ERROR},
    {"err",         NRF_ERROR},
};
#define NSTATES (sizeof(STATES)/sizeof(STATES[0]))

static const char *STATE_STR[] = {
    [NRF_OFFLINE]="offline", [NRF_BOOT]="boot", [NRF_ADVERTISING]="advertising",
    [NRF_CONNECTED]="connected", [NRF_BONDED]="bonded", [NRF_READY]="ready", [NRF_ERROR]="error",
};

static volatile nrf_state_t s_reported = NRF_BOOT;   // last token from the nRF (UART)
static volatile nrf_state_t s_effective = NRF_OFFLINE; // token gated by liveness
static volatile bool s_alive = false;
static int64_t s_last_rx_us, s_last_hb_us;
static int     s_hb_level = -1;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// ---- TX ----
bool uart_link_valid_btn(const char *btn)
{
    for (int i = 0; i < NVALID; i++) if (!strcmp(btn, VALID[i])) return true;
    return false;
}

bool uart_link_press(const char *btn)
{
    if (!uart_link_valid_btn(btn)) return false;
    char line[32];
    int n = snprintf(line, sizeof(line), "press %s\n", btn);
    uart_write_bytes(LINK_UART, line, n);
    ESP_LOGI(TAG, "-> nRF: press %s", btn);
    return true;
}

void uart_link_env(float t, float h, float p)
{
    char line[48];
    int n = snprintf(line, sizeof(line), "env %.2f %.2f %.2f\n", t, h, p);
    uart_write_bytes(LINK_UART, line, n);   // emulator CLI: env <tempC> <hum%> <hPa>
}

void uart_link_pairing(bool unpair)
{
    const char *line = unpair ? "unpair\n" : "pair\n";
    uart_write_bytes(LINK_UART, line, strlen(line));
    ESP_LOGI(TAG, "-> nRF: %s", unpair ? "unpair" : "pair");
}

// ---- RX line parsing ----
static void handle_line(char *line)
{
    s_last_rx_us = esp_timer_get_time();
    // "status <token>" — anything else (logs, banners) is ignored.
    if (strncmp(line, "status ", 7) != 0) return;
    const char *tok = line + 7;
    while (*tok == ' ') tok++;
    for (size_t i = 0; i < NSTATES; i++)
        if (!strncmp(tok, STATES[i].tok, strlen(STATES[i].tok))) {
            s_reported = STATES[i].st;
            return;
        }
}

// ---- effective state + reflection to LED / MQTT ----
static void reflect(nrf_state_t st)
{
    static nrf_state_t last = -1;
    if (st == last) {
        // still refresh the LED when Wi-Fi is up (wifi_mgr may have re-set it on reconnect)
    } else {
        last = st;
        ESP_LOGI(TAG, "nRF link: %s", STATE_STR[st]);
        mqtt_ha_publish_nrf(STATE_STR[st]);
    }
    // The LED shows the nRF link only once Wi-Fi is connected; during provisioning the
    // Wi-Fi states own it (handled by wifi_mgr).
    if (strcmp(wifi_mgr_state_str(), "connected") != 0) return;
    switch (st) {
    case NRF_OFFLINE:     led_status_set(LED_NRF_DOWN);    break;
    case NRF_ERROR:       led_status_set(LED_ERROR);       break;
    case NRF_BOOT:
    case NRF_ADVERTISING: led_status_set(LED_NRF_PAIRING); break;
    case NRF_CONNECTED:
    case NRF_BONDED:      led_status_set(LED_NRF_LINK);    break;
    case NRF_READY:       led_status_set(LED_NRF_READY);   break;
    }
}

static void link_task(void *arg)
{
    char line[128]; int len = 0;
    uint8_t buf[64];
    // Start offline: prime the level from the actual pin (so the first sample isn't a false
    // edge) and backdate the activity timestamps past the timeout.
    s_hb_level = gpio_get_level(s_hb_gpio);
    s_last_hb_us = s_last_rx_us = esp_timer_get_time() - (int64_t)(HB_TIMEOUT_MS + 1) * 1000;
    for (;;) {
        // drain UART (also paces the loop at LINK_POLL_MS when idle)
        int n = uart_read_bytes(LINK_UART, buf, sizeof(buf), pdMS_TO_TICKS(LINK_POLL_MS));
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (len) { line[len] = 0; handle_line(line); len = 0; }
            } else if (len < (int)sizeof(line) - 1) {
                line[len++] = c;
            }
        }

        // sample heartbeat edge
        int lvl = gpio_get_level(s_hb_gpio);
        if (lvl != s_hb_level) { s_hb_level = lvl; s_last_hb_us = esp_timer_get_time(); }

        // liveness = a heartbeat edge or UART traffic within the window
        int64_t t = esp_timer_get_time();
        bool alive = (t - s_last_hb_us) < (int64_t)HB_TIMEOUT_MS * 1000 ||
                     (t - s_last_rx_us) < (int64_t)HB_TIMEOUT_MS * 1000;
        s_alive = alive;
        s_effective = alive ? s_reported : NRF_OFFLINE;
        reflect(s_effective);
    }
}

void uart_link_init(void)
{
    const device_pins_t *pn = pins_get();
    s_hb_gpio = pn->nrf_hb;

    const uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, pn->nrf_tx, pn->nrf_rx,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    gpio_config_t hb = {
        .pin_bit_mask = 1ULL << s_hb_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&hb);

    xTaskCreate(link_task, "nrf_link", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "UART link up: TX=%d RX=%d @ %d, heartbeat on GPIO%d",
             pn->nrf_tx, pn->nrf_rx, LINK_BAUD, s_hb_gpio);
}

nrf_state_t uart_link_nrf_state(void) { return s_effective; }
bool        uart_link_alive(void)     { return s_alive; }
const char *uart_link_status(void)    { return STATE_STR[s_effective]; }
