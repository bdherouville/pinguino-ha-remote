#include "mqtt_ha.h"
#include "uart_link.h"
#include "ac_state.h"
#include "ac_cmd.h"
#include "bridge_buttons.h"
#include "command_queue.h"
#include "board_config.h"
#include "bridge_state.h"
#include "ui_lvgl.h"
#include "esp_app_desc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "mqtt_client.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#define NVS_NS    "mqtt"
#define AVTY_TOPIC "ganymede/status"                 // legacy bridge availability topic
#define STATE_AVTY_TOPIC AVTY_TOPIC                  // keep one retained availability/LWT topic
#define CMD_PREFIX "ganymede/cmd/"
#define NRF_TOPIC  "ganymede/nrf"
#define NRF_STATE_TOPIC "ganymede/state/nrf_status"
#define LAST_BUTTON_TOPIC "ganymede/state/last_button"
#define PRES_TOPIC "ganymede/presence"
#define PRES_AVTY  "ganymede/presence/status"   // per-sensor availability (LD2410 alive?)
#define AC_BASE    "ganymede/ac/"                // climate state/<x> + command <x>/set
#define SENSOR_STATE_BASE "ganymede/state/sensor/"
#define DISPLAY_BRIGHTNESS_TOPIC "ganymede/state/display/brightness"
#define DISPLAY_BRIGHTNESS_CMD_TOPIC "ganymede/display/brightness/set"
#define DISPLAY_TIMEOUT_TOPIC "ganymede/state/display/screen_timeout"
#define DISPLAY_TIMEOUT_CMD_TOPIC "ganymede/display/screen_timeout/set"
#define DISPLAY_THEME_TOPIC "ganymede/state/display/theme"
#define DISPLAY_THEME_CMD_TOPIC "ganymede/display/theme/set"
#define FW_VERSION_TOPIC "ganymede/state/firmware/version"
#define WIFI_RSSI_TOPIC "ganymede/state/wifi/rssi"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_nrf[16] = "offline";   // last nRF link state, republished on (re)connect
static char s_last_button[12] = "";
static char s_presence[4] = "OFF";      // last LD2410 presence, republished on (re)connect
static char s_presence_avail[8] = "offline";   // LD2410 availability (online once frames seen)
static char s_air_quality[16] = "Unknown";
static char s_last_gas[16] = "";
static uint8_t s_display_brightness = 80;
static uint16_t s_display_timeout_s = 60;
static char s_display_theme[8] = "dark";
static int s_wifi_rssi;
static char s_host[64] = "";
static int  s_port = 1883;
static char s_user[48] = "";
static char s_pass[64] = "";

// ---- NVS ----
static void cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n;
    n = sizeof(s_host); nvs_get_str(h, "host", s_host, &n);
    n = sizeof(s_user); if (nvs_get_str(h, "user", s_user, &n) != ESP_OK) s_user[0] = 0;
    n = sizeof(s_pass); if (nvs_get_str(h, "pass", s_pass, &n) != ESP_OK) s_pass[0] = 0;
    uint16_t p = 0; if (nvs_get_u16(h, "port", &p) == ESP_OK && p) s_port = p;
    nvs_close(h);
}
static void cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "host", s_host);
    nvs_set_str(h, "user", s_user);
    nvs_set_str(h, "pass", s_pass);
    nvs_set_u16(h, "port", (uint16_t)s_port);
    nvs_commit(h); nvs_close(h);
}

#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
static bool parse_int_range(const char *text, int min_value, int max_value, int *out)
{
    if (!text || !text[0]) return false;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if ((end && *end) || value < min_value || value > max_value) return false;
    if (out) *out = (int)value;
    return true;
}
#endif

// ---- HA discovery ----
static void publish_discovery(void)
{
    char topic[128], payload[760];
    size_t nbuttons = 0;
    const bridge_button_def_t *buttons = bridge_buttons(&nbuttons);
    for (size_t i = 0; i < nbuttons; i++) {
        snprintf(topic, sizeof(topic), "homeassistant/button/ganymede_%s/config", buttons[i].name);
        snprintf(payload, sizeof(payload),
            "{\"name\":\"%s\",\"unique_id\":\"ganymede_%s\","
            "\"command_topic\":\"" CMD_PREFIX "%s\",\"payload_press\":\"PRESS\","
            "\"availability_topic\":\"" STATE_AVTY_TOPIC "\","
            "\"device\":{\"identifiers\":[\"ganymede_bridge\"],\"name\":\"Ganymede Bridge\","
            "\"manufacturer\":\"DIY\",\"model\":\"De'Longhi remote emulator\"}}",
            buttons[i].label, buttons[i].name, buttons[i].name);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true); // retained
    }
    // Environmental Sensing -> HA sensor entities
    static const struct { const char *id, *name, *unit, *dc; } S[] = {
        {"temperature","Temperature","°C","temperature"},
        {"humidity","Humidity","%","humidity"},
        {"pressure","Pressure","hPa","pressure"},
    };
    for (size_t i = 0; i < 3; i++) {
        snprintf(topic, sizeof(topic), "homeassistant/sensor/ganymede_%s/config", S[i].id);
        snprintf(payload, sizeof(payload),
            "{\"name\":\"%s\",\"unique_id\":\"ganymede_%s\",\"state_topic\":\"" SENSOR_STATE_BASE "%s\","
            "\"unit_of_measurement\":\"%s\",\"device_class\":\"%s\",\"availability_topic\":\"" STATE_AVTY_TOPIC "\","
            "\"device\":{\"identifiers\":[\"ganymede_bridge\"],\"name\":\"Ganymede Bridge\"}}",
            S[i].name, S[i].id, S[i].id, S[i].unit, S[i].dc);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
    }
    // nRF link state -> diagnostic sensor
    snprintf(topic, sizeof(topic), "homeassistant/sensor/ganymede_nrf/config");
    snprintf(payload, sizeof(payload),
        "{\"name\":\"nRF Link\",\"unique_id\":\"ganymede_nrf\",\"state_topic\":\"" NRF_STATE_TOPIC "\","
        "\"icon\":\"mdi:bluetooth\",\"entity_category\":\"diagnostic\",\"availability_topic\":\"" STATE_AVTY_TOPIC "\","
        "\"device\":{\"identifiers\":[\"ganymede_bridge\"],\"name\":\"Ganymede Bridge\"}}");
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);

    static const struct { const char *id, *name, *topic, *unit, *dc, *icon; } EXTRA_S[] = {
        {"gas_resistance", "Gas resistance", SENSOR_STATE_BASE "gas_resistance", "Ohm", "", "mdi:air-filter"},
        {"air_quality", "Air quality", SENSOR_STATE_BASE "air_quality", "", "enum", "mdi:air-filter"},
        {"firmware", "Firmware version", FW_VERSION_TOPIC, "", "", "mdi:package-variant"},
        {"wifi_rssi", "Wi-Fi RSSI", WIFI_RSSI_TOPIC, "dBm", "signal_strength", "mdi:wifi"},
    };
    for (size_t i = 0; i < sizeof(EXTRA_S) / sizeof(EXTRA_S[0]); i++) {
        snprintf(topic, sizeof(topic), "homeassistant/sensor/ganymede_%s/config", EXTRA_S[i].id);
        snprintf(payload, sizeof(payload),
            "{\"name\":\"%s\",\"unique_id\":\"ganymede_%s\",\"state_topic\":\"%s\","
            "\"availability_topic\":\"" STATE_AVTY_TOPIC "\",\"entity_category\":\"diagnostic\","
            "\"device\":{\"identifiers\":[\"ganymede_bridge\"],\"name\":\"Ganymede Bridge\","
            "\"model\":\"%s\"}%s%s%s%s%s%s}",
            EXTRA_S[i].name, EXTRA_S[i].id, EXTRA_S[i].topic, BOARD_NAME,
            EXTRA_S[i].unit[0] ? ",\"unit_of_measurement\":\"" : "",
            EXTRA_S[i].unit[0] ? EXTRA_S[i].unit : "",
            EXTRA_S[i].unit[0] ? "\"" : "",
            EXTRA_S[i].dc[0] ? ",\"device_class\":\"" : "",
            EXTRA_S[i].dc[0] ? EXTRA_S[i].dc : "",
            EXTRA_S[i].dc[0] ? "\"" : "");
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
    }

#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
    snprintf(topic, sizeof(topic), "homeassistant/number/ganymede_display_brightness/config");
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Display brightness\",\"unique_id\":\"ganymede_display_brightness\","
        "\"state_topic\":\"" DISPLAY_BRIGHTNESS_TOPIC "\","
        "\"command_topic\":\"" DISPLAY_BRIGHTNESS_CMD_TOPIC "\","
        "\"min\":0,\"max\":100,\"step\":1,"
        "\"unit_of_measurement\":\"%%\",\"entity_category\":\"config\","
        "\"availability_topic\":\"" STATE_AVTY_TOPIC "\","
        "\"device\":{\"identifiers\":[\"ganymede_bridge\"],\"name\":\"Ganymede Bridge\",\"model\":\"%s\"}}",
        BOARD_NAME);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);

    snprintf(topic, sizeof(topic), "homeassistant/number/ganymede_display_timeout/config");
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Screen timeout\",\"unique_id\":\"ganymede_display_timeout\","
        "\"state_topic\":\"" DISPLAY_TIMEOUT_TOPIC "\","
        "\"command_topic\":\"" DISPLAY_TIMEOUT_CMD_TOPIC "\","
        "\"min\":10,\"max\":600,\"step\":10,"
        "\"unit_of_measurement\":\"s\",\"entity_category\":\"config\","
        "\"availability_topic\":\"" STATE_AVTY_TOPIC "\","
        "\"device\":{\"identifiers\":[\"ganymede_bridge\"],\"name\":\"Ganymede Bridge\",\"model\":\"%s\"}}",
        BOARD_NAME);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);

    snprintf(topic, sizeof(topic), "homeassistant/select/ganymede_display_theme/config");
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Display theme\",\"unique_id\":\"ganymede_display_theme\","
        "\"state_topic\":\"" DISPLAY_THEME_TOPIC "\","
        "\"command_topic\":\"" DISPLAY_THEME_CMD_TOPIC "\","
        "\"options\":[\"dark\",\"light\"],\"entity_category\":\"config\","
        "\"availability_topic\":\"" STATE_AVTY_TOPIC "\","
        "\"device\":{\"identifiers\":[\"ganymede_bridge\"],\"name\":\"Ganymede Bridge\",\"model\":\"%s\"}}",
        BOARD_NAME);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
#endif
    // LD2410 presence -> occupancy binary_sensor. Availability requires BOTH the bridge to be
    // online AND the LD2410 to be producing frames, so a dead/unwired sensor shows "unavailable"
    // in HA instead of a false "vacant" that could drive absence automations.
    snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/ganymede_presence/config");
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Presence\",\"unique_id\":\"ganymede_presence\",\"state_topic\":\"" PRES_TOPIC "\","
        "\"device_class\":\"occupancy\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
        "\"availability\":[{\"topic\":\"" STATE_AVTY_TOPIC "\"},{\"topic\":\"" PRES_AVTY "\"}],"
        "\"availability_mode\":\"all\","
        "\"device\":{\"identifiers\":[\"ganymede_bridge\"],\"name\":\"Ganymede Bridge\"}}");
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);

    // Open-loop AC model -> HA climate entity (mode / target temp / fan modes).
    snprintf(topic, sizeof(topic), "homeassistant/climate/ganymede_ac/config");
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Climatiseur\",\"unique_id\":\"ganymede_ac\","
        "\"modes\":[\"off\",\"cool\",\"dry\",\"fan_only\"],"
        "\"mode_command_topic\":\"" AC_BASE "mode/set\",\"mode_state_topic\":\"" AC_BASE "mode\","
        "\"temperature_command_topic\":\"" AC_BASE "temp/set\",\"temperature_state_topic\":\"" AC_BASE "temp\","
        "\"temperature_unit\":\"C\",\"min_temp\":%d,\"max_temp\":%d,\"temp_step\":1,"
        "\"fan_modes\":[\"min\",\"medium\",\"max\",\"auto\"],"
        "\"fan_mode_command_topic\":\"" AC_BASE "fan/set\",\"fan_mode_state_topic\":\"" AC_BASE "fan\","
        "\"availability_topic\":\"" STATE_AVTY_TOPIC "\","
        "\"device\":{\"identifiers\":[\"ganymede_bridge\"],\"name\":\"Ganymede Bridge\"}}",
        AC_TEMP_MIN, AC_TEMP_MAX);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);

    // COOL-only / airflow toggles -> HA switches (state mirrors the model; command = a press).
    static const struct { const char *id, *name, *icon; } SW[] = {
        {"swing",  "Swing",         "mdi:arrow-oscillating"},
        {"eco",    "Eco Real Feel", "mdi:leaf"},
        {"silent", "Silent",        "mdi:volume-mute"},
    };
    for (size_t i = 0; i < 3; i++) {
        snprintf(topic, sizeof(topic), "homeassistant/switch/ganymede_%s/config", SW[i].id);
        snprintf(payload, sizeof(payload),
            "{\"name\":\"%s\",\"unique_id\":\"ganymede_sw_%s\","
            "\"command_topic\":\"" AC_BASE "%s/set\",\"state_topic\":\"" AC_BASE "%s\","
            "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"icon\":\"%s\","
            "\"availability_topic\":\"" STATE_AVTY_TOPIC "\","
            "\"device\":{\"identifiers\":[\"ganymede_bridge\"],\"name\":\"Ganymede Bridge\"}}",
            SW[i].name, SW[i].id, SW[i].id, SW[i].id, SW[i].icon);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
    }
}

void mqtt_ha_publish_ac(const ac_state_t *st)
{
    if (!s_client || !s_connected) return;
    char v[8];
    esp_mqtt_client_publish(s_client, AC_BASE "mode", ac_mode_ha(st), 0, 1, true);
    snprintf(v, sizeof(v), "%d", st->temp_c);
    esp_mqtt_client_publish(s_client, AC_BASE "temp", v, 0, 1, true);
    esp_mqtt_client_publish(s_client, AC_BASE "fan", ac_fan_str(st->fan), 0, 1, true);
    esp_mqtt_client_publish(s_client, AC_BASE "swing",  st->swing  ? "ON" : "OFF", 0, 1, true);
    esp_mqtt_client_publish(s_client, AC_BASE "eco",    st->eco    ? "ON" : "OFF", 0, 1, true);
    esp_mqtt_client_publish(s_client, AC_BASE "silent", st->silent ? "ON" : "OFF", 0, 1, true);
}

void mqtt_ha_publish_nrf(const char *state)
{
    strlcpy(s_nrf, state ? state : "offline", sizeof(s_nrf));
    if (s_client && s_connected) {
        esp_mqtt_client_publish(s_client, NRF_TOPIC, s_nrf, 0, 1, true);
        esp_mqtt_client_publish(s_client, NRF_STATE_TOPIC, s_nrf, 0, 1, true);
    }
}

void mqtt_ha_publish_last_button(const char *button_name)
{
    if (!button_name || !button_name[0]) return;
    strlcpy(s_last_button, button_name, sizeof(s_last_button));
    if (s_client && s_connected)
        esp_mqtt_client_publish(s_client, LAST_BUTTON_TOPIC, s_last_button, 0, 0, true);
}

void mqtt_ha_publish_display_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_display_brightness = percent;
    if (s_client && s_connected) {
        char v[4];
        snprintf(v, sizeof(v), "%u", (unsigned)s_display_brightness);
        esp_mqtt_client_publish(s_client, DISPLAY_BRIGHTNESS_TOPIC, v, 0, 1, true);
    }
}

void mqtt_ha_publish_display_timeout(uint16_t seconds)
{
    s_display_timeout_s = seconds;
    if (s_client && s_connected) {
        char v[8];
        snprintf(v, sizeof(v), "%u", (unsigned)s_display_timeout_s);
        esp_mqtt_client_publish(s_client, DISPLAY_TIMEOUT_TOPIC, v, 0, 1, true);
    }
}

void mqtt_ha_publish_display_theme(const char *theme)
{
    if (!theme || (strcmp(theme, "dark") != 0 && strcmp(theme, "light") != 0)) {
        theme = "dark";
    }
    strlcpy(s_display_theme, theme, sizeof(s_display_theme));
    if (s_client && s_connected) {
        esp_mqtt_client_publish(s_client, DISPLAY_THEME_TOPIC, s_display_theme, 0, 1, true);
    }
}

void mqtt_ha_publish_wifi_rssi(int rssi)
{
    s_wifi_rssi = rssi;
    if (s_client && s_connected) {
        char v[8];
        snprintf(v, sizeof(v), "%d", s_wifi_rssi);
        esp_mqtt_client_publish(s_client, WIFI_RSSI_TOPIC, v, 0, 0, true);
    }
}

void mqtt_ha_publish_presence(bool present)
{
    // We only have a real presence reading when the sensor is alive, so mark it available here.
    strlcpy(s_presence, present ? "ON" : "OFF", sizeof(s_presence));
    strlcpy(s_presence_avail, "online", sizeof(s_presence_avail));
    if (s_client && s_connected) {
        esp_mqtt_client_publish(s_client, PRES_AVTY, s_presence_avail, 0, 1, true);
        esp_mqtt_client_publish(s_client, PRES_TOPIC, s_presence, 0, 1, true);
    }
}

void mqtt_ha_presence_unavailable(void)
{
    // LD2410 dropped out: don't assert OFF (that reads as "vacant"); flag the entity unavailable.
    strlcpy(s_presence_avail, "offline", sizeof(s_presence_avail));
    if (s_client && s_connected)
        esp_mqtt_client_publish(s_client, PRES_AVTY, s_presence_avail, 0, 1, true);
}

void mqtt_ha_publish_env(float t, float h, float p)
{
    mqtt_ha_publish_env_ext(t, h, p, false, 0, "Unknown");
}

void mqtt_ha_publish_env_ext(float t, float h, float p, bool gas_valid, float gas, const char *air_quality)
{
    if (gas_valid) {
        snprintf(s_last_gas, sizeof(s_last_gas), "%.0f", gas);
        strlcpy(s_air_quality, air_quality ? air_quality : "Unknown", sizeof(s_air_quality));
    }
    if (!s_client || !s_connected) return;
    char v[16];
    snprintf(v, sizeof(v), "%.2f", t); esp_mqtt_client_publish(s_client, "ganymede/env/temperature", v, 0, 0, true);
    snprintf(v, sizeof(v), "%.1f", h); esp_mqtt_client_publish(s_client, "ganymede/env/humidity", v, 0, 0, true);
    snprintf(v, sizeof(v), "%.1f", p); esp_mqtt_client_publish(s_client, "ganymede/env/pressure", v, 0, 0, true);
    snprintf(v, sizeof(v), "%.2f", t); esp_mqtt_client_publish(s_client, SENSOR_STATE_BASE "temperature", v, 0, 0, true);
    snprintf(v, sizeof(v), "%.1f", h); esp_mqtt_client_publish(s_client, SENSOR_STATE_BASE "humidity", v, 0, 0, true);
    snprintf(v, sizeof(v), "%.1f", p); esp_mqtt_client_publish(s_client, SENSOR_STATE_BASE "pressure", v, 0, 0, true);
    if (gas_valid) {
        esp_mqtt_client_publish(s_client, SENSOR_STATE_BASE "gas_resistance", s_last_gas, 0, 0, true);
        esp_mqtt_client_publish(s_client, SENSOR_STATE_BASE "air_quality", s_air_quality, 0, 0, true);
    }
}

// ---- events ----
static void on_mqtt(void *args, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        bridge_state_update_mqtt(s_host[0] != 0, true, s_host);
        ESP_LOGI(TAG, "connected to %s:%d", s_host, s_port);
        esp_mqtt_client_publish(s_client, AVTY_TOPIC, "online", 0, 1, true);
        esp_mqtt_client_publish(s_client, STATE_AVTY_TOPIC, "online", 0, 1, true);
        publish_discovery();
        esp_mqtt_client_publish(s_client, NRF_TOPIC, s_nrf, 0, 1, true);
        esp_mqtt_client_publish(s_client, NRF_STATE_TOPIC, s_nrf, 0, 1, true);
        if (s_last_button[0]) esp_mqtt_client_publish(s_client, LAST_BUTTON_TOPIC, s_last_button, 0, 0, true);
        if (s_last_gas[0]) esp_mqtt_client_publish(s_client, SENSOR_STATE_BASE "gas_resistance", s_last_gas, 0, 0, true);
        esp_mqtt_client_publish(s_client, SENSOR_STATE_BASE "air_quality", s_air_quality, 0, 0, true);
#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
        mqtt_ha_publish_display_brightness(s_display_brightness);
        mqtt_ha_publish_display_timeout(s_display_timeout_s);
        mqtt_ha_publish_display_theme(s_display_theme);
#endif
        mqtt_ha_publish_wifi_rssi(s_wifi_rssi);
        esp_mqtt_client_publish(s_client, FW_VERSION_TOPIC, esp_app_get_description()->version, 0, 1, true);
        esp_mqtt_client_publish(s_client, PRES_AVTY, s_presence_avail, 0, 1, true);
        esp_mqtt_client_publish(s_client, PRES_TOPIC, s_presence, 0, 1, true);
        esp_mqtt_client_subscribe(s_client, CMD_PREFIX "+", 1);
        esp_mqtt_client_subscribe(s_client, AC_BASE "+/set", 1);   // climate + switch commands
#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
        esp_mqtt_client_subscribe(s_client, DISPLAY_BRIGHTNESS_CMD_TOPIC, 1);
        esp_mqtt_client_subscribe(s_client, DISPLAY_TIMEOUT_CMD_TOPIC, 1);
        esp_mqtt_client_subscribe(s_client, DISPLAY_THEME_CMD_TOPIC, 1);
#endif
        { ac_state_t snap; ac_state_get_copy(&snap); mqtt_ha_publish_ac(&snap); }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        bridge_state_update_mqtt(s_host[0] != 0, false, s_host);
        break;
    case MQTT_EVENT_DATA: {
        // topic + payload arrive un-terminated; copy into bounded buffers.
        char topic[64] = {0}, payload[24] = {0};
        int tl = e->topic_len < (int)sizeof(topic) - 1 ? e->topic_len : (int)sizeof(topic) - 1;
        memcpy(topic, e->topic, tl);
        int pl = e->data_len < (int)sizeof(payload) - 1 ? e->data_len : (int)sizeof(payload) - 1;
        memcpy(payload, e->data, pl);

        if (!strncmp(topic, AC_BASE, strlen(AC_BASE))) {
            // climate / switch command: ganymede/ac/<field>/set
            const char *f = topic + strlen(AC_BASE);
            bool on = !strcmp(payload, "ON");
            if      (!strncmp(f, "mode/", 5))   ac_cmd_set_mode_ha(payload);
            else if (!strncmp(f, "temp/", 5))   ac_cmd_set_temp(atoi(payload));
            else if (!strncmp(f, "fan/", 4))    ac_cmd_set_fan(payload);
            else if (!strncmp(f, "swing/", 6))  ac_cmd_set_switch("swing", on);
            else if (!strncmp(f, "eco/", 4))    ac_cmd_set_switch("eco", on);
            else if (!strncmp(f, "silent/", 7)) ac_cmd_set_switch("silent", on);
            ESP_LOGI(TAG, "HA ac cmd '%s' = '%s'", f, payload);
        } else if (!strncmp(topic, CMD_PREFIX, strlen(CMD_PREFIX))) {
            const char *btn = topic + strlen(CMD_PREFIX);
            bool ok = bridge_press_button(btn) == ESP_OK;
            ESP_LOGI(TAG, "HA press '%s' -> %s", btn, ok ? "sent" : "invalid");
#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
        } else if (!strcmp(topic, DISPLAY_BRIGHTNESS_CMD_TOPIC)) {
            int percent = 0;
            esp_err_t err = parse_int_range(payload, 0, 100, &percent)
                            ? ui_lvgl_set_brightness_percent((uint8_t)percent)
                            : ESP_ERR_INVALID_ARG;
            ESP_LOGI(TAG, "HA display brightness '%s' -> %s", payload, esp_err_to_name(err));
        } else if (!strcmp(topic, DISPLAY_TIMEOUT_CMD_TOPIC)) {
            int seconds = 0;
            esp_err_t err = parse_int_range(payload, 10, 600, &seconds)
                            ? ui_lvgl_set_screen_timeout_s((uint16_t)seconds)
                            : ESP_ERR_INVALID_ARG;
            ESP_LOGI(TAG, "HA display timeout '%s' -> %s", payload, esp_err_to_name(err));
        } else if (!strcmp(topic, DISPLAY_THEME_CMD_TOPIC)) {
            esp_err_t err = ui_lvgl_set_theme(payload);
            ESP_LOGI(TAG, "HA display theme '%s' -> %s", payload, esp_err_to_name(err));
#endif
        }
        break;
    }
    default: break;
    }
}

// ---- lifecycle ----
static void start_client(void)
{
    if (s_client) { esp_mqtt_client_stop(s_client); esp_mqtt_client_destroy(s_client); s_client = NULL; }
    s_connected = false;
    if (!s_host[0]) { ESP_LOGI(TAG, "no broker configured — MQTT off"); return; }

    char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", s_host, s_port);
    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.uri = uri;
    if (s_user[0]) cfg.credentials.username = s_user;
    if (s_pass[0]) cfg.credentials.authentication.password = s_pass;
    cfg.session.last_will.topic = STATE_AVTY_TOPIC;
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.msg_len = 0;   // strlen
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) { ESP_LOGE(TAG, "client init failed"); return; }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    esp_mqtt_client_start(s_client);     // auto-reconnects; connects once Wi-Fi STA is up
    ESP_LOGI(TAG, "client started for %s", uri);
}

void mqtt_ha_init(void)
{
    ac_state_on_change(mqtt_ha_publish_ac);   // push climate/switch state on every model change
    cfg_load();
    bridge_state_update_mqtt(s_host[0] != 0, false, s_host);
    start_client();
}

bool mqtt_ha_save(const char *host, int port, const char *user, const char *pass)
{
    strlcpy(s_host, host ? host : "", sizeof(s_host));
    strlcpy(s_user, user ? user : "", sizeof(s_user));
    strlcpy(s_pass, pass ? pass : "", sizeof(s_pass));
    s_port = (port > 0 && port < 65536) ? port : 1883;
    cfg_save();
    bridge_state_update_mqtt(s_host[0] != 0, false, s_host);
    start_client();
    return true;
}

bool mqtt_ha_connected(void)   { return s_connected; }
const char *mqtt_ha_host(void) { return s_host; }
