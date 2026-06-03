#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WM_BOOT, WM_AP, WM_STA_CONNECTING, WM_STA_CONNECTED, WM_STA_FAILED
} wm_state_t;

typedef struct { char ssid[33]; int8_t rssi; uint8_t authmode; } wm_ap_t;

void        wifi_mgr_init(void);
wm_state_t  wifi_mgr_state(void);
const char *wifi_mgr_state_str(void);
const char *wifi_mgr_ip(void);       // "x.x.x.x" once connected, else ""
const char *wifi_mgr_ssid(void);     // current/target SSID
bool        wifi_mgr_has_creds(void);
const char *wifi_mgr_ap_ssid(void);  // the provisioning AP name

int  wifi_mgr_scan(wm_ap_t *out, int max);          // blocking scan; returns count
bool wifi_mgr_connect(const char *ssid, const char *pass); // save creds + (re)connect STA
