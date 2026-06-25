#include "pins.h"
#include "nvs.h"
#include "esp_log.h"
#include <limits.h>

#define NVS_NS "pins"
static const char *TAG = "pins";

static device_pins_t s = {
    .i2c_sda = PIN_DEF_I2C_SDA, .i2c_scl = PIN_DEF_I2C_SCL,
    .nrf_tx  = PIN_DEF_NRF_TX,  .nrf_rx  = PIN_DEF_NRF_RX, .nrf_hb = PIN_DEF_NRF_HB,
    .ld_tx   = PIN_DEF_LD_TX,   .ld_rx   = PIN_DEF_LD_RX,
};

static void log_pin_map(void)
{
    ESP_LOGI(TAG, "board %s pins: i2c sda=%d scl=%d | nrf tx=%d rx=%d hb=%d | ld tx=%d rx=%d",
             BOARD_NAME, s.i2c_sda, s.i2c_scl, s.nrf_tx, s.nrf_rx, s.nrf_hb, s.ld_tx, s.ld_rx);
}

static int8_t get_pin(nvs_handle_t h, const char *key, int8_t def)
{
    int8_t v;
    if (nvs_get_i8(h, key, &v) == ESP_OK) return v;

    uint8_t old_v;
    if (nvs_get_u8(h, key, &old_v) == ESP_OK && old_v <= INT8_MAX) {
        return (int8_t)old_v;
    }
    return def;
}

static void validate_loaded_pin(int8_t *pin, int8_t def, const char *name)
{
    if (pins_valid_gpio(*pin)) return;
    ESP_LOGW(TAG, "saved %s GPIO%d is not usable on this board; using GPIO%d", name, *pin, def);
    *pin = def;
}

static void validate_loaded_pins(void)
{
    validate_loaded_pin(&s.i2c_sda, PIN_DEF_I2C_SDA, "i2c_sda");
    validate_loaded_pin(&s.i2c_scl, PIN_DEF_I2C_SCL, "i2c_scl");
    validate_loaded_pin(&s.nrf_tx, PIN_DEF_NRF_TX, "nrf_tx");
    validate_loaded_pin(&s.nrf_rx, PIN_DEF_NRF_RX, "nrf_rx");
    if (s.nrf_hb >= 0) validate_loaded_pin(&s.nrf_hb, PIN_DEF_NRF_HB, "nrf_hb");
    validate_loaded_pin(&s.ld_tx, PIN_DEF_LD_TX, "ld_tx");
    validate_loaded_pin(&s.ld_rx, PIN_DEF_LD_RX, "ld_rx");
}

void pins_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved pins — using compiled defaults");
        validate_loaded_pins();
        log_pin_map();
        return;
    }
    s.i2c_sda = get_pin(h, "i2c_sda", PIN_DEF_I2C_SDA);
    s.i2c_scl = get_pin(h, "i2c_scl", PIN_DEF_I2C_SCL);
    s.nrf_tx  = get_pin(h, "nrf_tx",  PIN_DEF_NRF_TX);
    s.nrf_rx  = get_pin(h, "nrf_rx",  PIN_DEF_NRF_RX);
    s.nrf_hb  = get_pin(h, "nrf_hb",  PIN_DEF_NRF_HB);
    s.ld_tx   = get_pin(h, "ld_tx",   PIN_DEF_LD_TX);
    s.ld_rx   = get_pin(h, "ld_rx",   PIN_DEF_LD_RX);
    nvs_close(h);
    validate_loaded_pins();
    log_pin_map();
}

const device_pins_t *pins_get(void) { return &s; }

bool pins_valid_gpio(int g)
{
#ifdef CONFIG_IDF_TARGET_ESP32
    // Original ESP32: GPIO6..11 are used by SPI flash, 34..39 are input-only, and
    // GPIO20/24/28..31 are not exposed on the classic ESP32 GPIO matrix.
    if (g < 0 || g > 39) return false;
    if (g >= 6 && g <= 11) return false;
    if (g == 20 || g == 24) return false;
    if (g >= 28 && g <= 31) return false;
    if (g >= 34 && g <= 39) return false;
#else
    // ESP32-S3 has GPIO0..48. 22..25 are not bonded out; 26..32 drive the SPI flash/PSRAM
    // and must never be repurposed; GPIO48 is the WS2812 status LED (led_status.c, fixed).
    // Everything else is permitted (incl. strapping pins 0/3/45/46 and the USB pins 19/20 —
    // usable, just left to the user's judgement).
    if (g < 0 || g > 48)      return false;
    if (g >= 22 && g <= 25)   return false;
    if (g >= 26 && g <= 32)   return false;
#endif
#if STATUS_LED_GPIO >= 0
    if (g == STATUS_LED_GPIO) return false;
#endif
#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
    if (g == DISPLAY_SPI_MISO_GPIO || g == DISPLAY_SPI_MOSI_GPIO ||
        g == DISPLAY_SPI_SCLK_GPIO || g == DISPLAY_SPI_CS_GPIO ||
        g == DISPLAY_DC_GPIO || g == DISPLAY_BACKLIGHT_GPIO ||
        g == TOUCH_RST_GPIO || g == TOUCH_INT_GPIO) {
        return false;
    }
#endif
    return true;
}

bool pins_save(const device_pins_t *p)
{
    const int8_t all[] = { p->i2c_sda, p->i2c_scl, p->nrf_tx, p->nrf_rx,
                           p->ld_tx, p->ld_rx };
    for (size_t i = 0; i < sizeof(all); i++) {
        if (!pins_valid_gpio(all[i])) {
            ESP_LOGW(TAG, "rejecting save: GPIO%d is not usable", all[i]);
            return false;
        }
        // each pin drives a distinct signal; a shared pad would clobber a peripheral at boot
        for (size_t j = 0; j < i; j++)
            if (all[i] == all[j]) {
                ESP_LOGW(TAG, "rejecting save: GPIO%d assigned to two roles", all[i]);
                return false;
            }
    }
    if (p->nrf_hb >= 0) {
        if (!pins_valid_gpio(p->nrf_hb)) {
            ESP_LOGW(TAG, "rejecting save: GPIO%d is not usable", p->nrf_hb);
            return false;
        }
        for (size_t i = 0; i < sizeof(all); i++) {
            if (p->nrf_hb == all[i]) {
                ESP_LOGW(TAG, "rejecting save: GPIO%d assigned to two roles", p->nrf_hb);
                return false;
            }
        }
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    // Bail on the first failing write so a partial persist (e.g. full NVS) isn't reported as
    // success — otherwise the device could come back on a mixed pin map after reboot.
    esp_err_t e = nvs_set_i8(h, "i2c_sda", p->i2c_sda);
    if (e == ESP_OK) e = nvs_set_i8(h, "i2c_scl", p->i2c_scl);
    if (e == ESP_OK) e = nvs_set_i8(h, "nrf_tx",  p->nrf_tx);
    if (e == ESP_OK) e = nvs_set_i8(h, "nrf_rx",  p->nrf_rx);
    if (e == ESP_OK) e = nvs_set_i8(h, "nrf_hb",  p->nrf_hb);
    if (e == ESP_OK) e = nvs_set_i8(h, "ld_tx",   p->ld_tx);
    if (e == ESP_OK) e = nvs_set_i8(h, "ld_rx",   p->ld_rx);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "pins NVS save failed: %s", esp_err_to_name(e));
        return false;
    }

    s = *p;   // adopt only after a fully successful commit
    ESP_LOGI(TAG, "pins saved — reboot to apply");
    return true;
}
