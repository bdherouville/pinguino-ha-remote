#include "web.h"
#include "wifi_mgr.h"
#include "uart_link.h"
#include "mqtt_ha.h"
#include "bme280.h"
#include "pins.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
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
    float t = 0, h = 0, p = 0; bool bme = bme280_get(&t, &h, &p);
    char buf[440];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"ap\":\"%s\",\"has_creds\":%s,\"nrf\":\"%s\","
        "\"mqtt\":%s,\"mqtt_host\":\"%s\",\"bme\":%s,\"temp\":%.2f,\"hum\":%.1f,\"hpa\":%.1f}",
        wifi_mgr_state_str(), wifi_mgr_ssid(), wifi_mgr_ip(), wifi_mgr_ap_ssid(),
        wifi_mgr_has_creds() ? "true" : "false", uart_link_status(),
        mqtt_ha_connected() ? "true" : "false", mqtt_ha_host(),
        bme ? "true" : "false", t, h, p);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    wm_ap_t aps[20];
    int n = wifi_mgr_scan(aps, 20);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < n; i++) {
        char item[80];
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                 i ? "," : "", aps[i].ssid, aps[i].rssi, aps[i].authmode);
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
    bool ok = btn[0] && uart_link_press(btn);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
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

static void form_u8(const char *body, const char *key, uint8_t *out)
{
    char v[8];
    if (form_get(body, key, v, sizeof(v)) && v[0]) *out = (uint8_t)atoi(v);
}

static esp_err_t h_pins_save(httpd_req_t *req)
{
    char body[220]; read_body(req, body, sizeof(body));
    device_pins_t p = *pins_get();   // start from current, override the fields that were sent
    form_u8(body, "i2c_sda", &p.i2c_sda);
    form_u8(body, "i2c_scl", &p.i2c_scl);
    form_u8(body, "nrf_tx",  &p.nrf_tx);
    form_u8(body, "nrf_rx",  &p.nrf_rx);
    form_u8(body, "nrf_hb",  &p.nrf_hb);
    form_u8(body, "ld_tx",   &p.ld_tx);
    form_u8(body, "ld_rx",   &p.ld_rx);
    bool ok = pins_save(&p);          // validates; rejects (saves nothing) on a bad GPIO
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

static void reg(httpd_handle_t s, const char *uri, httpd_method_t m, esp_err_t (*fn)(httpd_req_t*))
{
    httpd_uri_t u = { .uri = uri, .method = m, .handler = fn };
    httpd_register_uri_handler(s, &u);
}

void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    httpd_handle_t s = NULL;
    if (httpd_start(&s, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }
    reg(s, "/", HTTP_GET, h_index);
    reg(s, "/api/status", HTTP_GET, h_status);
    reg(s, "/api/scan", HTTP_GET, h_scan);
    reg(s, "/api/connect", HTTP_POST, h_connect);
    reg(s, "/api/press", HTTP_POST, h_press);
    reg(s, "/api/pair", HTTP_POST, h_pairing);
    reg(s, "/api/unpair", HTTP_POST, h_pairing);
    reg(s, "/api/mqtt", HTTP_POST, h_mqtt);
    reg(s, "/api/pins", HTTP_GET, h_pins);
    reg(s, "/api/pins", HTTP_POST, h_pins_save);
    reg(s, "/api/reboot", HTTP_POST, h_reboot);
    ESP_LOGI(TAG, "web server up on :80");
}
