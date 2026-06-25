#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *name;
    const char *label;
} bridge_button_def_t;

#define BRIDGE_BUTTON_POWER "power"
#define BRIDGE_BUTTON_UP "up"
#define BRIDGE_BUTTON_DOWN "down"
#define BRIDGE_BUTTON_MODE "mode"
#define BRIDGE_BUTTON_FAN "fan"
#define BRIDGE_BUTTON_SILENT "silent"
#define BRIDGE_BUTTON_ECO "eco"
#define BRIDGE_BUTTON_TIMER "timer"
#define BRIDGE_BUTTON_FLAP "flap"

const bridge_button_def_t *bridge_buttons(size_t *count);
bool bridge_button_valid(const char *name);
