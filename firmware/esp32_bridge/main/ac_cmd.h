#pragma once
#include <stdbool.h>

// Translates a desired Home-Assistant target into the sequence of button presses needed to
// reach it from the current modeled state, paced so the AC registers each press. Runs on a
// dedicated worker task so MQTT/HTTP callers never block. Each issued press goes through the
// normal dispatch (notified to the AC AND folded into ac_state).
void ac_cmd_init(void);

void ac_cmd_set_mode_ha(const char *ha_mode);          // "off"/"cool"/"dry"/"fan_only"
void ac_cmd_set_temp(int target_c);                    // cooling setpoint
void ac_cmd_set_fan(const char *fan_str);              // "min"/"medium"/"max"/"auto"
void ac_cmd_set_switch(const char *which, bool on);    // "swing"/"eco"/"silent"
