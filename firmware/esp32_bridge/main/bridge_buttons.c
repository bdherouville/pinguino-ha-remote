#include "bridge_buttons.h"
#include <string.h>

static const bridge_button_def_t BUTTONS[] = {
    {BRIDGE_BUTTON_POWER, "Power"},
    {BRIDGE_BUTTON_UP, "Temp Up"},
    {BRIDGE_BUTTON_DOWN, "Temp Down"},
    {BRIDGE_BUTTON_MODE, "Mode"},
    {BRIDGE_BUTTON_FAN, "Fan"},
    {BRIDGE_BUTTON_SILENT, "Silent"},
    {BRIDGE_BUTTON_ECO, "Eco"},
    {BRIDGE_BUTTON_TIMER, "Timer"},
    {BRIDGE_BUTTON_FLAP, "Flap"},
};

const bridge_button_def_t *bridge_buttons(size_t *count)
{
    if (count) *count = sizeof(BUTTONS) / sizeof(BUTTONS[0]);
    return BUTTONS;
}

bool bridge_button_valid(const char *name)
{
    if (!name) return false;
    size_t count = 0;
    const bridge_button_def_t *buttons = bridge_buttons(&count);
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(name, buttons[i].name)) return true;
    }
    return false;
}
