#include "ac_cmd.h"
#include "ac_state.h"
#include "ble_emu.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "accmd";

// The AC's capacitive touch buttons debounce ~1.5 s: presses closer than that are dropped, which
// silently desyncs an open-loop multi-press sequence (e.g. fan->dry needs two "mode" presses).
// Pace to BLE_EMU_PRESS_GAP_MS, comfortably above the debounce.
#define MAX_STEPS    40       // hard cap on any single sequence (temp can need many)

typedef enum { REQ_MODE, REQ_TEMP, REQ_FAN, REQ_SW } req_kind_t;
typedef struct {
    req_kind_t kind;
    int  ival;            // MODE: ac_mode_t | TEMP: target | FAN: ac_fan_t | SW: on(0/1)
    bool on;              // MODE: want_on
    char name[8];         // SW: which ("swing"/"eco"/"silent")
} ac_req_t;

static QueueHandle_t s_q;

static void press(const char *b)
{
    // Wait until a full gap has elapsed since the last relayed press — which includes a web tap
    // that interleaved between worker presses (ble_emu stamps every relayed press) — then fire.
    // Pacing *before* the press off the shared timestamp keeps the AC-registered gap correct; a
    // fixed delay-after would let the worker fire just behind such a tap and be coalesced away.
    // If a tap still races in during the wait, ble_emu_press() coalesces ours (returns false);
    // re-wait and retry so the sequence doesn't silently drop a step. Bounded so non-stop
    // interleaving taps can't hang the worker.
    for (int attempt = 0; attempt < 10; attempt++) {
        if (!ble_emu_will_model()) return;  // link dropped / not commandable — stop; do_* re-checks
        int64_t wait_us = (int64_t)BLE_EMU_PRESS_GAP_MS * 1000 - ble_emu_since_press_us();
        if (wait_us > 0) vTaskDelay(pdMS_TO_TICKS((TickType_t)(wait_us / 1000) + 1));
        if (ble_emu_press(b)) return;       // relayed + folded into ac_state
    }
}

// A press only updates the model when the relay is ready (or we're syncing). If neither, the AC
// is unreachable — bail rather than spin pressing a model that won't move.
static void do_mode(ac_mode_t target, bool want_on)
{
    if (!ble_emu_will_model()) return;
    ac_state_t st; ac_state_get_copy(&st);
    if (!want_on) { if (st.on) press("power"); return; }
    if (!st.on) press("power");               // wake (resumes last mode)
    for (int i = 0; i < 3; i++) {
        if (!ble_emu_will_model()) return;
        ac_state_get_copy(&st);
        if (st.mode == target) break;
        press("mode");
    }
}

static void do_temp(int target)
{
    for (int i = 0; i < MAX_STEPS; i++) {
        if (!ble_emu_will_model()) return;
        ac_state_t st; ac_state_get_copy(&st);
        if (!st.on || st.mode != AC_MODE_COOL) break;   // setpoint is COOL-only
        if (st.temp_c == target) break;
        press(st.temp_c < target ? "up" : "down");
    }
}

static void do_fan(ac_fan_t target)
{
    for (int i = 0; i < 5; i++) {
        if (!ble_emu_will_model()) return;
        ac_state_t st; ac_state_get_copy(&st);
        if (!st.on || st.mode == AC_MODE_DRY) break;             // not settable in dry
        if (target == AC_FAN_AUTO && st.mode != AC_MODE_COOL) break; // auto = cool only
        if (st.fan == target) break;
        press("fan");
    }
}

static void do_switch(const char *which, bool on)
{
    if (!ble_emu_will_model()) return;
    ac_state_t st; ac_state_get_copy(&st);
    if (!st.on) return;
    bool eco_sil = (!strcmp(which, "eco") || !strcmp(which, "silent"));
    if (eco_sil && st.mode != AC_MODE_COOL) return;             // COOL-only
    bool cur = !strcmp(which, "swing") ? st.swing
             : !strcmp(which, "eco")   ? st.eco : st.silent;
    if (cur != on) press(!strcmp(which, "swing") ? "flap" : which);
}

static void worker(void *arg)
{
    ac_req_t r;
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) continue;
        switch (r.kind) {
            case REQ_MODE: do_mode((ac_mode_t)r.ival, r.on); break;
            case REQ_TEMP: do_temp(r.ival);                  break;
            case REQ_FAN:  do_fan((ac_fan_t)r.ival);         break;
            case REQ_SW:   do_switch(r.name, r.ival != 0);   break;
        }
    }
}

void ac_cmd_init(void)
{
    s_q = xQueueCreate(8, sizeof(ac_req_t));
    xTaskCreate(worker, "ac_cmd", 3072, NULL, 4, NULL);
}

static void enqueue(const ac_req_t *r) { if (s_q) xQueueSend(s_q, r, 0); }

void ac_cmd_set_mode_ha(const char *ha_mode)
{
    ac_mode_t m; bool on;
    if (!ac_mode_from_ha(ha_mode, &m, &on)) { ESP_LOGW(TAG, "bad mode '%s'", ha_mode); return; }
    ac_req_t r = { .kind = REQ_MODE, .ival = (int)m, .on = on };
    enqueue(&r);
}

void ac_cmd_set_temp(int target_c)
{
    if (target_c < AC_TEMP_MIN) target_c = AC_TEMP_MIN;
    if (target_c > AC_TEMP_MAX) target_c = AC_TEMP_MAX;
    ac_req_t r = { .kind = REQ_TEMP, .ival = target_c };
    enqueue(&r);
}

void ac_cmd_set_fan(const char *fan_str)
{
    ac_fan_t f;
    if (!ac_fan_from_str(fan_str, &f)) { ESP_LOGW(TAG, "bad fan '%s'", fan_str); return; }
    ac_req_t r = { .kind = REQ_FAN, .ival = (int)f };
    enqueue(&r);
}

void ac_cmd_set_switch(const char *which, bool on)
{
    ac_req_t r = { .kind = REQ_SW, .ival = on ? 1 : 0 };
    strlcpy(r.name, which, sizeof(r.name));
    enqueue(&r);
}
