#include "bme280.h"
#include "bridge_state.h"
#include "uart_link.h"
#include "mqtt_ha.h"
#include "pins.h"
#include "board_config.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#define BME_HZ    100000
#define BME_PERIOD_S 30      // how often to read + forward
#define BME_STALE_MS 120000ULL

static uint64_t s_last_update_ms;

#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN

#include <bme680.h>
#include <i2cdev.h>

static const char *TAG = "bme680";

static bme680_t s_sensor;
static bool s_present;
static float s_t = 0, s_h = 0, s_p = 0, s_gas = 0;
static const char *s_air_quality = "Unknown";
static float s_gas_baseline;

static const char *air_quality_from_gas(float gas_ohm)
{
    if (gas_ohm <= 0) return "Unknown";
    if (s_gas_baseline <= 0) {
        s_gas_baseline = gas_ohm;
        return "Unknown";
    }

    float ratio = gas_ohm / s_gas_baseline;
    s_gas_baseline = (s_gas_baseline * 0.95f) + (gas_ohm * 0.05f);
    if (ratio >= 0.8f) return "Good";
    if (ratio >= 0.5f) return "Average";
    return "Poor";
}

static bool read_once(void)
{
    bme680_values_float_t v = {0};
    if (bme680_measure_float(&s_sensor, &v) != ESP_OK) return false;
    if (v.temperature <= -300.0f || v.pressure <= 0.0f || v.humidity <= 0.0f) return false;

    s_t = v.temperature;
    s_h = v.humidity;
    s_p = v.pressure;
    s_gas = v.gas_resistance;
    s_air_quality = air_quality_from_gas(s_gas);
    s_last_update_ms = esp_timer_get_time() / 1000ULL;
    bme680_set_ambient_temperature(&s_sensor, (int16_t)s_t);
    return true;
}

static void task(void *arg)
{
    for (;;) {
        if (read_once()) {
            ESP_LOGI(TAG, "T=%.2f C  H=%.1f %%  P=%.1f hPa  G=%.0f Ohm (%s)",
                     s_t, s_h, s_p, s_gas, s_air_quality);
            bridge_state_update_sensor(true, "BME680", s_t, s_h, s_p,
                                       (uint32_t)s_gas, s_air_quality, s_last_update_ms);
            uart_link_env(s_t, s_h, s_p);        // legacy nRF Env Sensing payload
            mqtt_ha_publish_env_ext(s_t, s_h, s_p, true, s_gas, s_air_quality);
        }
        vTaskDelay(pdMS_TO_TICKS(BME_PERIOD_S * 1000));
    }
}

void bme280_init(void)
{
    const device_pins_t *pn = pins_get();
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2cdev_init());

    for (uint8_t addr = BME680_I2C_ADDR_0; addr <= BME680_I2C_ADDR_1; addr++) {
        memset(&s_sensor, 0, sizeof(s_sensor));
        if (bme680_init_desc(&s_sensor, addr, BME_I2C_PORT,
                             (gpio_num_t)pn->i2c_sda, (gpio_num_t)pn->i2c_scl) != ESP_OK) {
            continue;
        }
        s_sensor.i2c_dev.cfg.master.clk_speed = BME680_I2C_FREQ_HZ;
        if (bme680_init_sensor(&s_sensor) == ESP_OK) {
            ESP_LOGI(TAG, "BME680 found at 0x%02X (SDA=%d SCL=%d)", addr, pn->i2c_sda, pn->i2c_scl);
            s_present = true;
            break;
        }
        bme680_free_desc(&s_sensor);
    }

    if (!s_present) {
        ESP_LOGW(TAG, "no BME680 on the bus (SDA=%d SCL=%d)", pn->i2c_sda, pn->i2c_scl);
        bridge_state_update_sensor(false, "BME680", 0, 0, 0, 0, "Unknown", 0);
        return;
    }

    bme680_set_oversampling_rates(&s_sensor, BME680_OSR_2X, BME680_OSR_2X, BME680_OSR_2X);
    bme680_set_filter_size(&s_sensor, BME680_IIR_SIZE_3);
    bme680_set_heater_profile(&s_sensor, 0, 320, 150);
    bme680_use_heater_profile(&s_sensor, 0);
    xTaskCreate(task, "bme680", 4096, NULL, 4, NULL);
}

bool bme280_present(void) { return s_present; }
bool bme280_get(float *t, float *h, float *p)
{
    if (!s_present || !s_last_update_ms) return false;
    uint64_t now_ms = esp_timer_get_time() / 1000ULL;
    if (now_ms - s_last_update_ms > BME_STALE_MS) return false;
    if (t) *t = s_t;
    if (h) *h = s_h;
    if (p) *p = s_p;
    return true;
}

bool bme280_get_sample(env_sensor_sample_t *sample)
{
    if (!sample) return false;
    *sample = (env_sensor_sample_t) {
        .available = s_present && s_last_update_ms &&
                     ((esp_timer_get_time() / 1000ULL) - s_last_update_ms <= BME_STALE_MS),
        .type = "BME680",
        .temperature_c = s_t,
        .humidity_percent = s_h,
        .pressure_hpa = s_p,
        .gas_valid = s_present && s_last_update_ms && s_gas > 0,
        .gas_resistance_ohm = s_gas,
        .air_quality = s_air_quality,
        .last_update_ms = s_last_update_ms,
    };
    return sample->available;
}

const char *bme280_sensor_type(void) { return "BME680"; }

#else

#include "driver/i2c_master.h"

static const char *TAG = "bme280";

static i2c_master_dev_handle_t s_dev;
static bool   s_present;
static float  s_t = 0, s_h = 0, s_p = 0;

// calibration
static uint16_t T1; static int16_t T2,T3;
static uint16_t P1; static int16_t P2,P3,P4,P5,P6,P7,P8,P9;
static uint8_t  H1,H3; static int16_t H2,H4,H5; static int8_t H6;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t len)
{ return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 1000); }
static esp_err_t wr(uint8_t reg, uint8_t val)
{ uint8_t b[2] = {reg, val}; return i2c_master_transmit(s_dev, b, sizeof(b), 1000); }

static void read_calib(void)
{
    uint8_t t[26], h[7];
    rd(0x88, t, 26);
    T1=t[0]|t[1]<<8; T2=t[2]|t[3]<<8; T3=t[4]|t[5]<<8;
    P1=t[6]|t[7]<<8; P2=t[8]|t[9]<<8; P3=t[10]|t[11]<<8; P4=t[12]|t[13]<<8;
    P5=t[14]|t[15]<<8; P6=t[16]|t[17]<<8; P7=t[18]|t[19]<<8; P8=t[20]|t[21]<<8; P9=t[22]|t[23]<<8;
    H1=t[25];
    rd(0xE1, h, 7);
    H2=h[0]|h[1]<<8; H3=h[2];
    H4=(int16_t)((h[3]<<4)|(h[4]&0x0F));
    H5=(int16_t)((h[5]<<4)|(h[4]>>4));
    H6=(int8_t)h[6];
}

// Bosch fixed-point compensation (datasheet). Returns °C, %RH, hPa via out params.
static void compensate(int32_t adc_T, int32_t adc_P, int32_t adc_H,
                       float *tc, float *rh, float *hpa)
{
    int32_t v1,v2,t_fine;
    v1 = ((((adc_T>>3)-((int32_t)T1<<1)))*((int32_t)T2))>>11;
    v2 = (((((adc_T>>4)-((int32_t)T1))*((adc_T>>4)-((int32_t)T1)))>>12)*((int32_t)T3))>>14;
    t_fine = v1+v2;
    *tc = ((t_fine*5+128)>>8) / 100.0f;

    int64_t p1,p2,p;
    p1 = (int64_t)t_fine - 128000;
    p2 = p1*p1*(int64_t)P6;
    p2 = p2 + ((p1*(int64_t)P5)<<17);
    p2 = p2 + (((int64_t)P4)<<35);
    p1 = ((p1*p1*(int64_t)P3)>>8) + ((p1*(int64_t)P2)<<12);
    p1 = ((((int64_t)1)<<47)+p1)*((int64_t)P1)>>33;
    if (p1==0) { *hpa = 0; }
    else {
        p = 1048576 - adc_P;
        p = (((p<<31)-p2)*3125)/p1;
        p1 = (((int64_t)P9)*(p>>13)*(p>>13))>>25;
        p2 = (((int64_t)P8)*p)>>19;
        p = ((p+p1+p2)>>8) + (((int64_t)P7)<<4);
        *hpa = (p/256.0f) / 100.0f;     // Pa -> hPa
    }

    int32_t x = t_fine - 76800;
    x = ((((adc_H<<14) - (((int32_t)H4)<<20) - (((int32_t)H5)*x)) + 16384)>>15) *
        (((((((x*(int32_t)H6)>>10)*(((x*(int32_t)H3)>>11)+32768))>>10)+2097152)*(int32_t)H2+8192)>>14);
    x = x - (((((x>>15)*(x>>15))>>7)*(int32_t)H1)>>4);
    x = x<0?0:(x>419430400?419430400:x);
    *rh = (x>>12) / 1024.0f;
}

static bool read_once(void)
{
    // Forced-mode single measurement (ctrl_hum must be written before ctrl_meas to take effect),
    // then wait for the conversion to finish before reading — avoids stale/garbage first samples.
    wr(0xF2, 0x01);            // humidity oversampling x1
    wr(0xF4, 0x25);            // temp x1, press x1, FORCED mode
    uint8_t st; int tries = 0;
    do { vTaskDelay(pdMS_TO_TICKS(5)); if (rd(0xF3, &st, 1) != ESP_OK) return false; }
    while ((st & 0x08) && ++tries < 20);   // bit3 = measuring
    uint8_t d[8];
    if (rd(0xF7, d, 8) != ESP_OK) return false;
    int32_t adc_P = (d[0]<<12)|(d[1]<<4)|(d[2]>>4);
    int32_t adc_T = (d[3]<<12)|(d[4]<<4)|(d[5]>>4);
    int32_t adc_H = (d[6]<<8)|d[7];
    compensate(adc_T, adc_P, adc_H, &s_t, &s_h, &s_p);
    return true;
}

static void task(void *arg)
{
    for (;;) {
        if (read_once()) {
            s_last_update_ms = esp_timer_get_time() / 1000ULL;
            ESP_LOGI(TAG, "T=%.2f C  H=%.1f %%  P=%.1f hPa", s_t, s_h, s_p);
            bridge_state_update_sensor(true, "BME280", s_t, s_h, s_p, 0, "Unknown", s_last_update_ms);
            uart_link_env(s_t, s_h, s_p);        // -> nRF emulator Env Sensing
            mqtt_ha_publish_env(s_t, s_h, s_p);  // -> Home Assistant sensors
        }
        vTaskDelay(pdMS_TO_TICKS(BME_PERIOD_S * 1000));
    }
}

void bme280_init(void)
{
    const device_pins_t *pn = pins_get();
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = pn->i2c_sda,
        .scl_io_num = pn->i2c_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bh;
    if (i2c_new_master_bus(&bus, &bh) != ESP_OK) { ESP_LOGE(TAG, "i2c bus init failed"); return; }

    for (uint8_t addr = 0x76; addr <= 0x77; addr++) {
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                   .device_address = addr, .scl_speed_hz = BME_HZ };
        if (i2c_master_bus_add_device(bh, &dc, &s_dev) != ESP_OK) continue;
        uint8_t id = 0;
        if (rd(0xD0, &id, 1) == ESP_OK && id == 0x60) {   // 0x60 = BME280
            ESP_LOGI(TAG, "BME280 found at 0x%02X (SDA=%d SCL=%d)", addr, pn->i2c_sda, pn->i2c_scl);
            s_present = true;
            break;
        }
        i2c_master_bus_rm_device(s_dev); s_dev = NULL;
    }
    if (!s_present) {
        ESP_LOGW(TAG, "no BME280 on the bus (SDA=%d SCL=%d)", pn->i2c_sda, pn->i2c_scl);
        bridge_state_update_sensor(false, "BME280", 0, 0, 0, 0, "Unknown", 0);
        return;
    }

    read_calib();
    wr(0xF5, 0x00);   // config: filter off (sampling mode is FORCED, set per read)
    xTaskCreate(task, "bme280", 4096, NULL, 4, NULL);
}

bool bme280_present(void) { return s_present; }
bool bme280_get(float *t, float *h, float *p)
{
    if (!s_present || !s_last_update_ms) return false;
    uint64_t now_ms = esp_timer_get_time() / 1000ULL;
    if (now_ms - s_last_update_ms > BME_STALE_MS) return false;
    if (t) *t = s_t;
    if (h) *h = s_h;
    if (p) *p = s_p;
    return true;
}

bool bme280_get_sample(env_sensor_sample_t *sample)
{
    if (!sample) return false;
    *sample = (env_sensor_sample_t) {
        .available = s_present && s_last_update_ms &&
                     ((esp_timer_get_time() / 1000ULL) - s_last_update_ms <= BME_STALE_MS),
        .type = "BME280",
        .temperature_c = s_t,
        .humidity_percent = s_h,
        .pressure_hpa = s_p,
        .gas_valid = false,
        .gas_resistance_ohm = 0,
        .air_quality = "Unknown",
        .last_update_ms = s_last_update_ms,
    };
    return sample->available;
}

const char *bme280_sensor_type(void) { return "BME280"; }

#endif
