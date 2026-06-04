#include "ld2410.h"
#include "pins.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"

#define LD_UART  UART_NUM_2
#define LD_BAUD  256000          // LD2410 factory default
#define LD_ALIVE_TIMEOUT_MS 2000 // no valid frame for this long -> sensor considered absent

static const char *TAG = "ld2410";

static volatile bool     s_present;
static volatile bool     s_alive;
static volatile bool     s_seen;
static volatile uint8_t  s_state;
static volatile uint16_t s_moving_cm, s_static_cm;
static int64_t s_last_frame_us;

// Report frame: F4 F3 F2 F1 | len(2,LE) | data[len] | F8 F7 F6 F5
// Basic target data segment: 02 AA <state> <mov d(2)> <mov e> <stat d(2)> <stat e> <det d(2)> 55 00
static void parse_data(const uint8_t *d, int dlen)
{
    if (dlen < 11 || d[0] != 0x02 || d[1] != 0xAA) return;   // not a basic target frame
    s_state     = d[2];
    s_moving_cm = (uint16_t)(d[3] | (d[4] << 8));
    s_static_cm = (uint16_t)(d[6] | (d[7] << 8));
    s_present   = (s_state != 0x00);   // 0=no target, 1=moving, 2=static, 3=both
    s_seen      = true;
    s_last_frame_us = esp_timer_get_time();
}

static void task(void *arg)
{
    static const uint8_t HDR[4] = {0xF4,0xF3,0xF2,0xF1};
    static const uint8_t FTR[4] = {0xF8,0xF7,0xF6,0xF5};
    uint8_t acc[256]; int n = 0;
    uint8_t rx[128];
    for (;;) {
        int r = uart_read_bytes(LD_UART, rx, sizeof(rx), pdMS_TO_TICKS(100));
        if (r > 0) {
            if (n + r > (int)sizeof(acc)) n = 0;        // overflow -> resync
            memcpy(acc + n, rx, r); n += r;

            int i = 0;
            while (n - i >= 10) {                        // min framed length
                if (memcmp(acc + i, HDR, 4) != 0) { i++; continue; }
                int dlen = acc[i+4] | (acc[i+5] << 8);
                if (dlen < 2 || dlen > 64) { i++; continue; }   // bogus length -> resync
                int total = 4 + 2 + dlen + 4;
                if (n - i < total) break;                // wait for the rest of the frame
                if (memcmp(acc + i + 6 + dlen, FTR, 4) != 0) { i++; continue; }
                parse_data(acc + i + 6, dlen);
                i += total;
            }
            if (i > 0) { memmove(acc, acc + i, n - i); n -= i; }
        }
        s_alive = (esp_timer_get_time() - s_last_frame_us) < (int64_t)LD_ALIVE_TIMEOUT_MS * 1000;
        if (!s_alive) s_present = false;
    }
}

void ld2410_init(void)
{
    const device_pins_t *pn = pins_get();
    const uart_config_t cfg = {
        .baud_rate  = LD_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(LD_UART, 1024, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config(LD_UART, &cfg) != ESP_OK ||
        uart_set_pin(LD_UART, pn->ld_tx, pn->ld_rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "UART2 init failed");
        return;
    }
    s_last_frame_us = esp_timer_get_time() - (int64_t)(LD_ALIVE_TIMEOUT_MS + 1) * 1000;
    xTaskCreate(task, "ld2410", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "LD2410 on UART2 TX=%d RX=%d @ %d", pn->ld_tx, pn->ld_rx, LD_BAUD);
}

bool ld2410_present(void) { return s_present; }
bool ld2410_alive(void)   { return s_alive; }

bool ld2410_get(uint8_t *state, uint16_t *moving_cm, uint16_t *static_cm)
{
    if (!s_seen) return false;
    if (state)     *state     = s_state;
    if (moving_cm) *moving_cm = s_moving_cm;
    if (static_cm) *static_cm = s_static_cm;
    return true;
}
