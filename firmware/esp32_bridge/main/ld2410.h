#pragma once
#include <stdbool.h>
#include <stdint.h>

// Hi-Link LD2410 24 GHz presence radar on UART1 (pins from the runtime config, see pins.h).
// The module free-runs in its default "target data" reporting mode at 256000 8N1; a task
// parses the report frames and caches the current target state. No configuration is sent —
// we only listen.
void ld2410_init(void);

bool ld2410_present(void);   // a moving or static target is currently detected
bool ld2410_alive(void);     // valid frames are arriving (i.e. the sensor is wired up)

// Last parsed values. Returns false if no frame has been seen yet. Distances in cm.
bool ld2410_get(uint8_t *state, uint16_t *moving_cm, uint16_t *static_cm);
