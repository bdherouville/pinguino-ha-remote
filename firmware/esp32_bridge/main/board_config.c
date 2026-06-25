#include "board_config.h"

#include "esp_log.h"

#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2cdev.h"
#include "lvgl.h"
#endif

#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
static const char *TAG = "board";

#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_DRAW_LINES 40
#define LCD_DRAW_BUF_PIXELS (DISPLAY_H_RES * LCD_DRAW_LINES)
#define TOUCH_RAW_H_RES 240
#define TOUCH_RAW_V_RES 320
#define LCD_BACKLIGHT_DUTY_RES LEDC_TIMER_10_BIT
#define LCD_BACKLIGHT_DUTY_MAX ((1U << 10) - 1U)
#define LCD_BACKLIGHT_FREQ_HZ 5000
#define TOUCH_I2C_TIMEOUT_MS 50

#if LV_COLOR_DEPTH != 16
#error "JC2432W328 display driver expects LVGL RGB565 (LV_COLOR_DEPTH=16)"
#endif

static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_lcd_panel;
static lv_display_t *s_display;
static lv_indev_t *s_indev;
static i2c_dev_t s_touch_dev;
static bool s_touch_dev_ready;
static uint16_t s_last_touch_x;
static uint16_t s_last_touch_y;
static bool s_backlight_ready;
static board_touch_activity_cb_t s_touch_activity_cb;
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
static bool s_touch_diag_pressed;
static uint16_t s_touch_diag_start_x;
static uint16_t s_touch_diag_start_y;
static int64_t s_touch_diag_start_us;
static int64_t s_touch_diag_last_log_us;
#endif

static bool lcd_flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io;
    (void)edata;
    lv_display_t *display = (lv_display_t *)user_ctx;
    if (display) {
        lv_display_flush_ready(display);
    }
    return false;
}

static void lcd_flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    if (!s_lcd_panel) {
        lv_display_flush_ready(display);
        return;
    }

    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area));
    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_lcd_panel,
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        px_map);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LCD flush failed: %s", esp_err_to_name(err));
        lv_display_flush_ready(display);
    }
}

#ifdef CONFIG_PINGUINO_TOUCHSCREEN_DISPLAY_DIAGNOSTIC
static void lcd_diagnostic_fill(uint16_t *line, uint16_t color, size_t pixels)
{
    uint16_t swapped = (uint16_t)((color << 8) | (color >> 8));
    for (size_t i = 0; i < pixels; i++) {
        line[i] = swapped;
    }
}

static void lcd_diagnostic_render(void)
{
    if (!s_lcd_panel) return;

    uint16_t *line = heap_caps_malloc(DISPLAY_H_RES * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!line) {
        ESP_LOGW(TAG, "LCD diagnostic skipped: no DMA line buffer");
        return;
    }

    static const uint16_t bands[] = {0xf800, 0x07e0, 0x001f, 0xffff, 0x0000};
    const int band_h = DISPLAY_V_RES / (int)(sizeof(bands) / sizeof(bands[0]));
    for (size_t band = 0; band < sizeof(bands) / sizeof(bands[0]); band++) {
        lcd_diagnostic_fill(line, bands[band], DISPLAY_H_RES);
        int y1 = (int)band * band_h;
        int y2 = (band == sizeof(bands) / sizeof(bands[0]) - 1) ? DISPLAY_V_RES : y1 + band_h;
        for (int y = y1; y < y2; y++) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, y, DISPLAY_H_RES, y + 1, line));
        }
    }

    for (int y = DISPLAY_V_RES - 40; y < DISPLAY_V_RES; y++) {
        for (int x = 0; x < DISPLAY_H_RES; x++) {
            uint16_t color = (((x / 10) + (y / 10)) & 1) ? 0xffff : 0x0000;
            line[x] = (uint16_t)((color << 8) | (color >> 8));
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, y, DISPLAY_H_RES, y + 1, line));
    }

    free(line);
    vTaskDelay(pdMS_TO_TICKS(1500));
}
#endif

static esp_err_t backlight_init(void)
{
    if (s_backlight_ready) return ESP_OK;

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_BACKLIGHT_DUTY_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = LCD_BACKLIGHT_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) return err;

    ledc_channel_config_t channel = {
        .gpio_num = DISPLAY_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err == ESP_OK) s_backlight_ready = true;
    return err;
}

static esp_err_t cst820_read(uint8_t reg, uint8_t *data, size_t len)
{
    if (!s_touch_dev_ready || !data || len == 0) return ESP_ERR_INVALID_STATE;
    return i2c_dev_read_reg(&s_touch_dev, reg, data, len);
}

static esp_err_t cst820_write(uint8_t reg, uint8_t value)
{
    if (!s_touch_dev_ready) return ESP_ERR_INVALID_STATE;
    return i2c_dev_write_reg(&s_touch_dev, reg, &value, 1);
}

static bool cst820_get_touch(uint16_t *x, uint16_t *y)
{
    uint8_t fingers = 0;
    if (cst820_read(0x02, &fingers, 1) != ESP_OK || fingers == 0) {
        return false;
    }

    uint8_t xy[4] = {0};
    if (cst820_read(0x03, xy, sizeof(xy)) != ESP_OK) {
        return false;
    }

    uint16_t raw_x = (uint16_t)(((xy[0] & 0x0f) << 8) | xy[1]);
    uint16_t raw_y = (uint16_t)(((xy[2] & 0x0f) << 8) | xy[3]);
    if (raw_x >= TOUCH_RAW_H_RES) raw_x = TOUCH_RAW_H_RES - 1;
    if (raw_y >= TOUCH_RAW_V_RES) raw_y = TOUCH_RAW_V_RES - 1;

    *x = DISPLAY_H_RES - 1 - raw_y;
    *y = raw_x;
    return true;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x = 0;
    uint16_t y = 0;
    if (cst820_get_touch(&x, &y)) {
        s_last_touch_x = x;
        s_last_touch_y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        if (s_touch_activity_cb) s_touch_activity_cb();
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
        int64_t now_us = esp_timer_get_time();
        if (!s_touch_diag_pressed) {
            s_touch_diag_pressed = true;
            s_touch_diag_start_x = x;
            s_touch_diag_start_y = y;
            s_touch_diag_start_us = now_us;
            s_touch_diag_last_log_us = now_us;
            ESP_LOGI(TAG, "touch down x=%u y=%u", (unsigned)x, (unsigned)y);
        } else if (now_us - s_touch_diag_last_log_us >= 250000) {
            ESP_LOGI(TAG, "touch move x=%u y=%u dx=%d dy=%d held=%lldms",
                     (unsigned)x, (unsigned)y,
                     (int)x - (int)s_touch_diag_start_x,
                     (int)y - (int)s_touch_diag_start_y,
                     (long long)((now_us - s_touch_diag_start_us) / 1000));
            s_touch_diag_last_log_us = now_us;
        }
#endif
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
        if (s_touch_diag_pressed) {
            int64_t now_us = esp_timer_get_time();
            ESP_LOGI(TAG, "touch up x=%u y=%u held=%lldms",
                     (unsigned)s_last_touch_x, (unsigned)s_last_touch_y,
                     (long long)((now_us - s_touch_diag_start_us) / 1000));
            s_touch_diag_pressed = false;
        }
#endif
    }
    data->point.x = s_last_touch_x;
    data->point.y = s_last_touch_y;
}

static esp_err_t touch_reset(void)
{
    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << TOUCH_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&rst);
    if (err != ESP_OK) return err;

    gpio_config_t intr = {
        .pin_bit_mask = 1ULL << TOUCH_INT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&intr);
    if (err != ESP_OK) return err;

    gpio_set_level(TOUCH_INT_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(TOUCH_INT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    gpio_set_level(TOUCH_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(300));

    gpio_set_direction(TOUCH_INT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(TOUCH_INT_GPIO, GPIO_PULLUP_ONLY);
    return ESP_OK;
}
#endif

esp_err_t board_display_init(void)
{
#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
    if (s_display) return ESP_OK;

    esp_err_t err = backlight_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight PWM init failed: %s", esp_err_to_name(err));
    }

    spi_bus_config_t buscfg = {
        .sclk_io_num = DISPLAY_SPI_SCLK_GPIO,
        .mosi_io_num = DISPLAY_SPI_MOSI_GPIO,
        .miso_io_num = DISPLAY_SPI_MISO_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_DRAW_BUF_PIXELS * sizeof(lv_color_t),
    };
    err = spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = DISPLAY_DC_GPIO,
        .cs_gpio_num = DISPLAY_SPI_CS_GPIO,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .spi_mode = 3,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST, &io_config, &s_lcd_io);
    if (err != ESP_OK) return err;

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(s_lcd_io, &panel_config, &s_lcd_panel);
    if (err != ESP_OK) return err;

    err = esp_lcd_panel_reset(s_lcd_panel);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_init(s_lcd_panel);
    if (err != ESP_OK) return err;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_invert_color(s_lcd_panel, false));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_mirror(s_lcd_panel, false, true));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_swap_xy(s_lcd_panel, true));
    err = esp_lcd_panel_disp_on_off(s_lcd_panel, true);
    if (err != ESP_OK) return err;

#ifdef CONFIG_PINGUINO_TOUCHSCREEN_DISPLAY_DIAGNOSTIC
    lcd_diagnostic_render();
#endif

    void *buf1 = heap_caps_malloc(LCD_DRAW_BUF_PIXELS * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(LCD_DRAW_BUF_PIXELS * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf1 || !buf2) {
        free(buf1);
        free(buf2);
        return ESP_ERR_NO_MEM;
    }

    s_display = lv_display_create(DISPLAY_H_RES, DISPLAY_V_RES);
    if (!s_display) {
        free(buf1);
        free(buf2);
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, lcd_flush);
    lv_display_set_buffers(s_display, buf1, buf2, LCD_DRAW_BUF_PIXELS * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lcd_flush_done,
    };
    err = esp_lcd_panel_io_register_event_callbacks(s_lcd_io, &cbs, s_display);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "%s display ready: %s %dx%d SPI mosi=%d sclk=%d cs=%d dc=%d bl=%d",
             BOARD_MODEL, DISPLAY_DRIVER_NAME, DISPLAY_H_RES, DISPLAY_V_RES,
             DISPLAY_SPI_MOSI_GPIO, DISPLAY_SPI_SCLK_GPIO, DISPLAY_SPI_CS_GPIO,
             DISPLAY_DC_GPIO, DISPLAY_BACKLIGHT_GPIO);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t board_touch_init(void)
{
#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
    if (s_indev) return ESP_OK;

    esp_err_t err = touch_reset();
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2cdev_init());
    memset(&s_touch_dev, 0, sizeof(s_touch_dev));
    s_touch_dev.port = TOUCH_I2C_PORT;
    s_touch_dev.addr = TOUCH_I2C_ADDR_CST820;
    s_touch_dev.cfg.sda_io_num = TOUCH_I2C_SDA_GPIO;
    s_touch_dev.cfg.scl_io_num = TOUCH_I2C_SCL_GPIO;
    s_touch_dev.cfg.master.clk_speed = TOUCH_I2C_FREQ_HZ;
    err = i2c_dev_create_mutex(&s_touch_dev);
    if (err != ESP_OK) return err;

    err = i2c_dev_check_present(&s_touch_dev);
    if (err != ESP_OK) return err;
    s_touch_dev_ready = true;

    ESP_ERROR_CHECK_WITHOUT_ABORT(cst820_write(0xFE, 0xFF));

    s_indev = lv_indev_create();
    if (!s_indev) return ESP_ERR_NO_MEM;
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read);
    if (s_display) {
        lv_indev_set_display(s_indev, s_display);
    }

    ESP_LOGI(TAG, "%s touch ready: %s i2c=0x%02x sda=%d scl=%d rst=%d int=%d",
             BOARD_MODEL, TOUCH_DRIVER_NAME, TOUCH_I2C_ADDR_CST820,
             TOUCH_I2C_SDA_GPIO, TOUCH_I2C_SCL_GPIO, TOUCH_RST_GPIO, TOUCH_INT_GPIO);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t board_backlight_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
#if DISPLAY_BACKLIGHT_GPIO < 0
    return ESP_ERR_NOT_SUPPORTED;
#elif defined(CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN)
    esp_err_t err = backlight_init();
    if (err != ESP_OK) return err;
    uint32_t duty = ((uint32_t)percent * LCD_BACKLIGHT_DUTY_MAX) / 100U;
    err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (err != ESP_OK) return err;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void board_touch_set_activity_cb(board_touch_activity_cb_t cb)
{
#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
    s_touch_activity_cb = cb;
#else
    (void)cb;
#endif
}
