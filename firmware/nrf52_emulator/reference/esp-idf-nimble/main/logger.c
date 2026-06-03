#include "logger.h"

#include <stdarg.h>
#include <stdio.h>

#include "esp_timer.h"

uint32_t log_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

char *log_hex(char *out, size_t out_sz, const uint8_t *data, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    for (size_t i = 0; i < len && o + 2 < out_sz; i++) {
        out[o++] = hex[(data[i] >> 4) & 0xf];
        out[o++] = hex[data[i] & 0xf];
    }
    out[o] = '\0';
    return out;
}

void log_event(const char *type)
{
    printf("{\"type\":\"%s\",\"ts\":%u}\n", type, (unsigned)log_now_ms());
    fflush(stdout);
}

void log_json(const char *type, const char *body_fmt, ...)
{
    // Leading part: {"type":"...","ts":<ms>
    printf("{\"type\":\"%s\",\"ts\":%u", type, (unsigned)log_now_ms());

    if (body_fmt && body_fmt[0] != '\0') {
        putchar(',');
        va_list ap;
        va_start(ap, body_fmt);
        vprintf(body_fmt, ap);
        va_end(ap);
    }

    putchar('}');
    putchar('\n');
    fflush(stdout);
}
