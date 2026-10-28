/*
 * PLC output streams.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mqttsn_defs.h"
#include "plc_io.h"
#include "plc_io_hw.h"
#include "plc_stream.h"

#define LINE_MAX_LEN    (MQTTSN_MAX_MSG_LEN - MQTTSN_SIZEOF_PUBLISH)    // A line is one PUBLISH
#define NUM_MAX         64              // FLT_MAX with 8 digits after the point and the sign: 49

static struct {
    plc_stream_log_t log;
    struct {
        uint8_t len;
        char    text[LINE_MAX_LEN];
    } line[PLC_STREAM_LOG_LEVELS];
    struct {
        uint16_t head;                  // Oldest byte
        uint16_t count;
        uint8_t  buf[PLC_STREAM_UART_SIZE];
    } uart[PLC_HW_UART_PORTS];
} s;

static bool is_log(uint32_t stream)
{
    return (stream >= PLC_STREAM_LOG) && (stream < PLC_STREAM_LOG + PLC_STREAM_LOG_LEVELS);
}

static void end_line(int level)
{
    if (s.line[level].len > 0) {
        if (s.log != NULL) {
            s.log(level, s.line[level].text, s.line[level].len);
        }
        s.line[level].len = 0;
    }
}

// The bytes whole or nothing
static plc_status_t put(uint32_t stream, const char *data, size_t len, uint32_t *ok)
{
    if (is_log(stream)) {
        int level = (int)(stream - PLC_STREAM_LOG);
        *ok = (s.line[level].len + len <= LINE_MAX_LEN);
        if (*ok) {
            memcpy(&s.line[level].text[s.line[level].len], data, len);
            s.line[level].len += (uint8_t)len;
        }
        return PLC_RUN;
    }

    if (!plc_io_uart_ready(stream)) {
        return PLC_ERR_PARM;
    }
    *ok = (s.uart[stream].count + len <= PLC_STREAM_UART_SIZE);
    if (*ok) {
        for (size_t i = 0; i < len; i++) {
            s.uart[stream].buf[(s.uart[stream].head + s.uart[stream].count++) % PLC_STREAM_UART_SIZE] = (uint8_t)data[i];
        }
    }
    return PLC_RUN;
}

void plc_stream_init(plc_stream_log_t log)
{
    memset(&s, 0, sizeof(s));
    s.log = log;
}

void plc_stream_reset(void)
{
    plc_stream_log_t log = s.log;
    plc_stream_init(log);
}

plc_status_t plc_stream_byte(uint32_t stream, uint32_t value, uint32_t *ok)
{
    if (is_log(stream) && ((value & 0xFF) == 0)) {
        end_line((int)(stream - PLC_STREAM_LOG));
        *ok = 1;
        return PLC_RUN;
    }
    char byte = (char)value;
    return put(stream, &byte, 1, ok);
}

plc_status_t plc_stream_int(uint32_t stream, uint32_t value, uint32_t *ok)
{
    char buf[NUM_MAX];
    int n = snprintf(buf, sizeof(buf), "%ld", (long)(int32_t)value);
    return put(stream, buf, (size_t)n, ok);
}

plc_status_t plc_stream_float(uint32_t stream, uint32_t format, uint32_t value, uint32_t *ok)
{
    if (format > 299) {
        return PLC_ERR_PARM;
    }
    // Not set is the 6 digits of %f: a negative precision is "not set" in C, but not to every libc
    int prec = (format % 10 == 0) ? 6 : (int)(format % 10) - 1;
    int width = (int)(format / 10 % 10);
    uint32_t pad = format / 100;

    float f;
    memcpy(&f, &value, sizeof(f));
    char buf[NUM_MAX];
    int n;
    if (isnan(f) || isinf(f)) {         // As printf does, the same on every libc ("-nan(ind)" on Windows)
        const char *txt = isnan(f) ? "nan" : (f < 0) ? "-inf" : "inf";
        n = (pad == 2) ? snprintf(buf, sizeof(buf), "%-*s", width, txt) : snprintf(buf, sizeof(buf), "%*s", width, txt);
    } else if (pad == 0) {
        n = snprintf(buf, sizeof(buf), "%0*.*f", width, prec, (double)f);
    } else if (pad == 1) {
        n = snprintf(buf, sizeof(buf), "%*.*f", width, prec, (double)f);
    } else {
        n = snprintf(buf, sizeof(buf), "%-*.*f", width, prec, (double)f);
    }
    return put(stream, buf, (size_t)n, ok);
}

void plc_stream_flush(void)
{
    for (int level = 0; level < PLC_STREAM_LOG_LEVELS; level++) {
        end_line(level);
    }

    for (int port = 0; port < PLC_HW_UART_PORTS; port++) {
        uint16_t *head = &s.uart[port].head;
        uint16_t *count = &s.uart[port].count;
        if (*count == 0) {
            continue;
        }
        if (!plc_io_uart_ready((uint32_t)port)) {
            *head = 0;                  // The port was closed
            *count = 0;
            continue;
        }
        while (*count > 0) {            // Up to two pieces of the ring
            size_t piece = PLC_STREAM_UART_SIZE - *head;
            if (piece > *count) {
                piece = *count;
            }
            size_t n = plc_io_uart_write((uint32_t)port, &s.uart[port].buf[*head], piece);
            *head = (uint16_t)((*head + n) % PLC_STREAM_UART_SIZE);
            *count = (uint16_t)(*count - n);
            if (n < piece) {
                break;                  // FIFO full
            }
        }
    }
}
