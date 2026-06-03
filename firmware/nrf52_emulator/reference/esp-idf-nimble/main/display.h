#pragma once

// Local status display for the mock remote. Phase 2: no-op stub.
// Later: small I2C OLED (e.g. SSD1306) mirroring the real remote's LCD.
// I2C is intentionally NOT wired here yet (see memory mock-remote-hardware-plan).

void display_init(void);

// Show a short status line (last button sent, connection state, etc.).
// Stub logs nothing; the OLED implementation will render it later.
void display_status(const char *line);
