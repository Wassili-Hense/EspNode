/*
 * PLC output streams.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mqttsn_defs.h"
#include "plc_io.h"
#include "plc_io_hw.h"
#include "plc_stream.h"

#define LINE_MAX_LEN    (MQTTSN_MAX_MSG_LEN - MQTTSN_SIZEOF_PUBLISH)    // A line is one PUBLISH
#define OUT_MAX         PLC_STREAM_UART_SIZE    // Output of PrintF: longer fits no stream
#define NUM_MAX         64              // %f of FLT_MAX with 8 digits after the point and the sign: 49

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
static plc_status_t put(uint32_t stream, const uint8_t *data, size_t len, uint32_t *ok)
{
    if (is_log(stream)) {
        int level = (int)(stream - PLC_STREAM_LOG);
        // Every line the bytes continue or end must fit
        size_t line = s.line[level].len;
        for (size_t i = 0; i < len; i++) {
            line = (data[i] == 0) ? 0 : line + 1;
            if (line > LINE_MAX_LEN) {
                *ok = 0;
                return PLC_RUN;
            }
        }
        for (size_t i = 0; i < len; i++) {
            if (data[i] == 0) {
                end_line(level);
            } else {
                s.line[level].text[s.line[level].len++] = (char)data[i];
            }
        }
        *ok = 1;
        return PLC_RUN;
    }

    if (!plc_io_uart_ready(stream)) {
        return PLC_ERR_PARM;
    }
    *ok = (s.uart[stream].count + len <= PLC_STREAM_UART_SIZE);
    if (*ok) {
        for (size_t i = 0; i < len; i++) {
            s.uart[stream].buf[(s.uart[stream].head + s.uart[stream].count++) % PLC_STREAM_UART_SIZE] = data[i];
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
    uint8_t byte = (uint8_t)value;
    return put(stream, &byte, 1, ok);
}

////////////////////////////////////////////////////////////////////////
// PrintF

typedef struct {
    uint8_t buf[OUT_MAX];
    size_t  len;                        // All of the output, kept up to OUT_MAX
} out_t;

static void emit(out_t *o, const void *data, size_t len)
{
    if (o->len < OUT_MAX) {
        memcpy(&o->buf[o->len], data, (len < OUT_MAX - o->len) ? len : OUT_MAX - o->len);
    }
    o->len += len;
}

// flag: 0, '0' - zeros, '-' - spaces on the right; width 0..9; prec: -1 not set, 0..8
static void convert(out_t *o, char kind, char flag, int width, int prec, uint32_t v)
{
    char buf[NUM_MAX];
    int n;
    long d = (long)(int32_t)v;
    unsigned long x = v;

    switch (kind) {
    case 'c':
        buf[0] = (char)(uint8_t)v;
        n = 1;
        break;
    case 'd':
        n = (flag == '0') ? snprintf(buf, sizeof(buf), "%0*ld", width, d) :
            (flag == '-') ? snprintf(buf, sizeof(buf), "%-*ld", width, d) : snprintf(buf, sizeof(buf), "%*ld", width, d);
        break;
    case 'x':
        n = (flag == '0') ? snprintf(buf, sizeof(buf), "%0*lx", width, x) :
            (flag == '-') ? snprintf(buf, sizeof(buf), "%-*lx", width, x) : snprintf(buf, sizeof(buf), "%*lx", width, x);
        break;
    case 'X':
        n = (flag == '0') ? snprintf(buf, sizeof(buf), "%0*lX", width, x) :
            (flag == '-') ? snprintf(buf, sizeof(buf), "%-*lX", width, x) : snprintf(buf, sizeof(buf), "%*lX", width, x);
        break;
    default: {                          // 'f'
        float f;
        memcpy(&f, &v, sizeof(f));
        if (isnan(f) || isinf(f)) {     // Padded with spaces as printf does, the same on every libc ("-nan(ind)")
            const char *txt = isnan(f) ? "nan" : (f < 0) ? "-inf" : "inf";
            n = (flag == '-') ? snprintf(buf, sizeof(buf), "%-*s", width, txt) : snprintf(buf, sizeof(buf), "%*s", width, txt);
            break;
        }
        // Not set is the 6 digits of %f: a negative precision is "not set" in C, but not to every libc
        double g = f;
        prec = (prec < 0) ? 6 : prec;
        n = (flag == '0') ? snprintf(buf, sizeof(buf), "%0*.*f", width, prec, g) :
            (flag == '-') ? snprintf(buf, sizeof(buf), "%-*.*f", width, prec, g) :
            snprintf(buf, sizeof(buf), "%*.*f", width, prec, g);
        break;
    }
    }
    emit(o, buf, (size_t)n);
}

// The same rules as EP_PrintF.Arguments() of the compiler: a program from elsewhere stops instead of printing
plc_status_t plc_stream_printf(uint32_t stream, const uint8_t *fmt, size_t len, uint32_t n, plc_stream_arg_t arg,
                               uint32_t *ok)
{
    if (!is_log(stream) && !plc_io_uart_ready(stream)) {
        return PLC_ERR_PARM;
    }

    out_t o;
    o.len = 0;
    uint32_t used = 0;
    for (size_t i = 0; i < len;) {
        uint8_t c = fmt[i++];
        if (c != '%') {
            emit(&o, &c, 1);
            continue;
        }
        if ((i < len) && (fmt[i] == '%')) {
            i++;
            emit(&o, &c, 1);
            continue;
        }
        char flag = 0;
        if ((i < len) && ((fmt[i] == '0') || (fmt[i] == '-'))) {
            flag = (char)fmt[i++];
        }
        int width = 0;
        if ((i < len) && (fmt[i] >= '1') && (fmt[i] <= '9')) {
            width = fmt[i++] - '0';
        }
        int prec = -1;
        if ((i < len) && (fmt[i] == '.')) {
            i++;
            if ((i >= len) || (fmt[i] < '0') || (fmt[i] > '8')) {
                return PLC_ERR_PARM;
            }
            prec = fmt[i++] - '0';
        }
        char kind = (i < len) ? (char)fmt[i++] : 0;
        bool num = (kind == 'd') || (kind == 'x') || (kind == 'X');
        if (!(num || (kind == 'c') || (kind == 'f')) || (flag && !width) ||
            ((kind == 'c') && (width || (prec >= 0))) || (num && (prec >= 0)) || (used == n)) {
            return PLC_ERR_PARM;
        }
        uint32_t v;
        if (!arg(&v)) {
            return PLC_ERR_OFR_SP;
        }
        used++;
        convert(&o, kind, flag, width, prec, v);
    }
    if (used != n) {
        return PLC_ERR_PARM;
    }

    if (o.len > OUT_MAX) {
        *ok = 0;
        return PLC_RUN;
    }
    return put(stream, o.buf, o.len, ok);
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
