#include "led_status.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"

// NOTE: GPIO48 is a WS2812 RGB on the common ESP32-S3 super-mini revision. Some revisions
// wire a plain LED there instead — then this drives nothing harmful but the colours won't
// render. See docs/re/ESP32-S3-SuperMini-BOARD.md.
#define LED_GPIO   48

static const char *TAG = "led";
static led_strip_handle_t s_strip;
static volatile led_state_t s_state = LED_BOOT;

static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void led_task(void *arg)
{
    uint32_t tick = 0;
    for (;;) {
        bool on = (tick & 1);          // 0.5 Hz base toggle (period below)
        bool fast = (tick & 1);        // reused with shorter delay states
        switch (s_state) {
        case LED_BOOT:           set_rgb(10, 10, 10); break;            // white dim
        case LED_AP:             set_rgb(0, 0, on ? 40 : 4); break;     // blue pulse
        case LED_STA_CONNECTING: set_rgb(fast ? 30 : 0, fast ? 20 : 0, 0); break; // yellow blink
        case LED_STA_CONNECTED:  set_rgb(0, 24, 0); break;             // green steady
        case LED_STA_FAILED:     set_rgb(on ? 40 : 0, 0, 0); break;     // red slow blink
        case LED_ERROR:          set_rgb(fast ? 50 : 0, 0, 0); break;   // red fast blink
        case LED_NRF_DOWN:       set_rgb(on ? 40 : 4, 0, 0); break;     // red slow blink
        case LED_NRF_PAIRING:    set_rgb(0, on ? 22 : 2, on ? 32 : 4); break; // cyan pulse
        case LED_NRF_LINK:       set_rgb(28, 16, 0); break;            // yellow steady
        case LED_NRF_READY:      set_rgb(0, 24, 0); break;             // green steady
        }
        tick++;
        // Faster cadence for "blink/connecting/error", slower for steady states.
        vTaskDelay(pdMS_TO_TICKS((s_state == LED_STA_CONNECTING || s_state == LED_ERROR) ? 150 : 400));
    }
}

void led_status_set(led_state_t s) { s_state = s; }

void led_status_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = { .with_dma = false },
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led_strip init failed (%d) — status LED disabled", err);
        s_strip = NULL;
    }
    xTaskCreate(led_task, "led", 2048, NULL, 3, NULL);
}
