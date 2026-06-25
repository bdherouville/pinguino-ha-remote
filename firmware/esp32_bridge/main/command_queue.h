#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

void bridge_command_queue_init(void);
esp_err_t bridge_press_button(const char *button_name);
esp_err_t bridge_press_button_wait(const char *button_name, bool *sent);
bool bridge_last_button(char *out, size_t outlen, uint64_t *timestamp_ms, bool *sent);
