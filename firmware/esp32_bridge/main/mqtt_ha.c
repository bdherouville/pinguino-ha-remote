#include "mqtt_ha.h"
#include "ble_emu.h"
#include "ac_state.h"
#include "ac_cmd.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "mqtt_client.h"
#include "esp_log.h"
#include "nvs.h"

#define NVS_NS    "mqtt"
#define AVTY_TOPIC "ganymede/status"
#define CMD_PREFIX "ganymede/cmd/"
// Kept as ganymede/nrf (and unique_id ganymede_nrf below) even though the emulator now
// runs on this chip: renaming would orphan the existing Home Assistant entity.
#define LINK_TOPIC "ganymede/nrf"
#define PRES_TOPIC "ganymede/presence"
#define PRES_AVTY  "ganymede/presence/status"   // per-sensor availability (LD2410 alive?)
#define AC_BASE    "ganymede/ac/"                // climate state/<x> + command <x>/set

static const char *TAG = "mqtt";

// HA buttons (name = emulator button id, label = HA entity name).
static const struct { const char *name, *label; } BTNS[] = {
    {"power","Power"}, {"up","Up"}, {"down","Down"}, {"mode","Mode"}, {"eco","Eco"},
    {"timer","Timer"}, {"fan","Fan"}, {"silent","Silent"}, {"flap","Flap"},
};
#define NBTN (sizeof(BTNS)/sizeof(BTNS[0]))

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_link[16] = "offline";  // last emulator state, republished on (re)connect
static char s_presence[4] = "OFF";      // last LD2410 presence, republished on (re)connect
static char s_presence_avail[8] = "offline";   // LD2410 availability (online once frames seen)
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

// ---- HA discovery ----
static void publish_discovery(void)
{
    char topic[96], payload[640];
    for (size_t i = 0; i < NBTN; i++) {
        snprintf(topic, sizeof(topic), "homeassistant/button/ganymede_%s/config", BTNS[i].name);
        snprintf(payload, sizeof(payload),
            "{\"name\":\"%s\",\"unique_id\":\"ganymede_%s\","
            "\"command_topic\":\"" CMD_PREFIX "%s\",\"payload_press\":\"PRESS\","
            "\"availability_topic\":\"" AVTY_TOPIC "\","
            "\"device\":{\"identifiers\":[\"ganymede_bridge\"],\"name\":\"Ganymede Bridge\","
            "\"manufacturer\":\"DIY\",\"model\":\"De'Longhi remote emulator\"}}",
            BTNS[i].label, BTNS[i].name, BTNS[i].name);
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
            "{\"name\":\"%s\",\"unique_id\":\"ganymede_%s\",\"state_topic\":\"ganymede/env/%s\","
            "\"unit_of_measurement\":\"%s\",\"device_class\":\"%s\",\"availability_topic\":\"" AVTY_TOPIC "\","
            "\"device\":{\"identifiers\":[\"ganymede_bridge\"],\"name\":\"Ganymede Bridge\"}}",
            S[i].name, S[i].id, S[i].id, S[i].unit, S[i].dc);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
    }
    // BLE emulator state -> diagnostic sensor
    snprintf(topic, sizeof(topic), "homeassistant/sensor/ganymede_nrf/config");
    snprintf(payload, sizeof(payload),
        "{\"name\":\"BLE Link\",\"unique_id\":\"ganymede_nrf\",\"state_topic\":\"" LINK_TOPIC "\","
        "\"icon\":\"mdi:bluetooth\",\"entity_category\":\"diagnostic\",\"availability_topic\":\"" AVTY_TOPIC "\","
        "\"device\":{\"identifiers\":[\"ganymede_bridge\"],\"name\":\"Ganymede Bridge\"}}");
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
    // LD2410 presence -> occupancy binary_sensor. Availability requires BOTH the bridge to be
    // online AND the LD2410 to be producing frames, so a dead/unwired sensor shows "unavailable"
    // in HA instead of a false "vacant" that could drive absence automations.
    snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/ganymede_presence/config");
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Presence\",\"unique_id\":\"ganymede_presence\",\"state_topic\":\"" PRES_TOPIC "\","
        "\"device_class\":\"occupancy\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
        "\"availability\":[{\"topic\":\"" AVTY_TOPIC "\"},{\"topic\":\"" PRES_AVTY "\"}],"
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
        "\"availability_topic\":\"" AVTY_TOPIC "\","
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
            "\"availability_topic\":\"" AVTY_TOPIC "\","
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

void mqtt_ha_publish_link(const char *state)
{
    strlcpy(s_link, state ? state : "offline", sizeof(s_link));
    if (s_client && s_connected)
        esp_mqtt_client_publish(s_client, LINK_TOPIC, s_link, 0, 1, true);
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
    if (!s_client || !s_connected) return;
    char v[16];
    snprintf(v, sizeof(v), "%.2f", t); esp_mqtt_client_publish(s_client, "ganymede/env/temperature", v, 0, 0, true);
    snprintf(v, sizeof(v), "%.1f", h); esp_mqtt_client_publish(s_client, "ganymede/env/humidity", v, 0, 0, true);
    snprintf(v, sizeof(v), "%.1f", p); esp_mqtt_client_publish(s_client, "ganymede/env/pressure", v, 0, 0, true);
}

// ---- events ----
static void on_mqtt(void *args, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected to %s:%d", s_host, s_port);
        esp_mqtt_client_publish(s_client, AVTY_TOPIC, "online", 0, 1, true);
        publish_discovery();
        esp_mqtt_client_publish(s_client, LINK_TOPIC, s_link, 0, 1, true);
        esp_mqtt_client_publish(s_client, PRES_AVTY, s_presence_avail, 0, 1, true);
        esp_mqtt_client_publish(s_client, PRES_TOPIC, s_presence, 0, 1, true);
        esp_mqtt_client_subscribe(s_client, CMD_PREFIX "+", 1);
        esp_mqtt_client_subscribe(s_client, AC_BASE "+/set", 1);   // climate + switch commands
        { ac_state_t snap; ac_state_get_copy(&snap); mqtt_ha_publish_ac(&snap); }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
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
            bool ok = ble_emu_press(btn);
            ESP_LOGI(TAG, "HA press '%s' -> %s", btn, ok ? "sent" : "invalid");
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
    cfg.session.last_will.topic = AVTY_TOPIC;
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
    start_client();
}

bool mqtt_ha_save(const char *host, int port, const char *user, const char *pass)
{
    strlcpy(s_host, host ? host : "", sizeof(s_host));
    strlcpy(s_user, user ? user : "", sizeof(s_user));
    strlcpy(s_pass, pass ? pass : "", sizeof(s_pass));
    s_port = (port > 0 && port < 65536) ? port : 1883;
    cfg_save();
    start_client();
    return true;
}

bool mqtt_ha_connected(void)   { return s_connected; }
const char *mqtt_ha_host(void) { return s_host; }
