#include "web.h"
#include "wifi_mgr.h"
#include "uart_link.h"
#include "mqtt_ha.h"
#include "bme280.h"
#include "pins.h"
#include "rules.h"
#include "ld2410.h"
#include "ac_state.h"
#include "ac_cmd.h"
#include "board_config.h"
#include "bridge_state.h"
#include "command_queue.h"
#include "ui_lvgl.h"
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "web";

extern const char  index_html_start[] asm("_binary_index_html_start");
extern const char  index_html_end[]   asm("_binary_index_html_end");

// ---- tiny helpers ----
static void urldecode(char *s)
{
    char *d = s;
    for (; *s; s++) {
        if (*s == '+') { *d++ = ' '; }
        else if (*s == '%' && s[1] && s[2]) {
            char h[3] = { s[1], s[2], 0 };
            *d++ = (char)strtol(h, NULL, 16); s += 2;
        } else *d++ = *s;
    }
    *d = 0;
}

// extract value of `key` from an x-www-form-urlencoded buffer into out (decoded).
static bool form_get(const char *body, const char *key, char *out, size_t outlen)
{
    size_t kl = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            const char *e = strchr(v, '&'); size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= outlen) n = outlen - 1;
            memcpy(out, v, n); out[n] = 0; urldecode(out); return true;
        }
        p = strchr(p, '&'); if (p) p++;
    }
    return false;
}

static bool json_string_get(const char *body, const char *key, char *out, size_t outlen)
{
    char needle[24];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(body, needle);
    if (!p) return false;
    p += strlen(needle);
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return false;
    size_t n = (size_t)(e - p);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

static bool json_value_get(const char *body, const char *key, char *out, size_t outlen)
{
    char needle[24];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(body, needle);
    if (!p) return false;
    p += strlen(needle);
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') return json_string_get(body, key, out, outlen);

    const char *e = p;
    while (*e && *e != ',' && *e != '}' && *e != '\r' && *e != '\n' &&
           *e != ' ' && *e != '\t') {
        e++;
    }
    size_t n = (size_t)(e - p);
    if (!n) return false;
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

static void json_putc(char *out, size_t outlen, size_t *pos, char c)
{
    if (!out || !outlen || !pos) return;
    if (*pos + 1 < outlen) {
        out[*pos] = c;
    }
    (*pos)++;
}

static void json_escape_str(const char *src, char *out, size_t outlen)
{
    if (!out || !outlen) return;
    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++) {
        switch (*p) {
        case '"':  json_putc(out, outlen, &pos, '\\'); json_putc(out, outlen, &pos, '"'); break;
        case '\\': json_putc(out, outlen, &pos, '\\'); json_putc(out, outlen, &pos, '\\'); break;
        case '\b': json_putc(out, outlen, &pos, '\\'); json_putc(out, outlen, &pos, 'b'); break;
        case '\f': json_putc(out, outlen, &pos, '\\'); json_putc(out, outlen, &pos, 'f'); break;
        case '\n': json_putc(out, outlen, &pos, '\\'); json_putc(out, outlen, &pos, 'n'); break;
        case '\r': json_putc(out, outlen, &pos, '\\'); json_putc(out, outlen, &pos, 'r'); break;
        case '\t': json_putc(out, outlen, &pos, '\\'); json_putc(out, outlen, &pos, 't'); break;
        default:
            json_putc(out, outlen, &pos, *p < 0x20 ? '?' : (char)*p);
            break;
        }
    }
    out[pos < outlen ? pos : outlen - 1] = 0;
}

static int read_body(httpd_req_t *req, char *buf, size_t buflen)
{
    int total = 0;
    while (total < (int)buflen - 1) {
        int r = httpd_req_recv(req, buf + total, buflen - 1 - total);
        if (r <= 0) break;
        total += r;
        if (total >= (int)req->content_len) break;
    }
    buf[total] = 0;
    return total;
}

// ---- handlers ----
static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_status(httpd_req_t *req)
{
    env_sensor_sample_t sensor = { .type = bme280_sensor_type(), .air_quality = "Unknown" };
    bool bme = bme280_get_sample(&sensor);
    float t = sensor.temperature_c, h = sensor.humidity_percent, p = sensor.pressure_hpa;
    ac_state_t ac; ac_state_get_copy(&ac);
    const char *acmode = ac.mode == AC_MODE_DRY ? "dry" : ac.mode == AC_MODE_FAN ? "fan" : "cool";
    const char *actimer = ac.timer_state == TIMER_RUN ? "run" : ac.timer_state == TIMER_EDIT ? "edit" : "off";
    const esp_app_desc_t *app = esp_app_get_description();
    bridge_state_t st;
    bridge_state_get_snapshot(&st);
    char last_button[16] = "";
    uint64_t last_button_ms = 0;
    bool last_sent = false;
    bool has_last_button = bridge_last_button(last_button, sizeof(last_button), &last_button_ms, &last_sent);
    uint64_t now_ms = esp_timer_get_time() / 1000ULL;
    uint64_t last_age_ms = has_last_button && now_ms >= last_button_ms ? now_ms - last_button_ms : 0;
    uint64_t nrf_seen_age_ms = st.nrf_last_seen_ms && now_ms >= st.nrf_last_seen_ms
                               ? now_ms - st.nrf_last_seen_ms : 0;
    uint64_t sensor_age_ms = st.sensor_last_update_ms && now_ms >= st.sensor_last_update_ms
                             ? now_ms - st.sensor_last_update_ms : 0;
    char wifi_state[24], legacy_ssid[70], legacy_ip[40], legacy_ap[70], nrf_status[24];
    char mqtt_host[140], wifi_ssid[70], wifi_ip[40], mqtt_detail_host[140];
    char nrf_detail_status[24], nrf_last_message[140], sensor_type[32], air_quality[40];
    char display_screen[32], display_theme[24], disabled_reason[96];
    char command_button[40], command_error[128], app_version[80];
    json_escape_str(wifi_mgr_state_str(), wifi_state, sizeof(wifi_state));
    json_escape_str(wifi_mgr_ssid(), legacy_ssid, sizeof(legacy_ssid));
    json_escape_str(wifi_mgr_ip(), legacy_ip, sizeof(legacy_ip));
    json_escape_str(wifi_mgr_ap_ssid(), legacy_ap, sizeof(legacy_ap));
    json_escape_str(uart_link_status(), nrf_status, sizeof(nrf_status));
    json_escape_str(mqtt_ha_host(), mqtt_host, sizeof(mqtt_host));
    json_escape_str(st.wifi_ssid, wifi_ssid, sizeof(wifi_ssid));
    json_escape_str(st.wifi_ip, wifi_ip, sizeof(wifi_ip));
    json_escape_str(st.mqtt_host, mqtt_detail_host, sizeof(mqtt_detail_host));
    json_escape_str(st.nrf_status, nrf_detail_status, sizeof(nrf_detail_status));
    json_escape_str(st.nrf_last_message, nrf_last_message, sizeof(nrf_last_message));
    json_escape_str(st.sensor_type[0] ? st.sensor_type : (sensor.type ? sensor.type : "unknown"),
                    sensor_type, sizeof(sensor_type));
    json_escape_str(st.air_quality[0] ? st.air_quality : "Unknown", air_quality, sizeof(air_quality));
    json_escape_str(st.current_screen, display_screen, sizeof(display_screen));
    json_escape_str(st.theme, display_theme, sizeof(display_theme));
    json_escape_str(ui_lvgl_remote_disabled_reason(), disabled_reason, sizeof(disabled_reason));
    json_escape_str(last_button, command_button, sizeof(command_button));
    json_escape_str(st.last_error, command_error, sizeof(command_error));
    json_escape_str(app ? app->version : "unknown", app_version, sizeof(app_version));

    const size_t buflen = 4096;
    char *buf = malloc(buflen);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status allocation failed");
        return ESP_ERR_NO_MEM;
    }
    int written = snprintf(buf, buflen,
        "{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"ap\":\"%s\",\"has_creds\":%s,\"nrf\":\"%s\","
        "\"mqtt\":%s,\"mqtt_host\":\"%s\",\"bme\":%s,\"temp\":%.2f,\"hum\":%.1f,\"hpa\":%.1f,"
        "\"ld\":%s,\"presence\":%s,\"presence_s\":%lu,\"mute_s\":%d,"
        "\"ac\":{\"on\":%s,\"mode\":\"%s\",\"temp\":%d,\"fan\":\"%s\",\"silent\":%s,\"eco\":%s,\"swing\":%s,"
        "\"timer\":\"%s\",\"timer_h\":%.1f},"
        "\"firmware\":{\"name\":\"%s\",\"version\":\"%s\",\"board\":\"%s\"},"
        "\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d},"
        "\"mqtt_detail\":{\"configured\":%s,\"connected\":%s,\"host\":\"%s\"},"
        "\"nrf_detail\":{\"available\":%s,\"status\":\"%s\",\"last_seen_ms\":%" PRIu64 ","
        "\"last_message\":\"%s\"},"
        "\"sensor\":{\"type\":\"%s\",\"available\":%s,\"temperature_c\":%.2f,"
        "\"humidity_percent\":%.1f,\"pressure_hpa\":%.1f,"
        "\"gas_resistance_ohm\":%lu,\"air_quality\":\"%s\",\"last_update_ms\":%" PRIu64 ","
        "\"last_update_age_ms\":%" PRIu64 "},"
        "\"display\":{\"available\":%s,\"touch_available\":%s,\"brightness_percent\":%u,"
        "\"backlight_on\":%s,\"screen\":\"%s\",\"theme\":\"%s\",\"screen_timeout_s\":%u,"
        "\"remote_buttons_enabled\":%s,\"disabled_reason\":\"%s\"},"
        "\"command\":{\"has_last\":%s,\"last_button\":\"%s\",\"last_sent\":%s,"
        "\"last_age_ms\":%" PRIu64 ",\"last_error\":\"%s\"},"
        "\"runtime\":{\"uptime_s\":%lu,\"free_heap\":%lu}}",
        wifi_state, legacy_ssid, legacy_ip, legacy_ap,
        wifi_mgr_has_creds() ? "true" : "false", nrf_status,
        mqtt_ha_connected() ? "true" : "false", mqtt_host,
        bme ? "true" : "false", t, h, p,
        ld2410_alive() ? "true" : "false", rules_presence() ? "true" : "false",
        (unsigned long)rules_presence_secs(), uart_link_mute_secs(),
        ac.on ? "true" : "false", acmode, ac.temp_c, ac_fan_str(ac.fan),
        ac.silent ? "true" : "false", ac.eco ? "true" : "false", ac.swing ? "true" : "false",
        actimer, ac.timer_halfh / 2.0,
        CONFIG_PINGUINO_FIRMWARE_NAME, app_version, BOARD_NAME,
        st.wifi_connected ? "true" : "false", wifi_ssid, wifi_ip, st.wifi_rssi,
        st.mqtt_configured ? "true" : "false", st.mqtt_connected ? "true" : "false", mqtt_detail_host,
        st.nrf_available ? "true" : "false", nrf_detail_status, nrf_seen_age_ms, nrf_last_message,
        sensor_type,
        st.sensor_available ? "true" : "false", st.temperature_c, st.humidity_percent, st.pressure_hpa,
        (unsigned long)st.gas_resistance_ohm,
        air_quality, st.sensor_last_update_ms, sensor_age_ms,
        st.display_available ? "true" : "false",
        st.touch_available ? "true" : "false",
        (unsigned)st.brightness_percent,
        st.backlight_on ? "true" : "false",
        display_screen, display_theme, (unsigned)ui_lvgl_screen_timeout_s(),
        ui_lvgl_remote_buttons_enabled() ? "true" : "false",
        disabled_reason,
        has_last_button ? "true" : "false", command_button,
        last_sent ? "true" : "false", last_age_ms, command_error,
        (unsigned long)st.uptime_s, (unsigned long)st.free_heap);
    if (written < 0 || written >= (int)buflen) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status response too large");
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

static esp_err_t h_scan(httpd_req_t *req)
{
    wm_ap_t aps[20];
    int n = wifi_mgr_scan(aps, 20);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < n; i++) {
        char ssid[70], item[128];
        json_escape_str(aps[i].ssid, ssid, sizeof(ssid));
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                 i ? "," : "", ssid, aps[i].rssi, aps[i].authmode);
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t h_connect(httpd_req_t *req)
{
    char body[160]; read_body(req, body, sizeof(body));
    char ssid[33] = "", pass[65] = "";
    form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "pass", pass, sizeof(pass));
    bool ok = wifi_mgr_connect(ssid, pass);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t h_press(httpd_req_t *req)
{
    char body[48] = ""; char btn[16] = "";
    read_body(req, body, sizeof(body));
    if (!form_get(body, "btn", btn, sizeof(btn))) {
        // also accept ?btn=... in the query
        char q[48]; if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
            httpd_query_key_value(q, "btn", btn, sizeof(btn));
    }
    if (!btn[0]) json_string_get(body, "button", btn, sizeof(btn));
    esp_err_t err = bridge_press_button(btn);
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        char out[64];
        snprintf(out, sizeof(out), "{\"ok\":true,\"button\":\"%s\",\"queued\":true}", btn);
        return httpd_resp_sendstr(req, out);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"nrf_unavailable\"}");
    }
    if (err == ESP_ERR_TIMEOUT) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"queue_full\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_button\"}");
}

static esp_err_t h_ui(httpd_req_t *req)
{
    char body[220] = "";
    read_body(req, body, sizeof(body));
    char v[24] = "";
    esp_err_t err = ESP_OK;
    const char *error = NULL;

    if (form_get(body, "screen", v, sizeof(v)) || json_value_get(body, "screen", v, sizeof(v))) {
        err = ui_lvgl_set_screen(v);
        if (err != ESP_OK) error = "bad_screen";
    }

    if (!error && (form_get(body, "brightness", v, sizeof(v)) ||
                   json_value_get(body, "brightness", v, sizeof(v)))) {
        char *end = NULL;
        long percent = strtol(v, &end, 10);
        if (!v[0] || (end && *end) || percent < 0 || percent > 100) {
            error = "bad_brightness";
        } else {
            err = ui_lvgl_set_brightness_percent((uint8_t)percent);
            if (err != ESP_OK) error = "bad_brightness";
        }
    }

    if (!error && (form_get(body, "timeout", v, sizeof(v)) ||
                   json_value_get(body, "timeout", v, sizeof(v)))) {
        char *end = NULL;
        long seconds = strtol(v, &end, 10);
        if (!v[0] || (end && *end) || seconds < 0 || seconds > UINT16_MAX) {
            error = "bad_timeout";
        } else {
            err = ui_lvgl_set_screen_timeout_s((uint16_t)seconds);
            if (err != ESP_OK) error = "bad_timeout";
        }
    }

    if (!error && (form_get(body, "theme", v, sizeof(v)) || json_value_get(body, "theme", v, sizeof(v)))) {
        err = ui_lvgl_set_theme(v);
        if (err != ESP_OK) error = "bad_theme";
    }

    if (!error && (form_get(body, "wake", v, sizeof(v)) ||
                   json_value_get(body, "wake", v, sizeof(v)))) {
        if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "on")) {
            ui_lvgl_note_activity();
        }
    }

    if (!error &&
        (form_get(body, "button", v, sizeof(v)) ||
         form_get(body, "btn", v, sizeof(v)) ||
         json_string_get(body, "button", v, sizeof(v)))) {
        err = ui_lvgl_press_button(v);
        if (err == ESP_ERR_INVALID_STATE) error = "nrf_unavailable";
        else if (err == ESP_ERR_TIMEOUT) error = "queue_full";
        else if (err != ESP_OK) error = "bad_button";
    }

    httpd_resp_set_type(req, "application/json");
    if (error) {
        char out[64];
        snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\"}", error);
        return httpd_resp_sendstr(req, out);
    }

    char out[220];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"screen\":\"%s\",\"brightness_percent\":%u,"
             "\"screen_timeout_s\":%u,\"theme\":\"%s\",\"backlight_on\":%s}",
             ui_lvgl_current_screen(), (unsigned)ui_lvgl_brightness_percent(),
             (unsigned)ui_lvgl_screen_timeout_s(), ui_lvgl_theme(),
             ui_lvgl_backlight_on() ? "true" : "false");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t h_pairing(httpd_req_t *req)
{
    // /api/unpair clears the bond and re-enters pairing mode; /api/pair re-kicks advertising.
    bool unpair = strstr(req->uri, "unpair") != NULL;
    uart_link_pairing(unpair);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t h_mqtt(httpd_req_t *req)
{
    char body[220]; read_body(req, body, sizeof(body));
    char host[64] = "", ports[8] = "", user[48] = "", pass[64] = "";
    form_get(body, "host", host, sizeof(host));
    form_get(body, "port", ports, sizeof(ports));
    form_get(body, "user", user, sizeof(user));
    form_get(body, "pass", pass, sizeof(pass));
    bool ok = mqtt_ha_save(host, ports[0] ? atoi(ports) : 1883, user, pass);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// ---- pin configuration ----
static esp_err_t h_pins(httpd_req_t *req)
{
    const device_pins_t *p = pins_get();
    char buf[200];
    snprintf(buf, sizeof(buf),
        "{\"i2c_sda\":%d,\"i2c_scl\":%d,\"nrf_tx\":%d,\"nrf_rx\":%d,\"nrf_hb\":%d,"
        "\"ld_tx\":%d,\"ld_rx\":%d}",
        p->i2c_sda, p->i2c_scl, p->nrf_tx, p->nrf_rx, p->nrf_hb, p->ld_tx, p->ld_rx);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

// Parse one pin field. Returns: 0 = absent (keep current), 1 = set ok,
// -1 = present but not a valid GPIO. Validating as int before narrowing to int8_t
// stops out-of-range/non-numeric input from wrapping into a "valid" pin (300 -> 44).
static int form_pin(const char *body, const char *key, int8_t *out, bool allow_disabled)
{
    char v[8];
    if (!form_get(body, key, v, sizeof(v)) || !v[0]) return 0;
    char *end = NULL;
    long g = strtol(v, &end, 10);
    if (!end || *end) return -1;
    if (allow_disabled && g == -1) {
        *out = -1;
        return 1;
    }
    if (!pins_valid_gpio(g)) return -1;
    *out = (int8_t)g;
    return 1;
}

static esp_err_t h_pins_save(httpd_req_t *req)
{
    char body[220]; read_body(req, body, sizeof(body));
    device_pins_t p = *pins_get();   // start from current, override the fields that were sent
    bool bad = false;
    bad |= form_pin(body, "i2c_sda", &p.i2c_sda, false) < 0;
    bad |= form_pin(body, "i2c_scl", &p.i2c_scl, false) < 0;
    bad |= form_pin(body, "nrf_tx",  &p.nrf_tx,  false) < 0;
    bad |= form_pin(body, "nrf_rx",  &p.nrf_rx,  false) < 0;
    bad |= form_pin(body, "nrf_hb",  &p.nrf_hb,  true)  < 0;
    bad |= form_pin(body, "ld_tx",   &p.ld_tx,   false) < 0;
    bad |= form_pin(body, "ld_rx",   &p.ld_rx,   false) < 0;
    bool ok = !bad && pins_save(&p);  // pins_save re-validates as defence in depth
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void reboot_task(void *a)
{
    vTaskDelay(pdMS_TO_TICKS(500));   // let the HTTP response flush first
    esp_restart();
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ---- presence rules ----
// GET returns hand-rolled JSON (chunked, like h_scan); POST takes indexed form fields
// (n + en<i>/cond<i>/dur<i>/tgt<i>), parsed with form_get — no JSON lib needed.
static esp_err_t h_rules(httpd_req_t *req)
{
    rule_t rs[RULES_MAX];
    int n = rules_get(rs, RULES_MAX);
    httpd_resp_set_type(req, "application/json");
    char head[64];
    snprintf(head, sizeof(head), "{\"present\":%s,\"presence_s\":%lu,\"rules\":[",
             rules_presence() ? "true" : "false", (unsigned long)rules_presence_secs());
    httpd_resp_sendstr_chunk(req, head);
    for (int i = 0; i < n; i++) {
        char item[160];
        snprintf(item, sizeof(item),
            "%s{\"enabled\":%s,\"cond\":\"%s\",\"duration_s\":%lu,\"target\":\"%s\"}",
            i ? "," : "", rs[i].enabled ? "true" : "false",
            rs[i].cond == RULE_COND_ABSENCE ? "absence" : "presence",
            (unsigned long)rs[i].duration_s, rs[i].target);
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// Parse a non-negative seconds field, clamped to RULE_MAX_SECS so large values can't wrap.
static uint32_t form_secs(const char *body, const char *key)
{
    char v[16];
    if (!form_get(body, key, v, sizeof(v))) return 0;
    long s = strtol(v, NULL, 10);
    if (s < 0) s = 0;
    if (s > (long)RULE_MAX_SECS) s = RULE_MAX_SECS;
    return (uint32_t)s;
}

static esp_err_t h_rules_save(httpd_req_t *req)
{
    char body[768]; read_body(req, body, sizeof(body));
    char cnt[8] = "0"; form_get(body, "n", cnt, sizeof(cnt));
    int n = atoi(cnt);
    if (n < 0) n = 0;
    if (n > RULES_MAX) n = RULES_MAX;

    rule_t rs[RULES_MAX];
    for (int i = 0; i < n; i++) {
        memset(&rs[i], 0, sizeof(rule_t));
        char key[8], v[16];
        snprintf(key, sizeof(key), "en%d", i);   rs[i].enabled = form_get(body, key, v, sizeof(v)) && atoi(v);
        snprintf(key, sizeof(key), "cond%d", i);
        rs[i].cond = (form_get(body, key, v, sizeof(v)) && !strcmp(v, "absence"))
                     ? RULE_COND_ABSENCE : RULE_COND_PRESENCE;
        snprintf(key, sizeof(key), "dur%d", i);   rs[i].duration_s = form_secs(body, key);
        snprintf(key, sizeof(key), "tgt%d", i);   form_get(body, key, rs[i].target, sizeof(rs[i].target));
    }
    bool ok = rules_set(rs, n);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// ---- AC model: sync ("set current state") + climate commands ----
static ac_mode_t mode_from_form(const char *v)
{
    if (!strcmp(v, "dry")) return AC_MODE_DRY;
    if (!strcmp(v, "fan")) return AC_MODE_FAN;
    return AC_MODE_COOL;
}

// POST /api/acstate — overwrite the model to match reality (no presses sent).
static esp_err_t h_acstate(httpd_req_t *req)
{
    char body[200]; read_body(req, body, sizeof(body));
    ac_state_t st; ac_state_get_copy(&st);   // start from current, override sent fields
    char v[12];
    if (form_get(body, "on", v, sizeof(v)))     st.on     = atoi(v) != 0;
    if (form_get(body, "mode", v, sizeof(v)))   st.mode   = mode_from_form(v);
    if (form_get(body, "temp", v, sizeof(v)))   st.temp_c = (uint8_t)atoi(v);
    if (form_get(body, "fan", v, sizeof(v)))    { ac_fan_t f; if (ac_fan_from_str(v, &f)) st.fan = f; }
    if (form_get(body, "silent", v, sizeof(v))) st.silent = atoi(v) != 0;
    if (form_get(body, "eco", v, sizeof(v)))    st.eco    = atoi(v) != 0;
    if (form_get(body, "swing", v, sizeof(v)))  st.swing  = atoi(v) != 0;
    ac_state_set(&st);   // normalises, persists, pushes HA state
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// POST /api/mute — open a "sync window": presses update the model but aren't sent to the AC,
// so the user can re-align the model to the real AC by pressing the remote. ?s=<secs>, default 30.
static esp_err_t h_mute(httpd_req_t *req)
{
    char body[24] = ""; read_body(req, body, sizeof(body));
    char v[8] = ""; int secs = 30;
    if (form_get(body, "s", v, sizeof(v)) && v[0]) secs = atoi(v);
    if (secs < 0) secs = 0;
    if (secs > 300) secs = 300;
    uart_link_mute(secs);
    char out[32]; snprintf(out, sizeof(out), "{\"ok\":true,\"s\":%d}", secs);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

// POST /api/accmd — drive the AC toward a target via the press-sequence worker (one field per call).
static esp_err_t h_accmd(httpd_req_t *req)
{
    char body[64]; read_body(req, body, sizeof(body));
    char v[12];
    if (form_get(body, "mode", v, sizeof(v)))   ac_cmd_set_mode_ha(v);   // off/cool/dry/fan_only
    else if (form_get(body, "temp", v, sizeof(v))) ac_cmd_set_temp(atoi(v));
    else if (form_get(body, "fan", v, sizeof(v)))  ac_cmd_set_fan(v);
    else if (form_get(body, "swing", v, sizeof(v)))  ac_cmd_set_switch("swing",  atoi(v) != 0);
    else if (form_get(body, "eco", v, sizeof(v)))    ac_cmd_set_switch("eco",    atoi(v) != 0);
    else if (form_get(body, "silent", v, sizeof(v))) ac_cmd_set_switch("silent", atoi(v) != 0);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void reg(httpd_handle_t s, const char *uri, httpd_method_t m, esp_err_t (*fn)(httpd_req_t*))
{
    httpd_uri_t u = { .uri = uri, .method = m, .handler = fn };
    httpd_register_uri_handler(s, &u);
}

void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.lru_purge_enable = true;
    httpd_handle_t s = NULL;
    if (httpd_start(&s, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }
    reg(s, "/", HTTP_GET, h_index);
    reg(s, "/api/status", HTTP_GET, h_status);
    reg(s, "/api/scan", HTTP_GET, h_scan);
    reg(s, "/api/connect", HTTP_POST, h_connect);
    reg(s, "/api/press", HTTP_POST, h_press);
    reg(s, "/api/ui", HTTP_POST, h_ui);
    reg(s, "/api/pair", HTTP_POST, h_pairing);
    reg(s, "/api/unpair", HTTP_POST, h_pairing);
    reg(s, "/api/mqtt", HTTP_POST, h_mqtt);
    reg(s, "/api/pins", HTTP_GET, h_pins);
    reg(s, "/api/pins", HTTP_POST, h_pins_save);
    reg(s, "/api/reboot", HTTP_POST, h_reboot);
    reg(s, "/api/rules", HTTP_GET, h_rules);
    reg(s, "/api/rules", HTTP_POST, h_rules_save);
    reg(s, "/api/acstate", HTTP_POST, h_acstate);
    reg(s, "/api/accmd", HTTP_POST, h_accmd);
    reg(s, "/api/mute", HTTP_POST, h_mute);
    ESP_LOGI(TAG, "web server up on :80");
}
