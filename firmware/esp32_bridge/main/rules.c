#include "rules.h"
#include "ld2410.h"
#include "uart_link.h"
#include "mqtt_ha.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

static void eval_task(void *arg)
{
    bool prev = false;
    bool have_sample = false;   // false until the sensor has produced a valid frame
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();

        // A missing/dead LD2410 reads as present=false; that is NOT "empty room", so freeze
        // automation while the sensor is offline — otherwise absence rules would fire on boot
        // or on a wiring fault. The continuity timers restart once frames resume.
        if (!ld2410_alive()) {
            if (have_sample) mqtt_ha_presence_unavailable();  // unavailable, not a false "vacant"
            have_sample = false; prev = false;
            s_present_since_us = 0; s_absent_since_us = 0;
            continue;
        }

        bool present = ld2410_present();

        if (!have_sample) {
            // first valid sample (boot or sensor re-appeared): seed the anchor at *now*, not at
            // boot, and skip firing this tick so durations are measured from a known-good signal.
            if (present) s_present_since_us = now; else s_absent_since_us = now;
            mqtt_ha_publish_presence(present);
            have_sample = true; prev = present;
            continue;
        }

        if (present && !prev) { s_present_since_us = now; s_absent_since_us = 0;
                                mqtt_ha_publish_presence(true); }
        if (!present && prev) { s_absent_since_us = now; s_present_since_us = 0;
                                mqtt_ha_publish_presence(false); }
        prev = present;

        for (int i = 0; i < s_count; i++) {
            rule_t *r = &s_rules[i];
            if (!r->enabled) continue;

            int64_t held_us;
            if (r->cond == RULE_COND_PRESENCE) {
                if (!present) continue;
                held_us = now - s_present_since_us;
            } else {
                if (present) continue;
                held_us = now - s_absent_since_us;
            }
            if (held_us < (int64_t)r->duration_s * 1000000) continue;
            if (s_fire_us[i] && (now - s_fire_us[i]) < (int64_t)r->cooldown_s * 1000000) continue;

            if (uart_link_press(r->action)) {
                s_fire_us[i] = now;
                ESP_LOGI(TAG, "rule %d fired: press %s (%s held %llus)",
                         i, r->action, r->cond == RULE_COND_PRESENCE ? "presence" : "absence",
                         (unsigned long long)(held_us / 1000000));
            }
        }
    }
}

void rules_load(void)
{
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
    int n = s_count < max ? s_count : max;
    memcpy(out, s_rules, n * sizeof(rule_t));
    return n;
}

bool rules_set(const rule_t *in, int count)
{
    if (count < 0) count = 0;
    if (count > RULES_MAX) count = RULES_MAX;
    for (int i = 0; i < count; i++)
        if (in[i].action[0] && !uart_link_valid_btn(in[i].action)) {
            ESP_LOGW(TAG, "rejecting rules: '%s' is not a valid button", in[i].action);
            return false;
        }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_u8(h, NVS_VER_KEY, RULES_VER);
    nvs_set_blob(h, NVS_KEY, in, count * sizeof(rule_t));
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return false;

    memcpy(s_rules, in, count * sizeof(rule_t));
    s_count = count;
    memset(s_fire_us, 0, sizeof(s_fire_us));   // a fresh rule set starts with no cooldown debt
    ESP_LOGI(TAG, "%d rule(s) saved", count);
    return true;
}

bool rules_presence(void) { return ld2410_present(); }

uint32_t rules_presence_secs(void)
{
    if (!s_present_since_us) return 0;
    return (uint32_t)((esp_timer_get_time() - s_present_since_us) / 1000000);
}
