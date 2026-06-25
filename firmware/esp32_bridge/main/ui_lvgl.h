#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    UI_LVGL_SCREEN_REMOTE = 0,
    UI_LVGL_SCREEN_STATUS,
    UI_LVGL_SCREEN_SENSORS,
    UI_LVGL_SCREEN_SETTINGS,
} ui_lvgl_screen_t;

esp_err_t ui_lvgl_start(void);
bool ui_lvgl_display_available(void);
bool ui_lvgl_touch_available(void);
uint8_t ui_lvgl_brightness_percent(void);
uint16_t ui_lvgl_screen_timeout_s(void);
const char *ui_lvgl_current_screen(void);
const char *ui_lvgl_theme(void);
bool ui_lvgl_backlight_on(void);
bool ui_lvgl_remote_buttons_enabled(void);
const char *ui_lvgl_remote_disabled_reason(void);
esp_err_t ui_lvgl_set_screen(const char *screen_name);
esp_err_t ui_lvgl_press_button(const char *button_name);
void ui_lvgl_note_activity(void);
esp_err_t ui_lvgl_set_brightness_percent(uint8_t percent);
esp_err_t ui_lvgl_set_screen_timeout_s(uint16_t seconds);
esp_err_t ui_lvgl_set_theme(const char *theme);
