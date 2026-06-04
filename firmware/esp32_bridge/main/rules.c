#include "rules.h"
#include "ld2410.h"
#include "uart_link.h"
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
// Bump whenever rule_t's layout changes. A stored blob from a different version is ignored
// (rules reset to none) rather than misdecoded — there's no shipped installed base to migrate.
#define RULES_VER 2
static const char *TAG = "rules";

static rule_t  s_rules[RULES_MAX];
static int     s_count;
static int64_t s_fire_us[RULES_MAX];   // last fire per rule (0 = never)

// Continuous-state anchors (us). Exactly one is non-zero at a time.
static int64_t s_present_since_us;     // when the current presence began (0 if absent now)
static int64_t s_absent_since_us;      // when the current absence began  (0 if present now)

// Guards s_rules / s_count / s_fire_us / the anchors, which are touched by eval_task and by
// the web handlers (rules_get / rules_set / rules_presence_secs) on the httpd task. The
// ESP32-S3 is dual-core, so this is a real cross-core race, not just a preemption window.
// Blocking work (uart_link_press, MQTT publish) is always done OUTSIDE the lock.
static SemaphoreHandle_t s_lock;
#define LOCK()   do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_lock) xSemaphoreGive(s_lock); } while (0)

static void eval_task(void *arg)
{
    bool prev = false;
    bool have_sample = false;   // false until the sensor has produced a valid frame (this task only)
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        bool alive   = ld2410_alive();
        bool present = alive && ld2410_present();

        // Decide everything under the lock; defer the blocking work (MQTT + button presses)
        // until after we release it.
        int   pres_pub = -1;            // -1 none, 0 mark unavailable, 1 publish state
        bool  pres_state = false;
        char  to_press[RULES_MAX][RULE_ACTION_LEN];
        int   npress = 0;

        LOCK();
        if (!alive) {
            // A missing/dead LD2410 reads present=false; that is NOT "empty room", so freeze
            // automation while it's offline (otherwise absence rules fire on boot/wiring fault).
            if (have_sample) pres_pub = 0;
            have_sample = false; prev = false;
            s_present_since_us = 0; s_absent_since_us = 0;
        } else if (!have_sample) {
            // first valid sample (boot or sensor re-appeared): seed the anchor at *now* and skip
            // firing this tick so durations are measured from a known-good signal.
            if (present) s_present_since_us = now; else s_absent_since_us = now;
            pres_pub = 1; pres_state = present;
            have_sample = true; prev = present;
        } else {
            if (present && !prev) { s_present_since_us = now; s_absent_since_us = 0;
                                    pres_pub = 1; pres_state = true; }
            if (!present && prev) { s_absent_since_us = now; s_present_since_us = 0;
                                    pres_pub = 1; pres_state = false; }
            prev = present;

            for (int i = 0; i < s_count; i++) {
                rule_t *r = &s_rules[i];
                if (!r->enabled) continue;
                bool match    = (r->cond == RULE_COND_PRESENCE) ? present : !present;
                int64_t anchor = (r->cond == RULE_COND_PRESENCE) ? s_present_since_us
                                                                 : s_absent_since_us;
                if (!match || anchor == 0) continue;   // anchor==0 guard: never use a stale timer
                if (now - anchor < (int64_t)r->duration_s * 1000000) continue;
                if (s_fire_us[i] && (now - s_fire_us[i]) < (int64_t)r->cooldown_s * 1000000) continue;
                s_fire_us[i] = now;                    // stamp now; the press happens after unlock
                strlcpy(to_press[npress++], r->action, RULE_ACTION_LEN);
            }
        }
        UNLOCK();

        // ---- blocking work, outside the lock ----
        if (pres_pub == 0)      mqtt_ha_presence_unavailable();
        else if (pres_pub == 1) mqtt_ha_publish_presence(pres_state);
        for (int i = 0; i < npress; i++)
            if (uart_link_press(to_press[i]))
                ESP_LOGI(TAG, "rule fired: press %s", to_press[i]);
    }
}

void rules_load(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t ver = 0;
        size_t sz = sizeof(s_rules);
        // Only trust the blob if it was written by this rule_t layout and is an exact multiple
        // of the struct size — otherwise ignore it (degrade to 0 rules, never misdecode).
        if (nvs_get_u8(h, NVS_VER_KEY, &ver) == ESP_OK && ver == RULES_VER &&
            nvs_get_blob(h, NVS_KEY, s_rules, &sz) == ESP_OK && (sz % sizeof(rule_t)) == 0)
            s_count = (int)(sz / sizeof(rule_t));
        nvs_close(h);
    }
    if (s_count < 0 || s_count > RULES_MAX) s_count = 0;
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

    // Validate + normalise into a local copy first; never mutate the caller's buffer.
    rule_t v[RULES_MAX];
    for (int i = 0; i < count; i++) {
        v[i] = in[i];
        if (v[i].enabled && !uart_link_valid_btn(v[i].action)) {
            ESP_LOGW(TAG, "rejecting rules: rule %d has an invalid/empty action", i);
            return false;
        }
        if (v[i].enabled && v[i].cooldown_s < RULE_MIN_COOLDOWN_S)
            v[i].cooldown_s = RULE_MIN_COOLDOWN_S;   // anti-spam floor
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    // Bail on the first failing write so we never report success (or update RAM) for a save
    // that won't survive a reboot — e.g. a full NVS partition.
    esp_err_t e = nvs_set_u8(h, NVS_VER_KEY, RULES_VER);
    if (e == ESP_OK) e = nvs_set_blob(h, NVS_KEY, v, count * sizeof(rule_t));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "rules NVS save failed: %s", esp_err_to_name(e));
        return false;
    }

    LOCK();
    memcpy(s_rules, v, count * sizeof(rule_t));
    s_count = count;
    memset(s_fire_us, 0, sizeof(s_fire_us));   // a fresh rule set starts with no cooldown debt
    UNLOCK();
    ESP_LOGI(TAG, "%d rule(s) saved", count);
    return true;
}

bool rules_presence(void) { return ld2410_present(); }

uint32_t rules_presence_secs(void)
{
    LOCK();
    int64_t anchor = s_present_since_us;
    UNLOCK();
    if (!anchor) return 0;
    return (uint32_t)((esp_timer_get_time() - anchor) / 1000000);
}
