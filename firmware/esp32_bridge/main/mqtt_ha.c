#include "mqtt_ha.h"
#include "uart_link.h"
#include <string.h>
#include <stdio.h>
#include "mqtt_client.h"
#include "esp_log.h"
#include "nvs.h"

#define NVS_NS    "mqtt"
#define AVTY_TOPIC "ganymede/status"
#define CMD_PREFIX "ganymede/cmd/"
#define NRF_TOPIC  "ganymede/nrf"

static const char *TAG = "mqtt";

// HA buttons (name = UART/btn id, label = HA entity name).
static const struct { const char *name, *label; } BTNS[] = {
    {"power","Power"}, {"up","Up"}, {"down","Down"}, {"mode","Mode"}, {"eco","Eco"},
    {"timer","Timer"}, {"fan","Fan"}, {"silent","Silent"}, {"flap","Flap"},
};
#define NBTN (sizeof(BTNS)/sizeof(BTNS[0]))

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_nrf[16] = "offline";   // last nRF link state, republished on (re)connect
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
    char topic[96], payload[420];
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
    // nRF link state -> diagnostic sensor
    snprintf(topic, sizeof(topic), "homeassistant/sensor/ganymede_nrf/config");
    snprintf(payload, sizeof(payload),
        "{\"name\":\"nRF Link\",\"unique_id\":\"ganymede_nrf\",\"state_topic\":\"" NRF_TOPIC "\","
        "\"icon\":\"mdi:bluetooth\",\"entity_category\":\"diagnostic\",\"availability_topic\":\"" AVTY_TOPIC "\","
        "\"device\":{\"identifiers\":[\"ganymede_bridge\"],\"name\":\"Ganymede Bridge\"}}");
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
}

void mqtt_ha_publish_nrf(const char *state)
{
    strlcpy(s_nrf, state ? state : "offline", sizeof(s_nrf));
    if (s_client && s_connected)
        esp_mqtt_client_publish(s_client, NRF_TOPIC, s_nrf, 0, 1, true);
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
        esp_mqtt_client_publish(s_client, NRF_TOPIC, s_nrf, 0, 1, true);
        esp_mqtt_client_subscribe(s_client, CMD_PREFIX "+", 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        break;
    case MQTT_EVENT_DATA: {
        // topic = "ganymede/cmd/<btn>" (not null-terminated)
        char btn[16] = {0};
        const char *slash = NULL;
        for (int k = e->topic_len - 1; k >= 0; k--) if (e->topic[k] == '/') { slash = &e->topic[k+1]; break; }
        if (slash) {
            int len = (int)(&e->topic[e->topic_len] - slash);
            if (len > 0 && len < (int)sizeof(btn)) { memcpy(btn, slash, len); btn[len] = 0; }
        }
        if (btn[0]) {
            bool ok = uart_link_press(btn);
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

void mqtt_ha_init(void) { cfg_load(); start_client(); }

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
