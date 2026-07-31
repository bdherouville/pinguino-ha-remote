#include "rules.h"
#include "ld2410.h"
#include "ac_state.h"
#include "ac_cmd.h"
#include "ble_emu.h"
#include "mqtt_ha.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"

#define NVS_NS  "rules"
#define NVS_KEY "list"
#define NVS_VER_KEY "ver"
#define RULES_VER 3          // bumped: action+cooldown -> target, debounced edge-trigger

// Debounce the LD2410 presence: the raw signal must hold its new value this long before the
// debounced state flips, so a momentarily-lost still target doesn't reset the sustained timer.
#define DEBOUNCE_S  10
#define DEBOUNCE_US ((int64_t)DEBOUNCE_S * 1000000)

static const char *TAG = "rules";

static rule_t  s_rules[RULES_MAX];
static int     s_count;
static bool    s_armed[RULES_MAX];     // edge-trigger: a rule re-arms when its condition ends
static SemaphoreHandle_t s_lock;
#define LOCK()   do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_lock) xSemaphoreGive(s_lock); } while (0)

// Debounced presence + continuity anchors (exactly one anchor non-zero while a sample is held).
static volatile bool s_present;
static int64_t s_present_since_us;
static int64_t s_absent_since_us;

static bool   valid_target(const char *t) { ac_mode_t m; bool on; return ac_mode_from_ha(t, &m, &on); }

static void eval_task(void *arg)
{
    bool have = false;                 // have a valid (alive) sample yet (this task only)
    int64_t pending_since = 0;         // when the raw signal started differing from debounced

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        bool alive = ld2410_alive();
        bool raw   = alive && ld2410_present();

        // All shared state (s_present, the anchors, s_armed, s_rules) is touched under the lock;
        // the blocking work (MQTT publish, ac_cmd) is deferred until after we release it.
        int  pres_pub = -1;            // -1 none, 0 mark unavailable, 1 publish `raw`
        char to_apply[RULES_MAX][RULE_TARGET_LEN];
        int  napply = 0;
        bool ready = ble_emu_ready();   // only fire (and disarm) when the AC is reachable

        LOCK();
        if (!alive) {                  // sensor offline: freeze automation, re-arm everything
            if (have) pres_pub = 0;
            have = false; pending_since = 0;
            s_present = false; s_present_since_us = 0; s_absent_since_us = 0;
            for (int i = 0; i < s_count; i++) s_armed[i] = true;
        } else {
            if (!have) {               // first sample after boot/recovery: adopt immediately
                s_present = raw; pending_since = 0;
                if (raw) { s_present_since_us = now; s_absent_since_us = 0; }
                else     { s_absent_since_us = now; s_present_since_us = 0; }
                have = true; pres_pub = 1;
                for (int i = 0; i < s_count; i++) s_armed[i] = true;
            } else if (raw == s_present) {
                pending_since = 0;     // stable
            } else {
                if (pending_since == 0) pending_since = now;
                if (now - pending_since >= DEBOUNCE_US) {     // accept the debounced change
                    int64_t began = pending_since;
                    s_present = raw; pending_since = 0;
                    if (raw) { s_present_since_us = began; s_absent_since_us = 0; }
                    else     { s_absent_since_us = began; s_present_since_us = 0; }
                    pres_pub = 1;
                }
            }
            for (int i = 0; i < s_count; i++) {
                if (!s_rules[i].enabled) continue;
                bool active = (s_rules[i].cond == RULE_COND_PRESENCE) ? s_present : !s_present;
                if (!active) { s_armed[i] = true; continue; }  // re-arm when condition ends
                int64_t anchor = (s_rules[i].cond == RULE_COND_PRESENCE) ? s_present_since_us
                                                                         : s_absent_since_us;
                if (anchor == 0 || !s_armed[i]) continue;
                if (now - anchor < (int64_t)s_rules[i].duration_s * 1000000) continue;
                if (!ready) continue;                          // AC unreachable: stay armed, fire later
                s_armed[i] = false;                            // edge-trigger: fire once
                strlcpy(to_apply[napply++], s_rules[i].target, RULE_TARGET_LEN);
            }
        }
        UNLOCK();

        if (pres_pub == 0)      mqtt_ha_presence_unavailable();
        else if (pres_pub == 1) mqtt_ha_publish_presence(raw);
        for (int i = 0; i < napply; i++) {
            ac_cmd_set_mode_ha(to_apply[i]);                   // idempotent: no-op if already there
            ESP_LOGI(TAG, "rule fired -> AC target %s", to_apply[i]);
        }
    }
}

void rules_load(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t ver = 0; size_t sz = sizeof(s_rules);
        if (nvs_get_u8(h, NVS_VER_KEY, &ver) == ESP_OK && ver == RULES_VER &&
            nvs_get_blob(h, NVS_KEY, s_rules, &sz) == ESP_OK && (sz % sizeof(rule_t)) == 0)
            s_count = (int)(sz / sizeof(rule_t));
        nvs_close(h);
    }
    if (s_count < 0 || s_count > RULES_MAX) s_count = 0;
    for (int i = 0; i < RULES_MAX; i++) s_armed[i] = true;
    ESP_LOGI(TAG, "%d rule(s) loaded", s_count);
    xTaskCreate(eval_task, "rules", 3072, NULL, 4, NULL);
}

int rules_get(rule_t *out, int max)
{
    LOCK();
    int n = s_count < max ? s_count : max;
    memcpy(out, s_rules, n * sizeof(rule_t));
    UNLOCK();
    return n;
}

bool rules_set(const rule_t *in, int count)
{
    if (count < 0) count = 0;
    if (count > RULES_MAX) count = RULES_MAX;
    // Validate EVERY rule's target (an enabled rule must name a mode; a disabled one may be empty
    // but never arbitrary text — it is echoed verbatim into the /api/rules JSON).
    for (int i = 0; i < count; i++) {
        bool ok = valid_target(in[i].target);   // false for "" too
        if (in[i].enabled ? !ok : (in[i].target[0] && !ok)) {
            ESP_LOGW(TAG, "rejecting rules: rule %d target '%s' is not a valid AC mode", i, in[i].target);
            return false;
        }
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_u8(h, NVS_VER_KEY, RULES_VER);
    if (e == ESP_OK) e = nvs_set_blob(h, NVS_KEY, in, count * sizeof(rule_t));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) { ESP_LOGW(TAG, "rules NVS save failed: %s", esp_err_to_name(e)); return false; }

    LOCK();
    memcpy(s_rules, in, count * sizeof(rule_t));
    s_count = count;
    for (int i = 0; i < RULES_MAX; i++) s_armed[i] = true;   // fresh set: arm all
    UNLOCK();
    ESP_LOGI(TAG, "%d rule(s) saved", count);
    return true;
}

bool rules_presence(void) { return s_present; }

uint32_t rules_presence_secs(void)
{
    LOCK();
    int64_t anchor = s_present_since_us;
    UNLOCK();
    if (!anchor) return 0;
    return (uint32_t)((esp_timer_get_time() - anchor) / 1000000);
}
