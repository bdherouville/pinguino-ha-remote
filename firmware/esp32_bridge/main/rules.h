#pragma once
#include <stdbool.h>
#include <stdint.h>

// Simple presence-driven automation. Each rule watches the LD2410 presence signal: when
// presence (or absence) has held continuously for `duration_s`, it presses an AC button,
// then waits `cooldown_s` before it can fire again. Rules persist in NVS (namespace "rules").
#define RULES_MAX        6
#define RULE_ACTION_LEN  12

enum { RULE_COND_PRESENCE = 0, RULE_COND_ABSENCE = 1 };

// Upper bound for durations/cooldowns (7 days) — wide enough for any real rule, and keeps
// duration_s * 1e6 well within int64 in the evaluator.
#define RULE_MAX_SECS  (7u * 24 * 3600)

typedef struct {
    bool     enabled;
    uint8_t  cond;                     // RULE_COND_PRESENCE | RULE_COND_ABSENCE
    uint32_t duration_s;               // condition must hold this long before firing
    uint32_t cooldown_s;               // minimum gap between fires
    char     action[RULE_ACTION_LEN];  // button name (validated against uart_link)
} rule_t;

void rules_load(void);                 // load from NVS and start the evaluator task
int  rules_get(rule_t *out, int max);  // copy current rules into out; returns the count
bool rules_set(const rule_t *in, int count); // validate, persist, replace (resets cooldowns)

// Live status for the UI.
bool     rules_presence(void);
uint32_t rules_presence_secs(void);    // seconds presence has held (0 if currently absent)
