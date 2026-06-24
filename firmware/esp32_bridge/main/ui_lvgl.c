#include "ui_lvgl.h"
#include "board_config.h"
#include "bridge_buttons.h"
#include "bridge_state.h"
#include "command_queue.h"
#include "mqtt_ha.h"
#include "pins.h"
#include "rules.h"
#include "uart_link.h"
#include "wifi_mgr.h"
#include "ac_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
#include "lvgl.h"
#endif

#define UI_NVS_NS "ui"
#define UI_TIMEOUT_DEFAULT_S 60
#define UI_TIMEOUT_MIN_S 10
#define UI_TIMEOUT_MAX_S 600
#define UI_LVGL_TICK_MS 5

static const char *TAG = "ui";
static bool s_display_available;
static bool s_touch_available;
static uint8_t s_brightness_percent = 100;
static uint16_t s_screen_timeout_s = UI_TIMEOUT_DEFAULT_S;
static bool s_backlight_on;
static int64_t s_last_activity_us;
static char s_screen[12] = "remote";
static char s_theme[8] = "dark";

#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
#define UI_TILE_COUNT 6

typedef struct {
    lv_obj_t *root;
    lv_obj_t *tileview;
    lv_obj_t *remote;
    lv_obj_t *sync;
    lv_obj_t *sensors;
    lv_obj_t *network;
    lv_obj_t *rules;
    lv_obj_t *settings;
    lv_obj_t *tiles[UI_TILE_COUNT];
    lv_obj_t *page_dots[UI_TILE_COUNT];
    lv_obj_t *footer;
    lv_obj_t *page_label;
    lv_obj_t *home_button;
    lv_obj_t *top_nrf[UI_TILE_COUNT];
    lv_obj_t *top_wifi[UI_TILE_COUNT];
    lv_obj_t *top_mqtt[UI_TILE_COUNT];
    lv_obj_t *top_env[UI_TILE_COUNT];
    lv_obj_t *top_nrf_dot[UI_TILE_COUNT];
    lv_obj_t *top_wifi_dot[UI_TILE_COUNT];
    lv_obj_t *top_mqtt_dot[UI_TILE_COUNT];
    lv_obj_t *remote_model;
    lv_obj_t *remote_model_mode;
    lv_obj_t *remote_model_temp;
    lv_obj_t *remote_model_fan;
    lv_obj_t *remote_model_flags;
    lv_obj_t *remote_model_air;
    lv_obj_t *remote_disabled;
    lv_obj_t *remote_last;
    lv_obj_t *remote_buttons[9];
    lv_obj_t *sync_body;
    lv_obj_t *sync_mute;
    lv_obj_t *sync_state;
    lv_obj_t *sync_link;
    lv_obj_t *sync_last;
    lv_obj_t *sync_pair_mode;
    lv_obj_t *network_body;
    lv_obj_t *rules_body;
    lv_obj_t *rule_dot[3];
    lv_obj_t *rule_top[3];
    lv_obj_t *rule_target[3];
    lv_obj_t *rule_bottom[3];
    lv_obj_t *sensor_body;
    lv_obj_t *sensor_value[6];
    lv_obj_t *sensor_meta[6];
    lv_obj_t *pin_body;
    lv_obj_t *pin_value[8];
    lv_obj_t *settings_url;
    lv_obj_t *display_brightness_value;
    lv_obj_t *display_sleep_value;
    lv_obj_t *brightness_slider;
    lv_obj_t *timeout_dropdown;
    lv_obj_t *theme_dropdown;
} ui_objects_t;

static ui_objects_t s_ui;
static bool s_tile_built[UI_TILE_COUNT];
static bool s_lvgl_ready;
static SemaphoreHandle_t s_lvgl_mutex;
static esp_timer_handle_t s_lvgl_tick;
static char s_wifi_scan_summary[192];
static int64_t s_wifi_scan_until_us;
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
static uint32_t s_ui_diag_loop_count;
static int64_t s_ui_diag_last_heartbeat_us;
static volatile int64_t s_ui_diag_last_progress_us;
static volatile int64_t s_ui_diag_last_touch_us;
static volatile uint32_t s_ui_diag_last_loop_count;
static volatile int32_t s_ui_diag_last_scroll_x;
static volatile int32_t s_ui_diag_last_width;
static volatile uint8_t s_ui_diag_last_tile;
static volatile uint8_t s_ui_diag_last_built;
static volatile bool s_ui_diag_last_pressed;
static volatile bool s_ui_diag_last_scrolling;
static const char * volatile s_ui_diag_phase = "init";
static const char * volatile s_ui_diag_last_event = "boot";
#endif
static void build_tile_if_needed(uint8_t index);
static bool pointer_is_pressed(void);
#endif

static bool valid_screen_name(const char *screen_name)
{
    return screen_name &&
           (!strcmp(screen_name, "remote") ||
            !strcmp(screen_name, "status") ||
            !strcmp(screen_name, "sensors") ||
            !strcmp(screen_name, "settings"));
}

static void publish_display_state(void)
{
    bridge_state_update_display(s_display_available, s_touch_available, s_brightness_percent,
                                s_backlight_on, s_screen, s_theme);
}

static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t brightness = s_brightness_percent;
    uint16_t timeout = s_screen_timeout_s;
    size_t theme_len = sizeof(s_theme);
    if (nvs_get_u8(h, "bright", &brightness) == ESP_OK && brightness <= 100) {
        s_brightness_percent = brightness;
    }
    if (nvs_get_u16(h, "timeout", &timeout) == ESP_OK &&
        timeout >= UI_TIMEOUT_MIN_S && timeout <= UI_TIMEOUT_MAX_S) {
        s_screen_timeout_s = timeout;
    }
    if (nvs_get_str(h, "theme", s_theme, &theme_len) != ESP_OK ||
        (strcmp(s_theme, "dark") != 0 && strcmp(s_theme, "light") != 0)) {
        strlcpy(s_theme, "dark", sizeof(s_theme));
    }
    nvs_close(h);
}

static esp_err_t save_settings(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(UI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "bright", s_brightness_percent);
    if (err == ESP_OK) err = nvs_set_u16(h, "timeout", s_screen_timeout_s);
    if (err == ESP_OK) err = nvs_set_str(h, "theme", s_theme);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
static void lvgl_lock(void)
{
    if (s_lvgl_mutex) xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
}

static void lvgl_unlock(void)
{
    if (s_lvgl_mutex) xSemaphoreGiveRecursive(s_lvgl_mutex);
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(UI_LVGL_TICK_MS);
}

typedef enum {
    UI_FONT_TINY = 0,
    UI_FONT_META,
    UI_FONT_LABEL,
    UI_FONT_PRIMARY,
    UI_FONT_TITLE,
} ui_font_role_t;

static const lv_font_t *ui_font(ui_font_role_t role)
{
    switch (role) {
    case UI_FONT_TITLE:
#if LV_FONT_MONTSERRAT_24
        return &lv_font_montserrat_24;
#endif
        break;
    case UI_FONT_PRIMARY:
#if LV_FONT_MONTSERRAT_20
        return &lv_font_montserrat_20;
#endif
        break;
    case UI_FONT_LABEL:
#if LV_FONT_MONTSERRAT_16
        return &lv_font_montserrat_16;
#endif
        break;
    case UI_FONT_META:
#if LV_FONT_MONTSERRAT_14
        return &lv_font_montserrat_14;
#endif
        break;
    case UI_FONT_TINY:
    default:
#if LV_FONT_MONTSERRAT_10
        return &lv_font_montserrat_10;
#elif LV_FONT_MONTSERRAT_14
        return &lv_font_montserrat_14;
#endif
        break;
    }
    return LV_FONT_DEFAULT;
}

static lv_color_t theme_bg(void)
{
    return !strcmp(s_theme, "light") ? lv_color_hex(0xf4f6f8) : lv_color_hex(0x101827);
}

static lv_color_t theme_panel(void)
{
    return !strcmp(s_theme, "light") ? lv_color_hex(0xffffff) : lv_color_hex(0x182435);
}

static lv_color_t theme_button(void)
{
    return !strcmp(s_theme, "light") ? lv_color_hex(0x0f766e) : lv_color_hex(0x14b8a6);
}

static lv_color_t theme_button_alt(void)
{
    return !strcmp(s_theme, "light") ? lv_color_hex(0xe7edf3) : lv_color_hex(0x26302a);
}

static lv_color_t theme_fg(void)
{
    return !strcmp(s_theme, "light") ? lv_color_hex(0x18212b) : lv_color_hex(0xf5faf7);
}

static lv_color_t theme_muted(void)
{
    return !strcmp(s_theme, "light") ? lv_color_hex(0x5a6673) : lv_color_hex(0x9aa89f);
}

static void fixed_no_scroll(lv_obj_t *obj)
{
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void style_screen(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, theme_bg(), 0);
    lv_obj_set_style_text_color(screen, theme_fg(), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    fixed_no_scroll(screen);
}

static void style_fixed_panel(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, theme_panel(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, !strcmp(s_theme, "light") ? lv_color_hex(0xd5dde6) : lv_color_hex(0x344039), 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    fixed_no_scroll(obj);
}

static void activity_event(lv_event_t *e)
{
    ui_lvgl_note_activity();
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, ui_font_role_t role)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(obj, ui_font(role), 0);
    lv_obj_set_style_text_color(obj, theme_fg(), 0);
    fixed_no_scroll(obj);
    return obj;
}

static void label_set_text_if_changed(lv_obj_t *obj, const char *text)
{
    const char *current = lv_label_get_text(obj);
    if (!current || strcmp(current, text) != 0) {
        lv_label_set_text(obj, text);
    }
}

static const lv_style_transition_dsc_t *button_transition(void)
{
    static bool initialized;
    static lv_style_transition_dsc_t transition;
    static const lv_style_prop_t props[] = {
        LV_STYLE_TRANSFORM_SCALE_X,
        LV_STYLE_TRANSFORM_SCALE_Y,
        LV_STYLE_TRANSLATE_Y,
        LV_STYLE_BG_OPA,
        LV_STYLE_SHADOW_OPA,
        0
    };
    if (!initialized) {
        lv_style_transition_dsc_init(&transition, props, lv_anim_path_ease_out, 140, 0, NULL);
        initialized = true;
    }
    return &transition;
}

static void style_button_touch_feedback(lv_obj_t *btn)
{
    lv_obj_set_style_transform_scale(btn, 248, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 10, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_20, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_style_transition(btn, button_transition(), 0);
    lv_obj_set_style_transition(btn, button_transition(), LV_STATE_PRESSED);
}

static void remote_activation_outline_cb(void *obj, int32_t v)
{
    lv_obj_set_style_outline_width((lv_obj_t *)obj, v / 32, 0);
    lv_obj_set_style_outline_opa((lv_obj_t *)obj, v, 0);
}

static void remote_activation_anim(lv_obj_t *btn)
{
    if (!btn) return;
    lv_anim_del(btn, remote_activation_outline_cb);
    lv_obj_set_style_outline_color(btn, lv_color_hex(0x67e8f9), 0);
    lv_obj_set_style_outline_pad(btn, 1, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, btn);
    lv_anim_set_exec_cb(&a, remote_activation_outline_cb);
    lv_anim_set_values(&a, 180, 0);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int w, int h,
                        ui_font_role_t role, lv_color_t bg, lv_color_t fg,
                        lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x506178), LV_STATE_DISABLED);
    lv_obj_set_style_opa(btn, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x3a453e), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x56635b), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x344039), LV_STATE_DISABLED);
    uint32_t bg_rgb = lv_color_to_u32(bg) & 0x00ffffffu;
    if (bg_rgb == (lv_color_to_u32(theme_button()) & 0x00ffffffu)) {
        lv_obj_set_style_border_color(btn, lv_color_hex(0x0f766e), 0);
    } else if (bg_rgb == 0x00ef4444u) {
        lv_obj_set_style_border_color(btn, lv_color_hex(0xb91c1c), 0);
    } else if (bg_rgb == 0x003a2a12u) {
        lv_obj_set_style_border_color(btn, lv_color_hex(0x8a5a08), 0);
    }
    style_button_touch_feedback(btn);
    fixed_no_scroll(btn);
    lv_obj_add_event_cb(btn, activity_event, LV_EVENT_PRESSED, NULL);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(lbl, w - 8);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl, ui_font(role), 0);
    lv_obj_set_style_text_color(lbl, fg, 0);
    lv_obj_center(lbl);
    return btn;
}

static lv_obj_t *meta_badge(lv_obj_t *parent, const char *text, int x, int w)
{
    lv_obj_t *badge = label(parent, text, UI_FONT_TINY);
    lv_obj_set_pos(badge, x, 7);
    lv_obj_set_size(badge, w, 18);
    lv_obj_set_style_text_color(badge, theme_muted(), 0);
    return badge;
}

static lv_obj_t *status_dot(lv_obj_t *parent, int x)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_pos(dot, x, 10);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x68746d), 0);
    fixed_no_scroll(dot);
    return dot;
}

static lv_obj_t *top_strip(lv_obj_t *tile, uint8_t index)
{
    lv_obj_t *strip = lv_obj_create(tile);
    lv_obj_set_pos(strip, 6, 5);
    lv_obj_set_size(strip, 308, 28);
    style_fixed_panel(strip);
    lv_obj_set_style_radius(strip, 7, 0);
    lv_obj_set_style_bg_color(strip, !strcmp(s_theme, "light") ? lv_color_hex(0xffffff) : lv_color_hex(0x101511), 0);
    lv_obj_set_style_border_color(strip, !strcmp(s_theme, "light") ? lv_color_hex(0xd5dde6) : lv_color_hex(0x222b26), 0);
    s_ui.top_nrf_dot[index] = status_dot(strip, 6);
    s_ui.top_wifi_dot[index] = status_dot(strip, 72);
    s_ui.top_mqtt_dot[index] = status_dot(strip, 148);
    s_ui.top_nrf[index] = meta_badge(strip, "nRF --", 16, 52);
    s_ui.top_wifi[index] = meta_badge(strip, "Wi-Fi --", 82, 62);
    s_ui.top_mqtt[index] = meta_badge(strip, "MQTT --", 158, 56);
    s_ui.top_env[index] = meta_badge(strip, "--C --%", 218, 84);
    return strip;
}

static lv_obj_t *compact_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    style_fixed_panel(card);
    lv_obj_set_style_bg_color(card, theme_panel(), 0);
    return card;
}

static lv_obj_t *small_row(lv_obj_t *parent, const char *key, const char *value,
                           int x, int y, int w)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_pos(row, x, y);
    lv_obj_set_size(row, w, 20);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_bg_color(row, !strcmp(s_theme, "light") ? lv_color_hex(0xf2f5f8) : lv_color_hex(0x101511), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, !strcmp(s_theme, "light") ? lv_color_hex(0xd5dde6) : lv_color_hex(0x222b26), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    fixed_no_scroll(row);

    lv_obj_t *k = label(row, key, UI_FONT_TINY);
    lv_obj_set_pos(k, 6, 3);
    lv_obj_set_size(k, 58, 13);
    lv_obj_set_style_text_color(k, theme_muted(), 0);

    lv_obj_t *v = label(row, value, UI_FONT_TINY);
    lv_obj_set_pos(v, 68, 3);
    lv_obj_set_size(v, w - 74, 13);
    lv_obj_set_style_text_color(v, theme_fg(), 0);
    return v;
}

static lv_obj_t *metric_card(lv_obj_t *parent, const char *title, const char *meta,
                             int x, int y, int w, int h)
{
    lv_obj_t *card = compact_card(parent, x, y, w, h);
    lv_obj_t *k = label(card, title, UI_FONT_TINY);
    lv_obj_set_pos(k, 6, 5);
    lv_obj_set_size(k, w - 12, 12);
    lv_obj_set_style_text_color(k, theme_muted(), 0);
    lv_obj_t *v = label(card, "--", UI_FONT_PRIMARY);
    lv_obj_set_pos(v, 6, 19);
    lv_obj_set_size(v, w - 12, 23);
    lv_obj_t *m = label(card, meta, UI_FONT_TINY);
    lv_obj_set_pos(m, 6, 42);
    lv_obj_set_size(m, w - 12, 11);
    lv_obj_set_style_text_color(m, theme_muted(), 0);
    return v;
}

static lv_obj_t *pin_tile(lv_obj_t *parent, const char *title, int x, int y)
{
    lv_obj_t *card = compact_card(parent, x, y, 28, 22);
    lv_obj_set_style_radius(card, 7, 0);
    lv_obj_t *k = label(card, title, UI_FONT_TINY);
    lv_obj_set_pos(k, 3, 2);
    lv_obj_set_size(k, 22, 8);
    lv_obj_set_style_text_color(k, theme_muted(), 0);
    lv_obj_t *v = label(card, "--", UI_FONT_TINY);
    lv_obj_set_pos(v, 3, 10);
    lv_obj_set_size(v, 22, 10);
    lv_obj_set_style_text_color(v, theme_fg(), 0);
    return v;
}

static lv_obj_t *rule_card(lv_obj_t *parent, uint8_t index, int y)
{
    lv_obj_t *card = compact_card(parent, 8, y, 202, 34);
    s_ui.rule_dot[index] = lv_obj_create(card);
    lv_obj_set_pos(s_ui.rule_dot[index], 7, 8);
    lv_obj_set_size(s_ui.rule_dot[index], 6, 6);
    lv_obj_set_style_radius(s_ui.rule_dot[index], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ui.rule_dot[index], 0, 0);
    lv_obj_set_style_bg_color(s_ui.rule_dot[index], lv_color_hex(0x68746d), 0);
    lv_obj_set_style_bg_opa(s_ui.rule_dot[index], LV_OPA_COVER, 0);
    fixed_no_scroll(s_ui.rule_dot[index]);
    s_ui.rule_top[index] = label(card, "--", UI_FONT_TINY);
    lv_obj_set_pos(s_ui.rule_top[index], 18, 4);
    lv_obj_set_size(s_ui.rule_top[index], 104, 13);
    s_ui.rule_target[index] = label(card, "--", UI_FONT_TINY);
    lv_obj_set_pos(s_ui.rule_target[index], 126, 4);
    lv_obj_set_size(s_ui.rule_target[index], 68, 13);
    lv_obj_set_style_text_align(s_ui.rule_target[index], LV_TEXT_ALIGN_RIGHT, 0);
    s_ui.rule_bottom[index] = label(card, "--", UI_FONT_TINY);
    lv_obj_set_pos(s_ui.rule_bottom[index], 7, 17);
    lv_obj_set_size(s_ui.rule_bottom[index], 188, 12);
    lv_obj_set_style_text_color(s_ui.rule_bottom[index], theme_muted(), 0);
    return card;
}

static void nav_event(lv_event_t *e)
{
    const char *screen = (const char *)lv_event_get_user_data(e);
    ui_lvgl_set_screen(screen);
}

static void press_event(lv_event_t *e)
{
    const char *button_name = (const char *)lv_event_get_user_data(e);
    esp_err_t err = ui_lvgl_press_button(button_name);
    if (!s_ui.remote_last) return;
    if (err == ESP_OK) {
        remote_activation_anim((lv_obj_t *)lv_event_get_target(e));
        char text[40];
        snprintf(text, sizeof(text), "Queued: %s", button_name ? button_name : "");
        lv_label_set_text(s_ui.remote_last, text);
    } else if (err == ESP_ERR_INVALID_STATE) {
        lv_label_set_text(s_ui.remote_last, ui_lvgl_remote_disabled_reason());
    } else if (err == ESP_ERR_TIMEOUT) {
        lv_label_set_text(s_ui.remote_last, "Command queue full");
    } else {
        lv_label_set_text(s_ui.remote_last, "Invalid command");
    }
}

static void brightness_event(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    ui_lvgl_set_brightness_percent((uint8_t)lv_slider_get_value(slider));
}

static uint16_t timeout_from_index(uint16_t index)
{
    static const uint16_t values[] = {10, 30, 60, 120, 300, 600};
    if (index >= sizeof(values) / sizeof(values[0])) return UI_TIMEOUT_DEFAULT_S;
    return values[index];
}

static uint16_t __attribute__((unused)) timeout_to_index(uint16_t seconds)
{
    static const uint16_t values[] = {10, 30, 60, 120, 300, 600};
    for (uint16_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (values[i] == seconds) return i;
    }
    return 2;
}

static void __attribute__((unused)) timeout_event(lv_event_t *e)
{
    lv_obj_t *dropdown = (lv_obj_t *)lv_event_get_target(e);
    ui_lvgl_set_screen_timeout_s(timeout_from_index(lv_dropdown_get_selected(dropdown)));
}

static void __attribute__((unused)) theme_event(lv_event_t *e)
{
    lv_obj_t *dropdown = (lv_obj_t *)lv_event_get_target(e);
    ui_lvgl_set_theme(lv_dropdown_get_selected(dropdown) == 0 ? "dark" : "light");
}

static void reboot_event(lv_event_t *e)
{
    ui_lvgl_note_activity();
    esp_restart();
}

static void __attribute__((unused)) reset_wifi_event(lv_event_t *e)
{
    (void)e;
    ui_lvgl_note_activity();
    wifi_mgr_reset_provisioning();
}

static void sync_event(lv_event_t *e)
{
    (void)e;
    ui_lvgl_note_activity();
    uart_link_mute(30);
}

static void pair_event(lv_event_t *e)
{
    bool unpair = (bool)(uintptr_t)lv_event_get_user_data(e);
    ui_lvgl_note_activity();
    uart_link_pairing(unpair);
}

static void scan_event(lv_event_t *e)
{
    (void)e;
    ui_lvgl_note_activity();
    wm_ap_t aps[3];
    int n = wifi_mgr_scan(aps, 3);
    if (!s_ui.network_body) return;
    char text[192];
    if (n <= 0) {
        strlcpy(s_wifi_scan_summary, "No networks", sizeof(s_wifi_scan_summary));
        s_wifi_scan_until_us = esp_timer_get_time() + 10000000LL;
        lv_label_set_text(s_ui.network_body, s_wifi_scan_summary);
        return;
    }
    snprintf(text, sizeof(text), "%s %ddBm", aps[0].ssid, aps[0].rssi);
    strlcpy(s_wifi_scan_summary, text, sizeof(s_wifi_scan_summary));
    s_wifi_scan_until_us = esp_timer_get_time() + 10000000LL;
    lv_label_set_text(s_ui.network_body, s_wifi_scan_summary);
}

static void add_rule_event(lv_event_t *e)
{
    (void)e;
    ui_lvgl_note_activity();
    rule_t rs[RULES_MAX];
    int n = rules_get(rs, RULES_MAX);
    if (n < 0) n = 0;
    if (n >= RULES_MAX) return;
    rs[n].enabled = true;
    rs[n].cond = RULE_COND_PRESENCE;
    rs[n].duration_s = 5 * 60;
    strlcpy(rs[n].target, "cool", sizeof(rs[n].target));
    rules_set(rs, n + 1);
}

static void save_rules_event(lv_event_t *e)
{
    (void)e;
    ui_lvgl_note_activity();
    rule_t rs[RULES_MAX];
    int n = rules_get(rs, RULES_MAX);
    if (n < 0) n = 0;
    rules_set(rs, n);
}

static void save_pins_event(lv_event_t *e)
{
    (void)e;
    ui_lvgl_note_activity();
    pins_save(pins_get());
}

static uint8_t screen_index_for_name(const char *screen_name)
{
    if (!strcmp(screen_name, "sensors")) return 2;
    if (!strcmp(screen_name, "status")) return 5;
    if (!strcmp(screen_name, "settings")) return 5;
    return 0;
}

static const char *screen_name_for_index(uint8_t index)
{
    static const char *names[] = {"remote", "remote", "sensors", "settings", "settings", "settings"};
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "remote";
}

static void update_page_dots(uint8_t active)
{
    static const char *page_names[] = {
        "Remote 1/6", "Sync 2/6", "Sensors 3/6", "Network 4/6", "Rules 5/6", "System 6/6"
    };
    for (uint8_t i = 0; i < UI_TILE_COUNT; i++) {
        if (!s_ui.page_dots[i]) continue;
        lv_obj_set_style_bg_color(s_ui.page_dots[i],
                                  i == active ? theme_button() : theme_muted(), 0);
        lv_obj_set_style_bg_opa(s_ui.page_dots[i],
                                i == active ? LV_OPA_COVER : LV_OPA_50, 0);
    }
    if (s_ui.page_label && active < UI_TILE_COUNT) {
        lv_label_set_text(s_ui.page_label, page_names[active]);
    }
}

#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
static uint8_t built_tile_mask(void)
{
    uint8_t mask = 0;
    for (uint8_t i = 0; i < UI_TILE_COUNT; i++) {
        if (s_tile_built[i]) mask |= (uint8_t)(1U << i);
    }
    return mask;
}

static const char *lv_event_name(lv_event_code_t code)
{
    switch (code) {
    case LV_EVENT_SCROLL_BEGIN:
        return "scroll_begin";
    case LV_EVENT_SCROLL_END:
        return "scroll_end";
    case LV_EVENT_VALUE_CHANGED:
        return "value_changed";
    case LV_EVENT_PRESSED:
        return "pressed";
    case LV_EVENT_PRESSING:
        return "pressing";
    case LV_EVENT_RELEASED:
        return "released";
    default:
        return "event";
    }
}

static void log_tileview_diag(const char *reason, uint8_t index)
{
    int32_t scroll_x = s_ui.tileview ? lv_obj_get_scroll_x(s_ui.tileview) : 0;
    int32_t scroll_y = s_ui.tileview ? lv_obj_get_scroll_y(s_ui.tileview) : 0;
    int32_t width = s_ui.tileview ? lv_obj_get_content_width(s_ui.tileview) : 0;
    bool pressed = pointer_is_pressed();
    bool scrolling = s_ui.tileview && lv_obj_is_scrolling(s_ui.tileview);
    s_ui_diag_last_event = reason;
    s_ui_diag_last_tile = index;
    s_ui_diag_last_scroll_x = scroll_x;
    s_ui_diag_last_width = width;
    s_ui_diag_last_pressed = pressed;
    s_ui_diag_last_scrolling = scrolling;
    s_ui_diag_last_built = built_tile_mask();
    s_ui_diag_last_progress_us = esp_timer_get_time();
    ESP_LOGI(TAG, "diag %s tile=%u screen=%s scroll=%ld,%ld width=%ld pressed=%d scrolling=%d built=0x%02x heap=%lu",
             reason,
             (unsigned)index,
             s_screen,
             (long)scroll_x,
             (long)scroll_y,
             (long)width,
             pressed ? 1 : 0,
             scrolling ? 1 : 0,
             (unsigned)s_ui_diag_last_built,
             (unsigned long)esp_get_free_heap_size());
}
#endif

static bool active_tile_index(uint8_t *index)
{
    if (!s_ui.tileview || !index) return false;
    lv_obj_t *active = lv_tileview_get_tile_active(s_ui.tileview);
    for (uint8_t i = 0; i < sizeof(s_ui.tiles) / sizeof(s_ui.tiles[0]); i++) {
        if (active == s_ui.tiles[i]) {
            *index = i;
            return true;
        }
    }
    return false;
}

static void build_tile_neighbors(uint8_t index)
{
    if (index >= UI_TILE_COUNT) return;
    if (index > 0) {
        build_tile_if_needed((uint8_t)(index - 1));
    }
    build_tile_if_needed(index);
    if (index + 1 < UI_TILE_COUNT) {
        build_tile_if_needed((uint8_t)(index + 1));
    }
}

static bool pointer_is_pressed(void)
{
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
            lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
            s_ui_diag_last_touch_us = esp_timer_get_time();
#endif
            return true;
        }
        indev = lv_indev_get_next(indev);
    }
    return false;
}

static void recover_tileview_snap_locked(void)
{
    if (!s_ui.tileview || pointer_is_pressed()) return;

    int32_t width = lv_obj_get_content_width(s_ui.tileview);
    if (width <= 0) return;

    int32_t scroll_x = lv_obj_get_scroll_x(s_ui.tileview);
    int32_t max_scroll_x = (int32_t)(UI_TILE_COUNT - 1) * width;
    if (scroll_x < 0) scroll_x = 0;
    if (scroll_x > max_scroll_x) scroll_x = max_scroll_x;

    uint8_t index = (uint8_t)((scroll_x + (width / 2)) / width);
    if (index >= UI_TILE_COUNT) index = UI_TILE_COUNT - 1;

    int32_t target_x = (int32_t)index * width;
    int32_t delta = scroll_x > target_x ? scroll_x - target_x : target_x - scroll_x;
    if (delta <= 2) return;

#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
    ESP_LOGW(TAG, "diag snap_recover from=%ld target=%ld delta=%ld tile=%u",
             (long)scroll_x, (long)target_x, (long)delta, (unsigned)index);
#endif
    build_tile_neighbors(index);
    lv_tileview_set_tile_by_index(s_ui.tileview, index, 0, LV_ANIM_OFF);
    strlcpy(s_screen, screen_name_for_index(index), sizeof(s_screen));
    update_page_dots(index);
    publish_display_state();
}

static void tileview_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    uint8_t i = 0;
    if (!active_tile_index(&i)) return;
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
    ESP_LOGI(TAG, "diag tile_event %s active=%u", lv_event_name(code), (unsigned)i);
    log_tileview_diag(lv_event_name(code), i);
#endif
    if (code == LV_EVENT_SCROLL_BEGIN) {
        build_tile_neighbors(i);
        return;
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        strlcpy(s_screen, screen_name_for_index(i), sizeof(s_screen));
        update_page_dots(i);
        build_tile_neighbors(i);
        publish_display_state();
    }
}

static lv_obj_t *make_tile(uint8_t col)
{
    lv_obj_t *tile = lv_tileview_add_tile(s_ui.tileview, col, 0, LV_DIR_HOR);
    lv_obj_set_size(tile, DISPLAY_H_RES, DISPLAY_V_RES);
    lv_obj_set_style_bg_color(tile, theme_bg(), 0);
    lv_obj_set_style_text_color(tile, theme_fg(), 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    fixed_no_scroll(tile);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, activity_event, LV_EVENT_PRESSED, NULL);
    return tile;
}

static lv_obj_t **remote_button_slot(const char *name)
{
    size_t nbuttons = 0;
    const bridge_button_def_t *buttons = bridge_buttons(&nbuttons);
    for (size_t i = 0; i < nbuttons && i < sizeof(s_ui.remote_buttons) / sizeof(s_ui.remote_buttons[0]); i++) {
        if (!strcmp(buttons[i].name, name)) return &s_ui.remote_buttons[i];
    }
    return NULL;
}

static const char *remote_button_text(const char *name, const char *fallback)
{
    if (!strcmp(name, BRIDGE_BUTTON_POWER)) return LV_SYMBOL_POWER;
    if (!strcmp(name, BRIDGE_BUTTON_UP)) return LV_SYMBOL_UP;
    if (!strcmp(name, BRIDGE_BUTTON_DOWN)) return LV_SYMBOL_DOWN;
    if (!strcmp(name, BRIDGE_BUTTON_ECO)) return "Eco";
    if (!strcmp(name, BRIDGE_BUTTON_MODE)) return "MODE";
    return fallback;
}

static bool remote_feature_text(const char *name, const char **title, const char **meta, lv_color_t *accent)
{
    if (!strcmp(name, BRIDGE_BUTTON_FLAP)) {
        *title = "Swing";
        *meta = "Flap";
        *accent = lv_color_hex(0x38bdf8);
        return true;
    }
    if (!strcmp(name, BRIDGE_BUTTON_SILENT)) {
        *title = "Quiet";
        *meta = "Silent";
        *accent = lv_color_hex(0xa78bfa);
        return true;
    }
    if (!strcmp(name, BRIDGE_BUTTON_FAN)) {
        *title = "Fan";
        *meta = "Speed";
        *accent = lv_color_hex(0x34d399);
        return true;
    }
    if (!strcmp(name, BRIDGE_BUTTON_TIMER)) {
        *title = "Timer";
        *meta = "Delay";
        *accent = lv_color_hex(0xfbbf24);
        return true;
    }
    return false;
}

static lv_obj_t *remote_feature_action(lv_obj_t *parent, const char *name,
                                       int x, int y, int w, int h)
{
    const char *title = "";
    const char *meta = "";
    lv_color_t accent = theme_button();
    remote_feature_text(name, &title, &meta, &accent);

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 13, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x0d1424), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x26354b), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x121722), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, LV_STATE_DISABLED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, accent, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_60, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xffffff), LV_STATE_PRESSED);
    lv_obj_set_style_opa(btn, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_pad_all(btn, 0, 0);
    style_button_touch_feedback(btn);
    fixed_no_scroll(btn);
    lv_obj_add_event_cb(btn, activity_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn, press_event, LV_EVENT_CLICKED, (void *)name);

    lv_obj_t *bar = lv_obj_create(btn);
    lv_obj_set_pos(bar, 6, 7);
    lv_obj_set_size(bar, 3, h - 14);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, accent, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    fixed_no_scroll(bar);

    lv_obj_t *top = lv_label_create(btn);
    lv_label_set_text(top, title);
    lv_label_set_long_mode(top, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(top, 13, 6);
    lv_obj_set_size(top, w - 17, 16);
    lv_obj_set_style_text_font(top, ui_font(UI_FONT_META), 0);
    lv_obj_set_style_text_color(top, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(top, LV_TEXT_ALIGN_CENTER, 0);
    fixed_no_scroll(top);

    lv_obj_t *bottom = lv_label_create(btn);
    lv_label_set_text(bottom, meta);
    lv_label_set_long_mode(bottom, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(bottom, 13, 23);
    lv_obj_set_size(bottom, w - 17, 12);
    lv_obj_set_style_text_font(bottom, ui_font(UI_FONT_TINY), 0);
    lv_obj_set_style_text_color(bottom, accent, 0);
    lv_obj_set_style_text_align(bottom, LV_TEXT_ALIGN_CENTER, 0);
    fixed_no_scroll(bottom);

    lv_obj_t **slot = remote_button_slot(name);
    if (slot) *slot = btn;
    return btn;
}

static lv_obj_t *remote_action(lv_obj_t *parent, const char *name, const char *label_text,
                               int x, int y, int w, int h, bool primary)
{
    const char *feature_title = NULL;
    const char *feature_meta = NULL;
    lv_color_t feature_accent = theme_button();
    if (remote_feature_text(name, &feature_title, &feature_meta, &feature_accent)) {
        return remote_feature_action(parent, name, x, y, w, h);
    }

    lv_obj_t *btn = button(parent, remote_button_text(name, label_text), w, h,
                           primary ? UI_FONT_PRIMARY : UI_FONT_LABEL,
                           primary ? theme_button() : theme_button_alt(),
                           primary || strcmp(s_theme, "light") ? lv_color_hex(0xffffff) : theme_fg(),
                           press_event, (void *)name);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xdce7e1), 0);
    lv_obj_set_style_bg_color(btn, primary ? lv_color_hex(0x172036) : lv_color_hex(0x080c16), 0);
    lv_obj_set_style_bg_opa(btn, primary ? LV_OPA_80 : LV_OPA_70, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x283344), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x121722), LV_STATE_DISABLED);
    lv_obj_set_style_radius(btn, h >= 44 ? 22 : 8, 0);
    if (!strcmp(name, BRIDGE_BUTTON_POWER)) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x4c1d2b), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
    }
    lv_obj_t **slot = remote_button_slot(name);
    if (slot) *slot = btn;
    return btn;
}

static lv_obj_t *remote_temp_half(lv_obj_t *parent, const char *name, int x, int w)
{
    lv_obj_t *btn = button(parent, remote_button_text(name, ""), w, 44, UI_FONT_PRIMARY,
                           lv_color_hex(0x080c16), lv_color_hex(0xf7fbff),
                           press_event, (void *)name);
    lv_obj_set_pos(btn, x, 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, x ? 1 : 0, 0);
    lv_obj_set_style_border_side(btn, x ? LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x5f6978), 0);
    lv_obj_t **slot = remote_button_slot(name);
    if (slot) *slot = btn;
    return btn;
}

static lv_obj_t *remote_model_cell(lv_obj_t *parent, const char *text, int x, int w, bool emph)
{
    lv_obj_t *cell = label(parent, text, emph ? UI_FONT_META : UI_FONT_TINY);
    lv_obj_set_pos(cell, x, emph ? 3 : 6);
    lv_obj_set_size(cell, w, emph ? 18 : 12);
    lv_obj_set_style_text_align(cell, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(cell, emph ? lv_color_hex(0xffffff) : lv_color_hex(0xe7edf5), 0);
    return cell;
}

static const char *remote_mode_short(const ac_state_t *ac)
{
    if (!ac->on) return "OFF";
    switch (ac->mode) {
    case AC_MODE_DRY:
        return "DRY";
    case AC_MODE_FAN:
        return "FAN";
    case AC_MODE_COOL:
    default:
        return "COOL";
    }
}

static const char *remote_fan_short(ac_fan_t fan)
{
    switch (fan) {
    case AC_FAN_MIN:
        return "F1";
    case AC_FAN_MED:
        return "F2";
    case AC_FAN_MAX:
        return "F3";
    case AC_FAN_AUTO:
    default:
        return "FA";
    }
}

static void build_remote_screen(void)
{
    top_strip(s_ui.remote, 0);

    lv_obj_t *body = lv_obj_create(s_ui.remote);
    lv_obj_set_pos(body, 8, 38);
    lv_obj_set_size(body, 304, 153);
    style_fixed_panel(body);
    lv_obj_set_style_bg_color(body, !strcmp(s_theme, "light") ? lv_color_hex(0x242b33) : lv_color_hex(0x10233c), 0);
    lv_obj_set_style_bg_grad_color(body, lv_color_hex(0x18345b), 0);
    lv_obj_set_style_bg_grad_dir(body, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(0x3d4659), 0);
    lv_obj_set_style_radius(body, 18, 0);
    lv_obj_set_style_pad_all(body, 0, 0);

    lv_obj_t *glow = lv_obj_create(body);
    lv_obj_set_pos(glow, 20, 84);
    lv_obj_set_size(glow, 264, 30);
    lv_obj_set_style_radius(glow, 15, 0);
    lv_obj_set_style_border_width(glow, 0, 0);
    lv_obj_set_style_bg_color(glow, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(glow, LV_OPA_10, 0);
    fixed_no_scroll(glow);

    size_t nbuttons = 0;
    const bridge_button_def_t *buttons = bridge_buttons(&nbuttons);
    for (size_t i = 0; i < nbuttons; i++) {
        const char *name = buttons[i].name;
        const char *text = buttons[i].label;
        if (!strcmp(name, BRIDGE_BUTTON_FLAP)) remote_action(body, name, text, 10, 10, 66, 42, false);
        else if (!strcmp(name, BRIDGE_BUTTON_SILENT)) remote_action(body, name, text, 82, 10, 66, 42, false);
        else if (!strcmp(name, BRIDGE_BUTTON_FAN)) remote_action(body, name, text, 154, 10, 66, 42, false);
        else if (!strcmp(name, BRIDGE_BUTTON_TIMER)) remote_action(body, name, text, 226, 10, 66, 42, false);
        else if (!strcmp(name, BRIDGE_BUTTON_ECO)) remote_action(body, name, text, 10, 62, 63, 44, false);
        else if (!strcmp(name, BRIDGE_BUTTON_MODE)) remote_action(body, name, text, 78, 62, 72, 44, false);
        else if (!strcmp(name, BRIDGE_BUTTON_POWER)) remote_action(body, name, text, 251, 62, 44, 44, true);
    }

    lv_obj_t *temp = lv_obj_create(body);
    lv_obj_set_pos(temp, 155, 62);
    lv_obj_set_size(temp, 91, 44);
    lv_obj_set_style_radius(temp, 22, 0);
    lv_obj_set_style_bg_color(temp, lv_color_hex(0x080c16), 0);
    lv_obj_set_style_bg_opa(temp, LV_OPA_70, 0);
    lv_obj_set_style_border_width(temp, 1, 0);
    lv_obj_set_style_border_color(temp, lv_color_hex(0xdce7e1), 0);
    lv_obj_set_style_pad_all(temp, 0, 0);
    fixed_no_scroll(temp);
    remote_temp_half(temp, BRIDGE_BUTTON_DOWN, 0, 45);
    remote_temp_half(temp, BRIDGE_BUTTON_UP, 45, 46);

    s_ui.remote_model = lv_obj_create(body);
    lv_obj_set_pos(s_ui.remote_model, 12, 116);
    lv_obj_set_size(s_ui.remote_model, 280, 27);
    lv_obj_set_style_radius(s_ui.remote_model, 8, 0);
    lv_obj_set_style_bg_opa(s_ui.remote_model, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(s_ui.remote_model, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_ui.remote_model, 1, 0);
    lv_obj_set_style_border_color(s_ui.remote_model, lv_color_hex(0x3d4659), 0);
    lv_obj_set_style_pad_all(s_ui.remote_model, 0, 0);
    fixed_no_scroll(s_ui.remote_model);
    s_ui.remote_model_mode = remote_model_cell(s_ui.remote_model, "COOL", 6, 54, false);
    s_ui.remote_model_temp = remote_model_cell(s_ui.remote_model, "24", 65, 38, true);
    s_ui.remote_model_fan = remote_model_cell(s_ui.remote_model, "F2", 108, 35, false);
    s_ui.remote_model_flags = remote_model_cell(s_ui.remote_model, "ECO SIL SWG", 148, 80, false);
    s_ui.remote_model_air = remote_model_cell(s_ui.remote_model, "Good", 233, 41, false);

    s_ui.remote_disabled = label(s_ui.remote, "nRF offline - commands disabled", UI_FONT_TINY);
    lv_obj_set_pos(s_ui.remote_disabled, 60, 195);
    lv_obj_set_size(s_ui.remote_disabled, 166, 18);
    lv_obj_set_style_text_color(s_ui.remote_disabled, lv_color_hex(0xffc857), 0);

    s_ui.remote_last = label(s_ui.remote, "last: none", UI_FONT_TINY);
    lv_obj_set_pos(s_ui.remote_last, 226, 195);
    lv_obj_set_size(s_ui.remote_last, 84, 18);
    lv_obj_set_style_text_align(s_ui.remote_last, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_ui.remote_last, theme_muted(), 0);
}

static void build_sync_screen(void)
{
    top_strip(s_ui.sync, 1);
    lv_obj_t *left = compact_card(s_ui.sync, 8, 38, 148, 153);
    lv_obj_t *right = compact_card(s_ui.sync, 164, 38, 148, 153);

    lv_obj_t *title = label(left, "Sync", UI_FONT_TITLE);
    lv_obj_set_pos(title, 8, 6);
    lv_obj_set_size(title, 120, 22);
    lv_obj_t *sub = label(left, "Model-only window", UI_FONT_META);
    lv_obj_set_pos(sub, 8, 30);
    lv_obj_set_size(sub, 128, 16);
    lv_obj_set_style_text_color(sub, theme_muted(), 0);
    lv_obj_t *sync = button(left, "Sync 30 s", 132, 44, UI_FONT_LABEL,
                            theme_button(), lv_color_hex(0x001b18), sync_event, NULL);
    lv_obj_set_pos(sync, 8, 52);
    s_ui.sync_mute = small_row(left, "Mute", "0 s", 8, 102, 132);
    s_ui.sync_state = small_row(left, "State", "--", 8, 127, 132);

    title = label(right, "Pairing", UI_FONT_TITLE);
    lv_obj_set_pos(title, 8, 6);
    lv_obj_set_size(title, 120, 22);
    lv_obj_t *pair = button(right, "Pair", 63, 38, UI_FONT_LABEL,
                            theme_button(), lv_color_hex(0x001b18), pair_event, (void *)(uintptr_t)0);
    lv_obj_set_pos(pair, 8, 36);
    lv_obj_t *unpair = button(right, "Unpair", 63, 38, UI_FONT_LABEL,
                              lv_color_hex(0x3a2a12), lv_color_hex(0xffd98a),
                              pair_event, (void *)(uintptr_t)1);
    lv_obj_set_pos(unpair, 77, 36);
    s_ui.sync_link = small_row(right, "Link", "--", 8, 82, 132);
    s_ui.sync_last = small_row(right, "Last", "--", 8, 107, 132);
    s_ui.sync_pair_mode = small_row(right, "Mode", "--", 8, 132, 132);
}

static void build_sensors_screen(void)
{
    top_strip(s_ui.sensors, 2);
    lv_obj_t *title = label(s_ui.sensors, "Sensors", UI_FONT_TITLE);
    lv_obj_set_pos(title, 8, 38);
    lv_obj_set_size(title, 180, 24);

    const char *names[] = {"Temperature", "Humidity", "Pressure", "Gas", "Air", "Presence"};
    const char *meta[] = {"BME680", "indoor", "hPa", "ohms", "relative", "LD2410"};
    for (uint8_t i = 0; i < 6; i++) {
        int col = i % 3;
        int row = i / 3;
        s_ui.sensor_value[i] = metric_card(s_ui.sensors, names[i], meta[i],
                                           8 + col * 102, 62 + row * 61, 96, 55);
    }

    s_ui.sensor_meta[0] = small_row(s_ui.sensors, "BME", "--", 8, 184, 148);
    s_ui.sensor_meta[1] = small_row(s_ui.sensors, "LD", "--", 164, 184, 148);
}

static void build_network_screen(void)
{
    top_strip(s_ui.network, 3);
    lv_obj_t *wifi = compact_card(s_ui.network, 8, 38, 148, 153);
    lv_obj_t *mqtt = compact_card(s_ui.network, 164, 38, 148, 153);
    lv_obj_t *title = label(wifi, "Wi-Fi", UI_FONT_TITLE);
    lv_obj_set_pos(title, 8, 6);
    lv_obj_set_size(title, 100, 22);
    lv_obj_t *scan = button(wifi, "Scan", 44, 22, UI_FONT_TINY,
                            theme_button(), lv_color_hex(0x001b18), scan_event, NULL);
    lv_obj_set_pos(scan, 96, 36);
    s_ui.network_body = small_row(wifi, "Net", "--", 8, 36, 83);
    small_row(wifi, "SSID", "--", 8, 63, 132);
    small_row(wifi, "Pass", "******", 8, 90, 132);
    lv_obj_t *connect = button(wifi, "Connect", 132, 34, UI_FONT_META,
                               theme_button(), lv_color_hex(0x001b18), NULL, NULL);
    lv_obj_set_pos(connect, 8, 119);

    title = label(mqtt, "MQTT", UI_FONT_TITLE);
    lv_obj_set_pos(title, 8, 6);
    lv_obj_set_size(title, 110, 22);
    small_row(mqtt, "Broker", "--", 8, 36, 132);
    small_row(mqtt, "Port", "1883", 8, 63, 83);
    lv_obj_t *auth = button(mqtt, "Auth", 44, 22, UI_FONT_TINY,
                            theme_button_alt(), theme_fg(), NULL, NULL);
    lv_obj_set_pos(auth, 96, 63);
    small_row(mqtt, "User", "optional", 8, 90, 132);
    lv_obj_t *save = button(mqtt, "Save", 132, 34, UI_FONT_META,
                            theme_button(), lv_color_hex(0x001b18), NULL, NULL);
    lv_obj_set_pos(save, 8, 119);
}

static void build_rules_screen(void)
{
    top_strip(s_ui.rules, 4);
    lv_obj_t *title = label(s_ui.rules, "Automation", UI_FONT_TITLE);
    lv_obj_set_pos(title, 8, 38);
    lv_obj_set_size(title, 180, 24);

    rule_card(s_ui.rules, 0, 65);
    rule_card(s_ui.rules, 1, 105);
    rule_card(s_ui.rules, 2, 145);

    lv_obj_t *edit = button(s_ui.rules, "Edit", 94, 34, UI_FONT_LABEL,
                            theme_button_alt(), theme_fg(), NULL, NULL);
    lv_obj_set_pos(edit, 210, 65);
    lv_obj_t *add = button(s_ui.rules, "Add", 94, 34, UI_FONT_LABEL,
                           theme_button(), lv_color_hex(0x001b18), add_rule_event, NULL);
    lv_obj_set_pos(add, 210, 105);
    lv_obj_t *save = button(s_ui.rules, "Save", 94, 34, UI_FONT_LABEL,
                            theme_button(), lv_color_hex(0x001b18), save_rules_event, NULL);
    lv_obj_set_pos(save, 210, 145);
}

static void build_settings_screen(void)
{
    top_strip(s_ui.settings, 5);
    lv_obj_t *title = label(s_ui.settings, "System", UI_FONT_TITLE);
    lv_obj_set_pos(title, 8, 38);
    lv_obj_set_size(title, 180, 24);

    lv_obj_t *left = compact_card(s_ui.settings, 8, 62, 148, 82);
    lv_obj_t *right = compact_card(s_ui.settings, 164, 62, 148, 82);

    lv_obj_t *display = label(left, "Display", UI_FONT_LABEL);
    lv_obj_set_pos(display, 8, 8);
    lv_obj_set_size(display, 120, 18);
    lv_obj_set_style_text_color(display, theme_muted(), 0);
    s_ui.brightness_slider = lv_slider_create(left);
    lv_obj_set_pos(s_ui.brightness_slider, 10, 30);
    lv_obj_set_size(s_ui.brightness_slider, 126, 9);
    lv_slider_set_range(s_ui.brightness_slider, 0, 100);
    lv_slider_set_value(s_ui.brightness_slider, s_brightness_percent, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_ui.brightness_slider, brightness_event, LV_EVENT_VALUE_CHANGED, NULL);
    s_ui.display_brightness_value = small_row(left, "Bright", "--", 8, 43, 132);
    s_ui.display_sleep_value = small_row(left, "Sleep", "--", 8, 66, 132);

    lv_obj_t *pins = label(right, "Pins", UI_FONT_LABEL);
    lv_obj_set_pos(pins, 8, 8);
    lv_obj_set_size(pins, 120, 18);
    lv_obj_set_style_text_color(pins, theme_muted(), 0);
    const char *pin_names[] = {"SDA", "SCL", "TX", "RX", "HB", "LD-T", "LD-R", "UI"};
    for (uint8_t i = 0; i < 8; i++) {
        s_ui.pin_value[i] = pin_tile(right, pin_names[i], 8 + (i % 4) * 31, 32 + (i / 4) * 25);
    }

    lv_obj_t *save_pins = button(s_ui.settings, "Save pins", 148, 34, UI_FONT_META,
                                 theme_button(), lv_color_hex(0x001b18), save_pins_event, NULL);
    lv_obj_set_pos(save_pins, 8, 152);
    lv_obj_t *reboot = button(s_ui.settings, "Reboot", 148, 34, UI_FONT_META,
                              lv_color_hex(0xef4444), lv_color_hex(0xffffff), reboot_event, NULL);
    lv_obj_set_pos(reboot, 164, 152);
}

static void build_tile_if_needed(uint8_t index)
{
    if (index >= UI_TILE_COUNT || s_tile_built[index]) return;
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
    int64_t start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "diag build_tile_begin tile=%u built=0x%02x heap=%lu",
             (unsigned)index, (unsigned)built_tile_mask(), (unsigned long)esp_get_free_heap_size());
#endif
    s_tile_built[index] = true;
    switch (index) {
    case 0:
        build_remote_screen();
        break;
    case 1:
        build_sync_screen();
        break;
    case 2:
        build_sensors_screen();
        break;
    case 3:
        build_network_screen();
        break;
    case 4:
        build_rules_screen();
        break;
    case 5:
        build_settings_screen();
        break;
    default:
        break;
    }
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
    ESP_LOGI(TAG, "diag build_tile_end tile=%u elapsed=%lldms built=0x%02x heap=%lu",
             (unsigned)index,
             (long long)((esp_timer_get_time() - start_us) / 1000),
             (unsigned)built_tile_mask(),
             (unsigned long)esp_get_free_heap_size());
#endif
}

static void build_root_screen(void)
{
    s_ui.root = lv_obj_create(NULL);
    style_screen(s_ui.root);
    lv_obj_add_flag(s_ui.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ui.root, activity_event, LV_EVENT_PRESSED, NULL);

    s_ui.tileview = lv_tileview_create(s_ui.root);
    lv_obj_set_size(s_ui.tileview, DISPLAY_H_RES, DISPLAY_V_RES);
    lv_obj_set_pos(s_ui.tileview, 0, 0);
    lv_obj_set_scrollbar_mode(s_ui.tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_ui.tileview, theme_bg(), 0);
    lv_obj_set_style_border_width(s_ui.tileview, 0, 0);
    lv_obj_add_event_cb(s_ui.tileview, tileview_event, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(s_ui.tileview, tileview_event, LV_EVENT_VALUE_CHANGED, NULL);

    s_ui.remote = s_ui.tiles[0] = make_tile(0);
    s_ui.sync = s_ui.tiles[1] = make_tile(1);
    s_ui.sensors = s_ui.tiles[2] = make_tile(2);
    s_ui.network = s_ui.tiles[3] = make_tile(3);
    s_ui.rules = s_ui.tiles[4] = make_tile(4);
    s_ui.settings = s_ui.tiles[5] = make_tile(5);
    build_tile_if_needed(0);
    build_tile_if_needed(1);

    s_ui.footer = lv_obj_create(s_ui.root);
    lv_obj_set_pos(s_ui.footer, 0, 196);
    lv_obj_set_size(s_ui.footer, 320, 44);
    lv_obj_set_style_bg_color(s_ui.footer, lv_color_hex(0x0a0d0b), 0);
    lv_obj_set_style_bg_opa(s_ui.footer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.footer, 1, 0);
    lv_obj_set_style_border_color(s_ui.footer, lv_color_hex(0x1f2823), 0);
    lv_obj_set_style_pad_all(s_ui.footer, 0, 0);
    fixed_no_scroll(s_ui.footer);

    s_ui.home_button = button(s_ui.root, "Home", 54, 44, UI_FONT_META,
                              theme_button(), lv_color_hex(0x001b18), nav_event, "remote");
    lv_obj_set_pos(s_ui.home_button, 0, 196);
    lv_obj_set_style_radius(s_ui.home_button, 0, 0);

    for (uint8_t i = 0; i < UI_TILE_COUNT; i++) {
        s_ui.page_dots[i] = lv_obj_create(s_ui.root);
        lv_obj_set_size(s_ui.page_dots[i], 7, 7);
        lv_obj_set_pos(s_ui.page_dots[i], 84 + (i * 14), 212);
        lv_obj_set_style_radius(s_ui.page_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_ui.page_dots[i], 0, 0);
        fixed_no_scroll(s_ui.page_dots[i]);
    }
    s_ui.page_label = label(s_ui.root, "Remote 1/6", UI_FONT_TINY);
    lv_obj_set_pos(s_ui.page_label, 214, 206);
    lv_obj_set_size(s_ui.page_label, 96, 14);
    lv_obj_set_style_text_align(s_ui.page_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_ui.page_label, theme_muted(), 0);
}

static void load_screen_locked(const char *screen_name)
{
    if (!s_lvgl_ready) return;
    uint8_t index = screen_index_for_name(screen_name);
    build_tile_neighbors(index);
    if (s_ui.tileview) lv_tileview_set_tile_by_index(s_ui.tileview, index, 0, LV_ANIM_ON);
    update_page_dots(index);
}

static void refresh_ui_locked(void)
{
    if (!s_lvgl_ready) return;
    bridge_state_t st;
    bridge_state_get_snapshot(&st);
    uint64_t now_ms = esp_timer_get_time() / 1000ULL;
    uint64_t nrf_age = st.nrf_last_seen_ms && now_ms >= st.nrf_last_seen_ms ? now_ms - st.nrf_last_seen_ms : 0;
    bool buttons_enabled = ui_lvgl_remote_buttons_enabled();

    char text[768];
    for (uint8_t i = 0; i < UI_TILE_COUNT; i++) {
        lv_color_t nrf_color = buttons_enabled ? lv_color_hex(0x22c55e) :
                               (st.nrf_available ? lv_color_hex(0xf59e0b) : lv_color_hex(0xef4444));
        lv_color_t wifi_color = st.wifi_connected ? lv_color_hex(0x22c55e) : lv_color_hex(0xf59e0b);
        lv_color_t mqtt_color = st.mqtt_connected ? lv_color_hex(0x22c55e) :
                                (st.mqtt_configured ? lv_color_hex(0xf59e0b) : lv_color_hex(0x68746d));
        if (s_ui.top_nrf_dot[i]) {
            lv_obj_set_style_bg_color(s_ui.top_nrf_dot[i], nrf_color, 0);
        }
        if (s_ui.top_wifi_dot[i]) {
            lv_obj_set_style_bg_color(s_ui.top_wifi_dot[i], wifi_color, 0);
        }
        if (s_ui.top_mqtt_dot[i]) {
            lv_obj_set_style_bg_color(s_ui.top_mqtt_dot[i], mqtt_color, 0);
        }
        if (s_ui.top_nrf[i]) {
            snprintf(text, sizeof(text), "nRF %s",
                     buttons_enabled ? (st.nrf_status[0] ? st.nrf_status : "ready") : "off");
            label_set_text_if_changed(s_ui.top_nrf[i], text);
            lv_obj_set_style_text_color(s_ui.top_nrf[i], nrf_color, 0);
        }
        if (s_ui.top_wifi[i]) {
            snprintf(text, sizeof(text), st.wifi_connected ? "%s" : "Wi-Fi AP",
                     st.wifi_connected ? st.wifi_ssid : "");
            label_set_text_if_changed(s_ui.top_wifi[i], text);
            lv_obj_set_style_text_color(s_ui.top_wifi[i], wifi_color, 0);
        }
        if (s_ui.top_mqtt[i]) {
            snprintf(text, sizeof(text), "MQTT %s", st.mqtt_connected ? "on" : (st.mqtt_configured ? "cfg" : "off"));
            label_set_text_if_changed(s_ui.top_mqtt[i], text);
            lv_obj_set_style_text_color(s_ui.top_mqtt[i], mqtt_color, 0);
        }
        if (s_ui.top_env[i]) {
            lv_color_t env_color = theme_muted();
            switch (i) {
            case 0:
                snprintf(text, sizeof(text), "%.1fC %.0f%%", st.temperature_c, st.humidity_percent);
                break;
            case 1:
                snprintf(text, sizeof(text), "%.1fC %s", st.temperature_c,
                         st.air_quality[0] ? st.air_quality : "--");
                break;
            case 2:
                snprintf(text, sizeof(text), "%s", rules_presence() ? "presence" : "clear");
                env_color = rules_presence() ? lv_color_hex(0x22c55e) : theme_muted();
                break;
            case 3:
                snprintf(text, sizeof(text), "%s", st.wifi_ip[0] ? st.wifi_ip : "192.168.4.1");
                break;
            case 4:
                snprintf(text, sizeof(text), rules_presence() ? "present %lum" : "clear",
                         (unsigned long)(rules_presence_secs() / 60));
                env_color = rules_presence() ? lv_color_hex(0x22c55e) : theme_muted();
                break;
            case 5:
                snprintf(text, sizeof(text), "heap %luk", (unsigned long)(st.free_heap / 1024));
                break;
            default:
                snprintf(text, sizeof(text), "--");
                break;
            }
            label_set_text_if_changed(s_ui.top_env[i], text);
            lv_obj_set_style_text_color(s_ui.top_env[i], env_color, 0);
        }
    }

    if (s_ui.remote_model) {
        ac_state_t ac;
        ac_state_get_copy(&ac);
        if (s_ui.remote_model_mode) {
            label_set_text_if_changed(s_ui.remote_model_mode, remote_mode_short(&ac));
        }
        if (s_ui.remote_model_temp) {
            snprintf(text, sizeof(text), "%u", (unsigned)ac.temp_c);
            label_set_text_if_changed(s_ui.remote_model_temp, text);
        }
        if (s_ui.remote_model_fan) {
            label_set_text_if_changed(s_ui.remote_model_fan, remote_fan_short(ac.fan));
        }
        if (s_ui.remote_model_flags) {
            snprintf(text, sizeof(text), "%s%s%s%s",
                     ac.eco ? "ECO " : "",
                     ac.silent ? "SIL " : "",
                     ac.swing ? "SWG " : "",
                     ac.timer_state ? "TMR" : "");
            if (!text[0]) strlcpy(text, "--", sizeof(text));
            label_set_text_if_changed(s_ui.remote_model_flags, text);
        }
        if (s_ui.remote_model_air) {
            label_set_text_if_changed(s_ui.remote_model_air, st.air_quality[0] ? st.air_quality : "--");
        }
    }
    if (s_ui.remote_disabled) {
        label_set_text_if_changed(s_ui.remote_disabled, buttons_enabled ? "" : ui_lvgl_remote_disabled_reason());
    }
    if (s_ui.remote_last) {
        snprintf(text, sizeof(text), "last: %s", st.last_button[0] ? st.last_button : "none");
        label_set_text_if_changed(s_ui.remote_last, text);
    }
    for (size_t i = 0; i < sizeof(s_ui.remote_buttons) / sizeof(s_ui.remote_buttons[0]); i++) {
        if (!s_ui.remote_buttons[i]) continue;
        if (buttons_enabled) lv_obj_remove_state(s_ui.remote_buttons[i], LV_STATE_DISABLED);
        else lv_obj_add_state(s_ui.remote_buttons[i], LV_STATE_DISABLED);
    }

    if (s_ui.sync_mute) {
        snprintf(text, sizeof(text), "%d s", uart_link_mute_secs());
        label_set_text_if_changed(s_ui.sync_mute, text);
    }
    if (s_ui.sync_state) {
        ac_state_t ac;
        ac_state_get_copy(&ac);
        snprintf(text, sizeof(text), "%s %u %s", remote_mode_short(&ac),
                 (unsigned)ac.temp_c, remote_fan_short(ac.fan));
        label_set_text_if_changed(s_ui.sync_state, text);
    }
    if (s_ui.sync_link) {
        label_set_text_if_changed(s_ui.sync_link, st.nrf_status[0] ? st.nrf_status : "--");
    }
    if (s_ui.sync_last) {
        snprintf(text, sizeof(text), "%llu s", (unsigned long long)(nrf_age / 1000ULL));
        label_set_text_if_changed(s_ui.sync_last, text);
    }
    if (s_ui.sync_pair_mode) {
        label_set_text_if_changed(s_ui.sync_pair_mode, uart_link_will_model() ? "modeling" : "advertise");
    }

    if (s_ui.sensor_value[0]) {
        snprintf(text, sizeof(text), "%.1fC", st.temperature_c);
        label_set_text_if_changed(s_ui.sensor_value[0], text);
        snprintf(text, sizeof(text), "%.0f%%", st.humidity_percent);
        label_set_text_if_changed(s_ui.sensor_value[1], text);
        snprintf(text, sizeof(text), "%.0f", st.pressure_hpa);
        label_set_text_if_changed(s_ui.sensor_value[2], text);
        snprintf(text, sizeof(text), "%luk", (unsigned long)(st.gas_resistance_ohm / 1000));
        label_set_text_if_changed(s_ui.sensor_value[3], text);
        label_set_text_if_changed(s_ui.sensor_value[4], st.air_quality[0] ? st.air_quality : "--");
        snprintf(text, sizeof(text), "%02lu:%02lu",
                 (unsigned long)(rules_presence_secs() / 60),
                 (unsigned long)(rules_presence_secs() % 60));
        label_set_text_if_changed(s_ui.sensor_value[5], text);
        if (s_ui.sensor_meta[0]) {
            label_set_text_if_changed(s_ui.sensor_meta[0], st.sensor_available ? "ready" : "missing");
        }
        if (s_ui.sensor_meta[1]) {
            label_set_text_if_changed(s_ui.sensor_meta[1], rules_presence() ? "present" : "clear");
        }
    }

    if (s_ui.network_body) {
        if (s_wifi_scan_summary[0] && esp_timer_get_time() < s_wifi_scan_until_us) {
            strlcpy(text, s_wifi_scan_summary, sizeof(text));
        } else {
            snprintf(text, sizeof(text), st.wifi_connected ? "%s" : "%s",
                     st.wifi_connected ? st.wifi_ssid : wifi_mgr_state_str());
        }
        label_set_text_if_changed(s_ui.network_body, text);
    }

    if (s_ui.rule_top[0]) {
        rule_t rs[RULES_MAX];
        int n = rules_get(rs, RULES_MAX);
        for (uint8_t i = 0; i < 3; i++) {
            if (i < n) {
                lv_color_t rule_color = !rs[i].enabled ? lv_color_hex(0x68746d) :
                                        (rs[i].cond == RULE_COND_PRESENCE ?
                                         lv_color_hex(0x22c55e) : lv_color_hex(0xf59e0b));
                if (s_ui.rule_dot[i]) {
                    lv_obj_set_style_bg_color(s_ui.rule_dot[i], rule_color, 0);
                }
                snprintf(text, sizeof(text), "%s",
                         rs[i].cond == RULE_COND_PRESENCE ? "Presence" : "Absence");
                label_set_text_if_changed(s_ui.rule_top[i], text);
                if (s_ui.rule_target[i]) {
                    label_set_text_if_changed(s_ui.rule_target[i], rs[i].target);
                }
                snprintf(text, sizeof(text), "after %lu min, %s",
                         (unsigned long)(rs[i].duration_s / 60),
                         rs[i].enabled ? "enabled" : "disabled");
                label_set_text_if_changed(s_ui.rule_bottom[i], text);
            } else if (i == 0) {
                if (s_ui.rule_dot[i]) {
                    lv_obj_set_style_bg_color(s_ui.rule_dot[i], lv_color_hex(0x68746d), 0);
                }
                label_set_text_if_changed(s_ui.rule_top[i], "No rules");
                if (s_ui.rule_target[i]) {
                    label_set_text_if_changed(s_ui.rule_target[i], "");
                }
                snprintf(text, sizeof(text), "Presence %s %lus",
                         rules_presence() ? "yes" : "no",
                         (unsigned long)rules_presence_secs());
                label_set_text_if_changed(s_ui.rule_bottom[i], text);
            } else {
                if (s_ui.rule_dot[i]) {
                    lv_obj_set_style_bg_color(s_ui.rule_dot[i], lv_color_hex(0x68746d), 0);
                }
                label_set_text_if_changed(s_ui.rule_top[i], "--");
                if (s_ui.rule_target[i]) {
                    label_set_text_if_changed(s_ui.rule_target[i], "");
                }
                label_set_text_if_changed(s_ui.rule_bottom[i], "");
            }
        }
    }

    if (s_ui.pin_value[0]) {
        const device_pins_t *p = pins_get();
        int vals[] = {p->i2c_sda, p->i2c_scl, p->nrf_tx, p->nrf_rx,
                      p->nrf_hb, p->ld_tx, p->ld_rx, st.touch_available ? 1 : 0};
        for (uint8_t i = 0; i < 8; i++) {
            if (i == 7) snprintf(text, sizeof(text), vals[i] ? "OK" : "--");
            else snprintf(text, sizeof(text), "%d", vals[i]);
            label_set_text_if_changed(s_ui.pin_value[i], text);
        }
    }

    if (s_ui.display_brightness_value) {
        snprintf(text, sizeof(text), "%u%%", (unsigned)s_brightness_percent);
        label_set_text_if_changed(s_ui.display_brightness_value, text);
    }
    if (s_ui.display_sleep_value) {
        snprintf(text, sizeof(text), "%u s", (unsigned)s_screen_timeout_s);
        label_set_text_if_changed(s_ui.display_sleep_value, text);
    }
    if (s_ui.settings_url) {
        snprintf(text, sizeof(text), "Web: http://%s/",
                 st.wifi_ip[0] ? st.wifi_ip : wifi_mgr_ap_ssid());
        label_set_text_if_changed(s_ui.settings_url, text);
    }
}

#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
static void log_ui_heartbeat_locked(uint32_t delay_ms)
{
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_ui_diag_last_heartbeat_us < 1000000LL) return;
    s_ui_diag_last_heartbeat_us = now_us;
    s_ui_diag_last_progress_us = now_us;
    s_ui_diag_last_loop_count = s_ui_diag_loop_count;

    uint8_t index = 0;
    bool has_active = active_tile_index(&index);
    int32_t scroll_x = s_ui.tileview ? lv_obj_get_scroll_x(s_ui.tileview) : 0;
    int32_t width = s_ui.tileview ? lv_obj_get_content_width(s_ui.tileview) : 0;
    bool pressed = pointer_is_pressed();
    bool scrolling = s_ui.tileview && lv_obj_is_scrolling(s_ui.tileview);
    s_ui_diag_last_event = "heartbeat";
    s_ui_diag_last_tile = index;
    s_ui_diag_last_scroll_x = scroll_x;
    s_ui_diag_last_width = width;
    s_ui_diag_last_pressed = pressed;
    s_ui_diag_last_scrolling = scrolling;
    s_ui_diag_last_built = built_tile_mask();
    ESP_LOGI(TAG, "diag heartbeat loop=%lu active=%d tile=%u screen=%s scroll=%ld width=%ld pressed=%d scrolling=%d built=0x%02x backlight=%d delay=%lu heap=%lu",
             (unsigned long)s_ui_diag_loop_count,
             has_active ? 1 : 0,
             (unsigned)index,
             s_screen,
             (long)scroll_x,
             (long)width,
             pressed ? 1 : 0,
             scrolling ? 1 : 0,
             (unsigned)s_ui_diag_last_built,
             s_backlight_on ? 1 : 0,
             (unsigned long)delay_ms,
             (unsigned long)esp_get_free_heap_size());
}

static void ui_diag_task(void *arg)
{
    (void)arg;
    for (;;) {
        int64_t now_us = esp_timer_get_time();
        int64_t last_us = s_ui_diag_last_progress_us;
        int64_t age_ms = last_us > 0 ? (now_us - last_us) / 1000 : -1;
        bool stale = age_ms > 2000;
        ESP_LOG_LEVEL(stale ? ESP_LOG_WARN : ESP_LOG_INFO, TAG,
                      "diag monitor %s age=%lldms phase=%s event=%s loop=%lu tile=%u scroll=%ld width=%ld pressed=%d scrolling=%d built=0x%02x touch_age=%lldms heap=%lu",
                      stale ? "STALE" : "ok",
                      (long long)age_ms,
                      s_ui_diag_phase,
                      s_ui_diag_last_event,
                      (unsigned long)s_ui_diag_last_loop_count,
                      (unsigned)s_ui_diag_last_tile,
                      (long)s_ui_diag_last_scroll_x,
                      (long)s_ui_diag_last_width,
                      s_ui_diag_last_pressed ? 1 : 0,
                      s_ui_diag_last_scrolling ? 1 : 0,
                      (unsigned)s_ui_diag_last_built,
                      s_ui_diag_last_touch_us > 0 ? (long long)((now_us - s_ui_diag_last_touch_us) / 1000) : -1,
                      (unsigned long)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

static esp_err_t init_lvgl_screens(void)
{
    if (!lv_display_get_default()) {
        ESP_LOGW(TAG, "LVGL display is not registered by board layer yet");
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_lvgl_mutex) return ESP_ERR_NO_MEM;

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_err_t err = esp_timer_create(&tick_args, &s_lvgl_tick);
    if (err != ESP_OK) return err;
    err = esp_timer_start_periodic(s_lvgl_tick, UI_LVGL_TICK_MS * 1000);
    if (err != ESP_OK) return err;

    lvgl_lock();
    build_root_screen();
    lv_screen_load(s_ui.root);
    s_lvgl_ready = true;
    load_screen_locked(s_screen);
    lvgl_unlock();
    return ESP_OK;
}

static void ui_task(void *arg)
{
    ESP_LOGI(TAG, "touchscreen UI task running for board %s", BOARD_NAME);
    for (;;) {
        if (s_display_available && s_backlight_on && s_screen_timeout_s > 0) {
            int64_t idle_us = esp_timer_get_time() - s_last_activity_us;
            if (idle_us >= (int64_t)s_screen_timeout_s * 1000000LL) {
                esp_err_t err = board_backlight_set(0);
                if (err == ESP_OK) {
                    s_backlight_on = false;
                    publish_display_state();
                } else if (err != ESP_ERR_NOT_SUPPORTED) {
                    ESP_LOGW(TAG, "backlight timeout failed: %s", esp_err_to_name(err));
                }
            }
        }
        if (s_lvgl_ready) {
            lvgl_lock();
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
            s_ui_diag_phase = "loop_start";
            s_ui_diag_last_progress_us = esp_timer_get_time();
            s_ui_diag_loop_count++;
#endif
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
            s_ui_diag_phase = "lv_timer_handler";
#endif
            uint32_t delay_ms = lv_timer_handler();
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
            s_ui_diag_phase = "snap_recover";
#endif
            recover_tileview_snap_locked();
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
            s_ui_diag_phase = "refresh";
#endif
            refresh_ui_locked();
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
            s_ui_diag_phase = "heartbeat";
            log_ui_heartbeat_locked(delay_ms);
            s_ui_diag_phase = "unlock";
#endif
            lvgl_unlock();
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
            s_ui_diag_phase = "delay";
#endif
            if (delay_ms < 20) delay_ms = 20;
            if (delay_ms > 250) delay_ms = 250;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        } else {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}
#endif

esp_err_t ui_lvgl_start(void)
{
    load_settings();
#ifndef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
    return ESP_ERR_NOT_SUPPORTED;
#else
    lv_init();

    esp_err_t display = board_display_init();
    esp_err_t touch = board_touch_init();
    board_touch_set_activity_cb(ui_lvgl_note_activity);

    s_display_available = display == ESP_OK;
    s_touch_available = touch == ESP_OK;

    if (display != ESP_OK) {
        ESP_LOGW(TAG, "display unavailable: %s", esp_err_to_name(display));
    }
    if (touch != ESP_OK) {
        ESP_LOGW(TAG, "touch unavailable: %s", esp_err_to_name(touch));
    }
    if (s_display_available && !s_touch_available) {
        strlcpy(s_screen, "status", sizeof(s_screen));
    }

    esp_err_t backlight = board_backlight_set(s_brightness_percent);
    s_backlight_on = backlight == ESP_OK && s_brightness_percent > 0;
    s_last_activity_us = esp_timer_get_time();
    if (backlight != ESP_OK && backlight != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "backlight init failed: %s", esp_err_to_name(backlight));
    }
    mqtt_ha_publish_display_brightness(s_brightness_percent);
    mqtt_ha_publish_display_timeout(s_screen_timeout_s);
    mqtt_ha_publish_display_theme(s_theme);
    publish_display_state();
    if (s_display_available) {
        esp_err_t lvgl = init_lvgl_screens();
        if (lvgl != ESP_OK && lvgl != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "LVGL screen init failed: %s", esp_err_to_name(lvgl));
        }
    }

    xTaskCreate(ui_task, "ui_lvgl", 8192, NULL, 3, NULL);
#ifdef CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC
    xTaskCreate(ui_diag_task, "ui_diag", 3072, NULL, 2, NULL);
#endif
    return ESP_OK;
#endif
}

bool ui_lvgl_display_available(void) { return s_display_available; }
bool ui_lvgl_touch_available(void) { return s_touch_available; }
uint8_t ui_lvgl_brightness_percent(void) { return s_brightness_percent; }
uint16_t ui_lvgl_screen_timeout_s(void) { return s_screen_timeout_s; }
const char *ui_lvgl_current_screen(void) { return s_screen; }
const char *ui_lvgl_theme(void) { return s_theme; }
bool ui_lvgl_backlight_on(void) { return s_backlight_on; }

bool ui_lvgl_remote_buttons_enabled(void)
{
    nrf_state_t st = uart_link_nrf_state();
    return st == NRF_READY || st == NRF_BONDED;
}

const char *ui_lvgl_remote_disabled_reason(void)
{
    if (ui_lvgl_remote_buttons_enabled()) return "";
    if (!uart_link_alive()) return "nRF offline - commands disabled";
    if (uart_link_nrf_state() == NRF_ERROR) return "nRF error";
    return "nRF not ready";
}

void ui_lvgl_note_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (!s_display_available || s_brightness_percent == 0 || s_backlight_on) return;
    esp_err_t err = board_backlight_set(s_brightness_percent);
    if (err == ESP_OK) {
        s_backlight_on = true;
        publish_display_state();
    } else if (err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "backlight wake failed: %s", esp_err_to_name(err));
    }
}

esp_err_t ui_lvgl_set_screen(const char *screen_name)
{
    if (!valid_screen_name(screen_name)) return ESP_ERR_INVALID_ARG;
    strlcpy(s_screen, screen_name, sizeof(s_screen));
    ui_lvgl_note_activity();
#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
    if (s_lvgl_ready) {
        lvgl_lock();
        load_screen_locked(s_screen);
        lvgl_unlock();
    }
#endif
    publish_display_state();
    return ESP_OK;
}

esp_err_t ui_lvgl_press_button(const char *button_name)
{
    if (!ui_lvgl_remote_buttons_enabled()) return ESP_ERR_INVALID_STATE;
    ui_lvgl_note_activity();
    return bridge_press_button(button_name);
}

esp_err_t ui_lvgl_set_brightness_percent(uint8_t percent)
{
    if (percent > 100) return ESP_ERR_INVALID_ARG;
    s_brightness_percent = percent;
    esp_err_t err = board_backlight_set(percent);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) return err;
    s_backlight_on = err == ESP_OK && percent > 0;
    mqtt_ha_publish_display_brightness(percent);
    ui_lvgl_note_activity();
    publish_display_state();
    return save_settings();
}

esp_err_t ui_lvgl_set_screen_timeout_s(uint16_t seconds)
{
    if (seconds < UI_TIMEOUT_MIN_S || seconds > UI_TIMEOUT_MAX_S) return ESP_ERR_INVALID_ARG;
    s_screen_timeout_s = seconds;
    ui_lvgl_note_activity();
    mqtt_ha_publish_display_timeout(seconds);
    publish_display_state();
    return save_settings();
}

esp_err_t ui_lvgl_set_theme(const char *theme)
{
    if (!theme || (strcmp(theme, "dark") != 0 && strcmp(theme, "light") != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_theme, theme, sizeof(s_theme));
    ui_lvgl_note_activity();
    mqtt_ha_publish_display_theme(s_theme);
#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
    if (s_lvgl_ready) {
        lvgl_lock();
        if (s_ui.remote) style_screen(s_ui.remote);
        if (s_ui.sync) style_screen(s_ui.sync);
        if (s_ui.sensors) style_screen(s_ui.sensors);
        if (s_ui.network) style_screen(s_ui.network);
        if (s_ui.rules) style_screen(s_ui.rules);
        if (s_ui.settings) style_screen(s_ui.settings);
        lvgl_unlock();
    }
#endif
    publish_display_state();
    return save_settings();
}
