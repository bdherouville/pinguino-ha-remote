#include "led_status.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#if BOARD_LED_KIND == BOARD_LED_WS2812
#include "led_strip.h"
#elif BOARD_LED_KIND == BOARD_LED_PLAIN
#include "driver/gpio.h"
#endif

// Which LED this board has, and on which pin, comes from board.h — GPIO48 exists only on the
// S3, so hard-coding it made the firmware S3-only.
//
// Every state carries BOTH a colour and a blink pattern. A WS2812 shows both; a plain
// single-colour LED can only show the pattern, so the patterns alone have to distinguish the
// states that can occur at the same time. They do: solid = good, slow blink = waiting, fast
// blink = trouble, 2 blips = emulator dead, 3 blips = AC connected but not encrypted yet.
// (LED_STA_CONNECTED and LED_EMU_READY are both solid, which is fine — the Wi-Fi states only
// show before the emulator takes the LED over.)

static const char *TAG = "led";

typedef enum { PAT_SOLID, PAT_SLOW, PAT_FAST, PAT_BLIP2, PAT_BLIP3 } pattern_t;

static const struct { uint8_t r, g, b; pattern_t pat; } LED_STATES[] = {
    [LED_BOOT]           = { 10, 10, 10, PAT_SOLID },  // white dim
    [LED_AP]             = {  0,  0, 40, PAT_SLOW  },  // blue pulse
    [LED_STA_CONNECTING] = { 30, 20,  0, PAT_FAST  },  // yellow blink
    [LED_STA_CONNECTED]  = {  0, 24,  0, PAT_SOLID },  // green
    [LED_STA_FAILED]     = { 40,  0,  0, PAT_SLOW  },  // red slow
    [LED_ERROR]          = { 50,  0,  0, PAT_FAST  },  // red fast
    [LED_EMU_DOWN]       = { 40,  0,  0, PAT_BLIP2 },  // red, two blips
    [LED_EMU_PAIRING]    = {  0, 22, 32, PAT_SLOW  },  // cyan pulse
    [LED_EMU_LINK]       = { 28, 16,  0, PAT_BLIP3 },  // amber, three blips
    [LED_EMU_READY]      = {  0, 24,  0, PAT_SOLID },  // green
};

static volatile led_state_t s_state = LED_BOOT;

// Pattern phase, in 100 ms slots.
static bool pattern_lit(pattern_t p, uint32_t t)
{
    switch (p) {
    case PAT_SOLID: return true;
    case PAT_SLOW:  return (t % 10) < 5;                      // 500 ms on / 500 ms off
    case PAT_FAST:  return (t % 4)  < 2;                      // 200 ms on / 200 ms off
    case PAT_BLIP2: { uint32_t k = t % 20;                     // blip blip ... pause
                      return k < 2 || (k >= 4 && k < 6); }
    case PAT_BLIP3: { uint32_t k = t % 26;
                      return k < 2 || (k >= 4 && k < 6) || (k >= 8 && k < 10); }
    }
    return false;
}

#if BOARD_LED_KIND == BOARD_LED_WS2812
static led_strip_handle_t s_strip;

static void led_render(uint8_t r, uint8_t g, uint8_t b, bool lit, pattern_t pat)
{
    if (!s_strip) return;
    if (!lit) {
        // A slow pulse looks better dipping to a dim floor than to black; the blip patterns
        // need true black or you can't count the blips.
        if (pat == PAT_SLOW) { r /= 8; g /= 8; b /= 8; } else { r = g = b = 0; }
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void led_hw_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_LED_GPIO,
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
        ESP_LOGW(TAG, "WS2812 init on GPIO%d failed (%s) — status LED disabled. If this board "
                      "has a plain LED there, set BOARD_LED_KIND to BOARD_LED_PLAIN in board.h",
                 BOARD_LED_GPIO, esp_err_to_name(err));
        s_strip = NULL;
    }
}

#elif BOARD_LED_KIND == BOARD_LED_PLAIN
static void led_render(uint8_t r, uint8_t g, uint8_t b, bool lit, pattern_t pat)
{
    (void)r; (void)g; (void)b; (void)pat;
    gpio_set_level(BOARD_LED_GPIO, BOARD_LED_ACTIVE_HIGH ? lit : !lit);
}

static void led_hw_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    ESP_LOGI(TAG, "plain status LED on GPIO%d (patterns, not colours)", BOARD_LED_GPIO);
}

#else /* BOARD_LED_NONE */
static void led_render(uint8_t r, uint8_t g, uint8_t b, bool lit, pattern_t pat)
{ (void)r; (void)g; (void)b; (void)lit; (void)pat; }
static void led_hw_init(void) { ESP_LOGI(TAG, "no status LED on this board"); }
#endif

static void led_task(void *arg)
{
    uint32_t t = 0;
    for (;;) {
        led_state_t st = s_state;
        led_render(LED_STATES[st].r, LED_STATES[st].g, LED_STATES[st].b,
                   pattern_lit(LED_STATES[st].pat, t), LED_STATES[st].pat);
        t++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void led_status_set(led_state_t s) { s_state = s; }

void led_status_init(void)
{
    led_hw_init();
    xTaskCreate(led_task, "led", 2048, NULL, 3, NULL);
}
