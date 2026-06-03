#pragma once

#include <stddef.h>
#include <stdint.h>

// Line-delimited JSON logger over the USB serial console (SPECS.md §9).
// Every event is emitted as one self-contained JSON object on its own line, e.g.
//   {"type":"connect","ts":2345,"addr":"00:A0:50:XX:XX:XX","status":0}
// JSON is the default machine-readable output; plain ESP_LOG lines may coexist for humans.

// Emit one JSON line. Caller supplies the object body WITHOUT the surrounding braces;
// the logger wraps it as {"type":"<type>","ts":<ms>,<body>} and appends a newline.
// Pass body="" for events that carry only a type. `body` is a printf-style format.
void log_json(const char *type, const char *body_fmt, ...) __attribute__((format(printf, 2, 3)));

// Emit a body-less JSON line: {"type":"<type>","ts":<ms>}.
void log_event(const char *type);

// Format a byte buffer as a lowercase hex string (no separators) into `out`.
// Returns `out`. Truncates safely if the buffer would overflow `out_sz`.
char *log_hex(char *out, size_t out_sz, const uint8_t *data, size_t len);

// Milliseconds since boot (esp_timer based), used as the "ts" field.
uint32_t log_now_ms(void);
