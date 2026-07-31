#include "pins.h"
#include "board.h"
#include "nvs.h"
#include "esp_log.h"

#define NVS_NS "pins"
static const char *TAG = "pins";

static device_pins_t s = {
    .i2c_sda = PIN_DEF_I2C_SDA, .i2c_scl = PIN_DEF_I2C_SCL,
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
    s.ld_tx   = get_u8(h, "ld_tx",   PIN_DEF_LD_TX);
    s.ld_rx   = get_u8(h, "ld_rx",   PIN_DEF_LD_RX);
    nvs_close(h);
    ESP_LOGI(TAG, "pins (%s): i2c sda=%d scl=%d | ld tx=%d rx=%d",
             BOARD_NAME, s.i2c_sda, s.i2c_scl, s.ld_tx, s.ld_rx);
}

const device_pins_t *pins_get(void) { return &s; }

// Per-chip rules live in board.h so this file stays target-agnostic. Strapping pins are
// permitted (usable, just left to the user's judgement); flash, USB-console and non-bonded
// pins are not, because getting those wrong costs you the board or the serial port.
bool pins_valid_gpio(int g)
{
    return board_gpio_assignable(g);
}

bool pins_save(const device_pins_t *p)
{
    const uint8_t all[] = { p->i2c_sda, p->i2c_scl, p->ld_tx, p->ld_rx };
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
    // Bail on the first failing write so a partial persist (e.g. full NVS) isn't reported as
    // success — otherwise the device could come back on a mixed pin map after reboot.
    esp_err_t e = nvs_set_u8(h, "i2c_sda", p->i2c_sda);
    if (e == ESP_OK) e = nvs_set_u8(h, "i2c_scl", p->i2c_scl);
    if (e == ESP_OK) e = nvs_set_u8(h, "ld_tx",   p->ld_tx);
    if (e == ESP_OK) e = nvs_set_u8(h, "ld_rx",   p->ld_rx);
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
