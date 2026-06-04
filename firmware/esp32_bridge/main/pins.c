#include "pins.h"
#include "nvs.h"
#include "esp_log.h"

#define NVS_NS "pins"
static const char *TAG = "pins";

static device_pins_t s = {
    .i2c_sda = PIN_DEF_I2C_SDA, .i2c_scl = PIN_DEF_I2C_SCL,
    .nrf_tx  = PIN_DEF_NRF_TX,  .nrf_rx  = PIN_DEF_NRF_RX, .nrf_hb = PIN_DEF_NRF_HB,
    .ld_tx   = PIN_DEF_LD_TX,   .ld_rx   = PIN_DEF_LD_RX,
};

static uint8_t get_u8(nvs_handle_t h, const char *key, uint8_t def)
{
    uint8_t v;
    return nvs_get_u8(h, key, &v) == ESP_OK ? v : def;
}

void pins_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved pins — using compiled defaults");
        return;
    }
    s.i2c_sda = get_u8(h, "i2c_sda", PIN_DEF_I2C_SDA);
    s.i2c_scl = get_u8(h, "i2c_scl", PIN_DEF_I2C_SCL);
    s.nrf_tx  = get_u8(h, "nrf_tx",  PIN_DEF_NRF_TX);
    s.nrf_rx  = get_u8(h, "nrf_rx",  PIN_DEF_NRF_RX);
    s.nrf_hb  = get_u8(h, "nrf_hb",  PIN_DEF_NRF_HB);
    s.ld_tx   = get_u8(h, "ld_tx",   PIN_DEF_LD_TX);
    s.ld_rx   = get_u8(h, "ld_rx",   PIN_DEF_LD_RX);
    nvs_close(h);
    ESP_LOGI(TAG, "pins: i2c sda=%d scl=%d | nrf tx=%d rx=%d hb=%d | ld tx=%d rx=%d",
             s.i2c_sda, s.i2c_scl, s.nrf_tx, s.nrf_rx, s.nrf_hb, s.ld_tx, s.ld_rx);
}

const device_pins_t *pins_get(void) { return &s; }

bool pins_valid_gpio(int g)
{
    // ESP32-S3 has GPIO0..48. 22..25 are not bonded out; 26..32 drive the SPI flash/PSRAM
    // and must never be repurposed; GPIO48 is the WS2812 status LED (led_status.c, fixed).
    // Everything else is permitted (incl. strapping pins 0/3/45/46 and the USB pins 19/20 —
    // usable, just left to the user's judgement).
    if (g < 0 || g > 48)      return false;
    if (g >= 22 && g <= 25)   return false;
    if (g >= 26 && g <= 32)   return false;
    if (g == 48)              return false;   // owned by the status LED
    return true;
}

bool pins_save(const device_pins_t *p)
{
    const uint8_t all[] = { p->i2c_sda, p->i2c_scl, p->nrf_tx, p->nrf_rx,
                            p->nrf_hb, p->ld_tx, p->ld_rx };
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

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_u8(h, "i2c_sda", p->i2c_sda);
    nvs_set_u8(h, "i2c_scl", p->i2c_scl);
    nvs_set_u8(h, "nrf_tx",  p->nrf_tx);
    nvs_set_u8(h, "nrf_rx",  p->nrf_rx);
    nvs_set_u8(h, "nrf_hb",  p->nrf_hb);
    nvs_set_u8(h, "ld_tx",   p->ld_tx);
    nvs_set_u8(h, "ld_rx",   p->ld_rx);
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return false;

    s = *p;   // adopt only after a successful commit
    ESP_LOGI(TAG, "pins saved — reboot to apply");
    return true;
}
