/*
 * Log of the node for the gateway: hook into the ESP-IDF log.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "node_log.h"

#define LOG_LINE_MAX        96          // Level, time, tag and the text up to what is sent

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_console;        // The previous output: the console of a debug build

// Called by the log of any task, on its stack
static int log_vprintf(const char *fmt, va_list args)
{
    char line[LOG_LINE_MAX];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) {
        taskENTER_CRITICAL(&s_lock);
        node_log_put(line, ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1);
        taskEXIT_CRITICAL(&s_lock);
    }
#if CONFIG_ESP_CONSOLE_NONE
    return n;
#else
    return s_console(fmt, args);
#endif
}

void node_log_init(int min_level)
{
    node_log_reset(min_level);
    if (min_level < NODE_LOG_NONE) {
        s_console = esp_log_set_vprintf(log_vprintf);
    }
}

#if !CONFIG_ESP_CONSOLE_NONE
static void console(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    if (s_console != NULL) {
        s_console(fmt, args);           // Not through the hook: the line is queued already
    } else {
        vprintf(fmt, args);
    }
    va_end(args);
}
#endif

void node_log_add(int level, const char *text, size_t len)
{
    taskENTER_CRITICAL(&s_lock);
    node_log_put_text(level, text, len);
    taskEXIT_CRITICAL(&s_lock);
#if !CONFIG_ESP_CONSOLE_NONE
    console("%c (%" PRIu32 ") prg: %.*s\n", "DIWE"[level & 3], esp_log_timestamp(), (int)len, text);
#endif
}

bool node_log_take(node_log_line_t *line)
{
    taskENTER_CRITICAL(&s_lock);
    bool ok = node_log_get(line);
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}
