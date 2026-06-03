#include "env_source.h"

// Stub source. Defaults roughly match values seen from the real remote
// (temp ~25 °C, humidity ~47 %, pressure ~1013 hPa). Replace env_source_get()
// with a BME280 I2C read later; nothing else needs to change.
static env_reading_t s_stub = {
    .temp_c_x100   = 2500,        // 25.00 °C
    .humidity_x100 = 4700,        // 47.00 %
    .pressure_x10  = 10132500,    // 1013250 Pa = 1013.25 hPa, in 0.1 Pa units
};

void env_source_init(void)
{
    // No-op for the stub. Later: init I2C bus + BME280.
}

env_reading_t env_source_get(void)
{
    return s_stub;
}

void env_source_set_stub(int16_t temp_c_x100, uint16_t humidity_x100, uint32_t pressure_x10)
{
    s_stub.temp_c_x100 = temp_c_x100;
    s_stub.humidity_x100 = humidity_x100;
    s_stub.pressure_x10 = pressure_x10;
}
