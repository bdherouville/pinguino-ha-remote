#include "wifi_mgr.h"
#include "led_status.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"

#define AP_SSID       "Ganymede-Bridge"
#define AP_CHANNEL    11           // clear of typical ch1/ch6 congestion
#define NVS_NS        "wificfg"
#define MAX_RETRY     8

static const char *TAG = "wifi";

static esp_netif_t *s_sta_netif, *s_ap_netif;
static volatile wm_state_t s_state = WM_BOOT;
static char s_ip[16]   = "";
static char s_ssid[33] = "";
static char s_pass[65] = "";
static int  s_retry    = 0;

static void set_state(wm_state_t st)
{
    s_state = st;
    switch (st) {
    case WM_BOOT:           led_status_set(LED_BOOT); break;
    case WM_AP:             led_status_set(LED_AP); break;
    case WM_STA_CONNECTING: led_status_set(LED_STA_CONNECTING); break;
    case WM_STA_CONNECTED:  led_status_set(LED_STA_CONNECTED); break;
    case WM_STA_FAILED:     led_status_set(LED_STA_FAILED); break;
    }
}

// ---- NVS creds ----
static bool creds_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = sizeof(s_ssid), pl = sizeof(s_pass);
    bool ok = (nvs_get_str(h, "ssid", s_ssid, &sl) == ESP_OK) && s_ssid[0];
    if (nvs_get_str(h, "pass", s_pass, &pl) != ESP_OK) s_pass[0] = 0;
    nvs_close(h);
    return ok;
}
static void creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
}

// ---- config helpers ----
static void apply_ap_config(void)
{
    wifi_config_t ap = {0};
    strlcpy((char*)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = AP_CHANNEL;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;   // open provisioning portal
    esp_wifi_set_config(WIFI_IF_AP, &ap);
}

// AP-only provisioning. (On ESP32-S3, plain AP beacons reliably; verified on-air. A faulty
// board may not radiate at all — that is hardware, not this code.)
static void raise_ap(void)
{
    esp_wifi_set_mode(WIFI_MODE_AP);
    apply_ap_config();
    set_state(WM_AP);
    ESP_LOGI(TAG, "provisioning AP up: %s (ch %d)", AP_SSID, AP_CHANNEL);
}

static void apply_sta_config(void)
{
    wifi_config_t sta = {0};
    strlcpy((char*)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid));
    strlcpy((char*)sta.sta.password, s_pass, sizeof(sta.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &sta);
}

// ---- deferred reboot (lets the HTTP response flush before re-init in STA mode) ----
static void reboot_task(void *arg) { vTaskDelay(pdMS_TO_TICKS(1200)); esp_restart(); }

// ---- events ----
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == WM_STA_CONNECTED) { set_state(WM_STA_CONNECTING); s_retry = 0; }
        if (s_state == WM_STA_CONNECTING && s_retry++ < MAX_RETRY) {
            esp_wifi_connect();
        } else if (s_state == WM_STA_CONNECTING) {
            ESP_LOGW(TAG, "connect failed after %d tries; raising provisioning AP", s_retry);
            set_state(WM_STA_FAILED);
            raise_ap();              // fall back to AP-only for re-provisioning
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        set_state(WM_STA_CONNECTED);
        ESP_LOGI(TAG, "got IP %s", s_ip);
    }
}

void wifi_mgr_init(void)
{
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    // Real country (the default "01" worldwide uses the lowest TX power; beacons inaudible).
    wifi_country_t country = { .cc = "FR", .schan = 1, .nchan = 13,
                               .policy = WIFI_COUNTRY_POLICY_MANUAL };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));

    bool have = creds_load();
    if (have) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        apply_sta_config();
    } else {
        raise_ap();                  // sets AP mode + config + WM_AP state
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    // SoftAP defaults to WIFI_PS_MIN_MODEM (suppresses beacons until a station associates) —
    // disable it + lift TX power so the AP is discoverable.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    esp_wifi_set_max_tx_power(84);

    if (have) {
        set_state(WM_STA_CONNECTING);
        esp_wifi_connect();
        ESP_LOGI(TAG, "connecting to '%s'", s_ssid);
    } else {
        ESP_LOGI(TAG, "no Wi-Fi creds — provisioning AP only");
    }
}

wm_state_t wifi_mgr_state(void) { return s_state; }
const char *wifi_mgr_ip(void)   { return s_ip; }
const char *wifi_mgr_ssid(void) { return s_ssid; }
bool wifi_mgr_has_creds(void)   { return s_ssid[0] != 0; }
const char *wifi_mgr_ap_ssid(void) { return AP_SSID; }

const char *wifi_mgr_state_str(void)
{
    switch (s_state) {
    case WM_BOOT: return "boot";
    case WM_AP: return "ap";
    case WM_STA_CONNECTING: return "connecting";
    case WM_STA_CONNECTED: return "connected";
    case WM_STA_FAILED: return "failed";
    }
    return "?";
}

int wifi_mgr_scan(wm_ap_t *out, int max)
{
    // Scanning needs the STA interface. In AP-only (provisioning), add STA momentarily.
    wifi_mode_t m; esp_wifi_get_mode(&m);
    bool restore_ap = (m == WIFI_MODE_AP);
    if (restore_ap) { esp_wifi_set_mode(WIFI_MODE_APSTA); vTaskDelay(pdMS_TO_TICKS(120)); }

    int cnt = 0;
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n) {
            wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
            if (recs) {
                esp_wifi_scan_get_ap_records(&n, recs);
                for (int i = 0; i < n && cnt < max; i++) {
                    if (recs[i].ssid[0] == 0) continue;
                    strlcpy(out[cnt].ssid, (char*)recs[i].ssid, sizeof(out[cnt].ssid));
                    out[cnt].rssi = recs[i].rssi;
                    out[cnt].authmode = recs[i].authmode;
                    cnt++;
                }
                free(recs);
            }
        }
    }
    if (restore_ap) esp_wifi_set_mode(WIFI_MODE_AP);
    return cnt;
}

bool wifi_mgr_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return false;
    creds_save(ssid, pass);          // persist; we reconnect cleanly in STA mode after reboot
    ESP_LOGI(TAG, "creds saved for '%s' — rebooting into STA", ssid);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return true;
}
