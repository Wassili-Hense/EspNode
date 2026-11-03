// Host tests of the PLC output streams (components/plc/plc_stream.c): PrintF, the node log, UART on fake
// hardware
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fake_hw.h"
#include "plc_io.h"
#include "plc_stream.h"

static int failed, passed;

#define CHECK(cond) do { \
    if (cond) { passed++; } else { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define LOG_INFO    (PLC_STREAM_LOG + 1)
#define LOG_WARN    (PLC_STREAM_LOG + 2)
#define LINE_MAX_LEN 43

// Lines of the log
static struct {
    int    level;
    size_t len;
    char   text[64];
} lines[8];
static int n_lines;

static void on_log(int level, const char *text, size_t len)
{
    if ((n_lines < 8) && (len < sizeof(lines[0].text))) {
        lines[n_lines].level = level;
        lines[n_lines].len = len;
        memcpy(lines[n_lines].text, text, len);
        lines[n_lines].text[len] = 0;
    }
    n_lines++;
}

static int line_is(int i, int level, const char *text)
{
    return (i < n_lines) && (lines[i].level == level) && (lines[i].len == strlen(text)) &&
           (strcmp(lines[i].text, text) == 0);
}

// As main.c: MQTT-SN link on UART1; UART2 open on 4/5
static void setup(void)
{
    const plc_io_config_t cfg = {
        .reserved_pins  = 0xFC0ull | (1ull << 16) | (1ull << 17),
        .reserved_uarts = 1u << 1,
    };
    fake_reset();
    plc_io_init(&cfg);
    CHECK(plc_io_config(4, PLC_PIN_UART2_TX, 9600) == PLC_RUN);
    CHECK(plc_io_config(5, PLC_PIN_UART2_RX, 0) == PLC_RUN);
    plc_stream_init(on_log);
    n_lines = 0;
}

static uint32_t bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

// The values of PrintF, as the VM stack gives them
static uint32_t vals[16];
static uint32_t n_vals, taken;

static bool next_val(uint32_t *v)
{
    if (taken >= n_vals) {
        return false;
    }
    *v = vals[taken++];
    return true;
}

static plc_status_t pf(uint32_t stream, const char *fmt, size_t len, uint32_t n, const uint32_t *v, uint32_t *ok)
{
    memcpy(vals, v, n * sizeof(v[0]));
    n_vals = n;
    taken = 0;
    return plc_stream_printf(stream, (const uint8_t *)fmt, len, n, next_val, ok);
}

// The format is a literal: its length is the one of the literal, a \x00 in it included
#define ARGS(...)       ((const uint32_t[]){ 0, ##__VA_ARGS__ } + 1)
#define N_ARGS(...)     ((uint32_t)(sizeof((const uint32_t[]){ 0, ##__VA_ARGS__ }) / sizeof(uint32_t) - 1))
#define PF(stream, fmt, ...)    pf(stream, fmt, sizeof(fmt) - 1, N_ARGS(__VA_ARGS__), ARGS(__VA_ARGS__), &ok)
#define PF_IS(text, fmt, ...)   pf_is(text, fmt, sizeof(fmt) - 1, N_ARGS(__VA_ARGS__), ARGS(__VA_ARGS__))
#define PF_ERR(fmt, ...)        (PF(LOG_INFO, fmt, ##__VA_ARGS__) == PLC_ERR_PARM)

// The output as one Info line, all values taken
static int pf_is(const char *text, const char *fmt, size_t len, uint32_t n, const uint32_t *v)
{
    uint32_t ok = 0;
    n_lines = 0;
    if ((pf(LOG_INFO, fmt, len, n, v, &ok) != PLC_RUN) || !ok || (taken != n)) {
        return 0;
    }
    plc_stream_flush();
    return (n_lines == 1) && line_is(0, 1, text);
}

static int bytes(uint32_t stream, const char *text)
{
    uint32_t ok = 1;
    for (const char *p = text; *p != 0; p++) {
        if ((plc_stream_byte(stream, (uint8_t)*p, &ok) != PLC_RUN) || !ok) {
            return 0;
        }
    }
    return 1;
}

static int sent(int port, const char *text, size_t len)
{
    return (fake_uart_tx_len[port] == len) && (memcmp(fake_uart_tx[port], text, len) == 0);
}

static void test_printf(void)
{
    setup();
    CHECK(PF_IS("T=", "T="));
    CHECK(PF_IS("100%", "100%%"));
    CHECK(PF_IS("A", "%c", 0x141));                     // Low 8 bits

    CHECK(PF_IS("0 -1", "%d %d", 0, (uint32_t)-1));
    CHECK(PF_IS("-2147483648 2147483647", "%d %d", (uint32_t)INT32_MIN, INT32_MAX));
    CHECK(PF_IS("   42|42   |-0042", "%5d|%-5d|%05d", 42, 42, (uint32_t)-42));
    CHECK(PF_IS("12345", "%2d", 12345));                // Width does not cut

    CHECK(PF_IS("deadbeef DEADBEEF", "%x %X", 0xDEADBEEF, 0xDEADBEEF));
    CHECK(PF_IS("0000001f 000000AB", "%08x %08X", 0x1F, 0xAB));
    CHECK(PF_IS("a   |ffffffff", "%-4x|%x", 0xA, (uint32_t)-1));

    CHECK(PF_IS("21.500000", "%f", bits(21.5f)));
    CHECK(PF_IS("21.5 3", "%.1f %.0f", bits(21.46f), bits(2.6f)));
    CHECK(PF_IS("  2.3|2.3  |-02.3", "%5.1f|%-5.1f|%05.1f", bits(2.26f), bits(2.26f), bits(-2.26f)));
    CHECK(PF_IS("0.50000000 -3.14", "%.8f %.2f", bits(0.5f), bits(-3.14159f)));
    CHECK(PF_IS("-0.000000 -0.0", "%f %.1f", bits(-0.0f), bits(-0.0f)));        // Negative zero kept
    CHECK(PF_IS("nan nan inf -inf", "%f %f %f %f", bits(NAN), bits(-NAN), bits(INFINITY), bits(-INFINITY)));
    CHECK(PF_IS("  nan|nan  |  inf", "%5f|%-5f|%05f", bits(NAN), bits(NAN), bits(INFINITY)));   // Spaces, as printf

    // The example of docs/plc-api.md: bytes above 0x7F as they are, a \x00 ends the log line
    uint32_t ok = 0;
    n_lines = 0;
    CHECK((PF(LOG_INFO, "%c %d %08x T=%.1f\xB0" "C\x00", 0x11, (uint32_t)-5, 0xBEEF, bits(21.46f)) == PLC_RUN) && ok);
    CHECK((n_lines == 1) && line_is(0, 1, "\x11 -5 0000beef T=21.5\xB0" "C"));
    plc_stream_flush();
    CHECK(n_lines == 1);
    CHECK((PF(2, "%c %d %08x T=%.1f\xB0" "C\x00", 0x11, (uint32_t)-5, 0xBEEF, bits(21.46f)) == PLC_RUN) && ok);
    plc_stream_flush();
    static const char out[] = "\x11 -5 0000beef T=21.5\xB0" "C\x00";       // The 0 goes to the UART
    CHECK(sent(2, out, sizeof(out) - 1));
}

static void test_errors(void)
{
    uint32_t ok;
    setup();

    // What the compiler refuses stops the program
    CHECK(PF_ERR("%s", 1));
    CHECK(PF_ERR("%u", 1));
    CHECK(PF_ERR("%ld", 1));
    CHECK(PF_ERR("%*d", 1, 1));
    CHECK(PF_ERR("%+d", 1));
    CHECK(PF_ERR("%-d", 1));                            // Padding without width
    CHECK(PF_ERR("%0d", 1));
    CHECK(PF_ERR("%10d", 1));                           // Width 1..9
    CHECK(PF_ERR("%.9f", 1));                           // Precision 0..8
    CHECK(PF_ERR("%.f", 1));
    CHECK(PF_ERR("%5c", 1));
    CHECK(PF_ERR("%.1c", 1));
    CHECK(PF_ERR("%.1d", 1));
    CHECK(PF_ERR("%.1x", 1));
    CHECK(PF_ERR("x%", 1));
    CHECK(PF_ERR("%5", 1));
    CHECK(PF_ERR("%d"));                                // Values: fewer, more
    CHECK(PF_ERR("%d", 1, 2));
    CHECK(PF_ERR("x", 1));
    CHECK(PF(1, "x") == PLC_ERR_PARM);                  // MQTT-SN link
    CHECK(PF(PLC_STREAM_LOG + 4, "x") == PLC_ERR_PARM);
    plc_stream_flush();
    CHECK((n_lines == 0) && (fake_uart_tx_len[2] == 0));

    // Stack underflow
    n_vals = 0;
    taken = 0;
    CHECK(plc_stream_printf(LOG_INFO, (const uint8_t *)"%d", 2, 1, next_val, &ok) == PLC_ERR_OFR_SP);
}

static void test_streams(void)
{
    uint32_t ok;
    setup();
    CHECK(plc_stream_byte(PLC_STREAM_LOG - 1, 'x', &ok) == PLC_ERR_PARM);
    CHECK(plc_stream_byte(PLC_STREAM_LOG + 4, 'x', &ok) == PLC_ERR_PARM);
    CHECK(plc_stream_byte(0, 'x', &ok) == PLC_ERR_PARM);       // UART0 not open
    CHECK(plc_stream_byte(1, 'x', &ok) == PLC_ERR_PARM);       // MQTT-SN link
    CHECK(plc_stream_byte(3, 'x', &ok) == PLC_ERR_PARM);
    CHECK((plc_stream_byte(PLC_STREAM_LOG, 'D', &ok) == PLC_RUN) && (ok == 1));
    CHECK((plc_stream_byte(PLC_STREAM_LOG + 3, 'E', &ok) == PLC_RUN) && (ok == 1));
    plc_stream_flush();
    CHECK((n_lines == 2) && line_is(0, 0, "D") && line_is(1, 3, "E"));
    plc_stream_init(NULL);                                      // No log: lines dropped
    CHECK(bytes(LOG_WARN, "x"));
    plc_stream_flush();
    CHECK(n_lines == 2);
}

static void test_log(void)
{
    uint32_t ok;
    setup();

    // A 0 byte ends the line at once, the rest ends with the cycle; empty lines are not sent
    CHECK(bytes(LOG_WARN, "T="));
    CHECK((PF(LOG_WARN, "%.1f", bits(21.46f)) == PLC_RUN) && ok);
    CHECK((plc_stream_byte(LOG_WARN, 0x100, &ok) == PLC_RUN) && ok);   // Low byte 0
    CHECK((n_lines == 1) && line_is(0, 2, "T=21.5"));
    CHECK((plc_stream_byte(LOG_WARN, 0, &ok) == PLC_RUN) && ok);
    CHECK((PF(LOG_WARN, "n=%d", 7) == PLC_RUN) && ok);
    CHECK((plc_stream_byte(LOG_INFO, 0x141, &ok) == PLC_RUN) && ok);   // Low byte 'A'
    CHECK(n_lines == 1);
    plc_stream_flush();
    CHECK((n_lines == 3) && line_is(1, 1, "A") && line_is(2, 2, "n=7"));
    plc_stream_flush();
    CHECK(n_lines == 3);

    // Two lines in one PrintF
    n_lines = 0;
    CHECK((PF(LOG_INFO, "a=%d\x00" "b=%d", 1, 2) == PLC_RUN) && ok);
    CHECK((n_lines == 1) && line_is(0, 1, "a=1"));
    plc_stream_flush();
    CHECK((n_lines == 2) && line_is(1, 1, "b=2"));

    // 43 bytes, the 44th does not fit; the output goes whole or not at all
    n_lines = 0;
    for (int i = 0; i < LINE_MAX_LEN; i++) {
        CHECK((plc_stream_byte(LOG_INFO, 'a' + i % 26, &ok) == PLC_RUN) && ok);
    }
    CHECK((plc_stream_byte(LOG_INFO, 'z', &ok) == PLC_RUN) && (ok == 0));
    plc_stream_flush();
    CHECK((n_lines == 1) && (lines[0].len == LINE_MAX_LEN) && (lines[0].text[LINE_MAX_LEN - 1] == 'q'));
    n_lines = 0;
    for (int i = 0; i < LINE_MAX_LEN - 3; i++) {
        bytes(LOG_INFO, "x");
    }
    CHECK((PF(LOG_INFO, "%d", 1234) == PLC_RUN) && (ok == 0) && (taken == 1));
    CHECK((PF(LOG_INFO, "%d", 123) == PLC_RUN) && (ok == 1));
    plc_stream_flush();
    CHECK((n_lines == 1) && (lines[0].len == LINE_MAX_LEN) && (memcmp(&lines[0].text[40], "123", 3) == 0));
    // The first line would fit, the second not: neither
    char fmt[LINE_MAX_LEN + 3] = "a";
    memset(&fmt[2], 'y', LINE_MAX_LEN + 1);
    n_lines = 0;
    CHECK((pf(LOG_INFO, fmt, sizeof(fmt), 0, ARGS(), &ok) == PLC_RUN) && (ok == 0));
    plc_stream_flush();
    CHECK(n_lines == 0);

    // Program start and stop drop what was not sent
    n_lines = 0;
    CHECK(bytes(LOG_INFO, "lost"));
    plc_stream_reset();
    plc_stream_flush();
    CHECK(n_lines == 0);
    CHECK(bytes(LOG_INFO, "kept"));                             // The log callback stays
    plc_stream_flush();
    CHECK((n_lines == 1) && line_is(0, 1, "kept"));
}

static void test_uart(void)
{
    uint32_t ok;
    setup();

    // After the cycle; the FIFO takes what it can, the rest goes with the next ones
    CHECK(bytes(2, "hello"));
    CHECK(fake_uart_tx_len[2] == 0);
    fake_uart_tx_room = 3;
    plc_stream_flush();
    CHECK(sent(2, "hel", 3));
    CHECK((PF(2, "%d", (uint32_t)-42) == PLC_RUN) && ok);
    fake_uart_tx_room = 100;
    plc_stream_flush();
    CHECK(sent(2, "hello-42", 8));

    // 256 bytes wait; when full, a byte is refused and the output of PrintF is not cut
    fake_uart_tx_len[2] = 0;
    fake_uart_tx_room = 0;
    for (int i = 0; i < PLC_STREAM_UART_SIZE - 3; i++) {
        CHECK((plc_stream_byte(2, i, &ok) == PLC_RUN) && ok);
    }
    CHECK((PF(2, "%d", 1000) == PLC_RUN) && (ok == 0));
    CHECK((PF(2, "%d", 100) == PLC_RUN) && (ok == 1));
    CHECK((plc_stream_byte(2, 'x', &ok) == PLC_RUN) && (ok == 0));
    // Through the end of the ring: 10 out, 10 more in, all out
    fake_uart_tx_room = 10;
    plc_stream_flush();
    CHECK(fake_uart_tx_len[2] == 10);
    for (int i = 0; i < 10; i++) {
        CHECK((plc_stream_byte(2, 'A' + i, &ok) == PLC_RUN) && ok);
    }
    CHECK((plc_stream_byte(2, 'x', &ok) == PLC_RUN) && (ok == 0));
    fake_uart_tx_room = FAKE_UART_BUF;
    plc_stream_flush();
    CHECK(fake_uart_tx_len[2] == PLC_STREAM_UART_SIZE + 10);
    int in_order = 1;
    for (int i = 0; i < PLC_STREAM_UART_SIZE - 3; i++) {
        in_order &= (fake_uart_tx[2][i] == (uint8_t)i);
    }
    CHECK(in_order);
    CHECK(memcmp(&fake_uart_tx[2][PLC_STREAM_UART_SIZE - 3], "100ABCDEFGHIJ", 13) == 0);

    // Longer than the buffer: refused, the values are taken anyway
    static char big[PLC_STREAM_UART_SIZE + 2];
    memset(big, 'x', sizeof(big));
    memcpy(big, "%d", 2);
    fake_uart_tx_len[2] = 0;
    CHECK((pf(2, big, sizeof(big), 1, ARGS(5), &ok) == PLC_RUN) && (ok == 0) && (taken == 1));
    plc_stream_flush();
    CHECK(fake_uart_tx_len[2] == 0);

    // Closed port: its bytes are dropped
    fake_uart_tx_room = 0;
    CHECK(bytes(2, "old"));
    CHECK(plc_io_config(4, PLC_PIN_OFF, 0) == PLC_RUN);
    CHECK(plc_stream_byte(2, 'x', &ok) == PLC_ERR_PARM);
    CHECK(PF(2, "x") == PLC_ERR_PARM);
    fake_uart_tx_room = 100;
    plc_stream_flush();
    CHECK(plc_io_config(4, PLC_PIN_UART2_TX, 9600) == PLC_RUN);
    plc_stream_flush();
    CHECK(fake_uart_tx_len[2] == 0);
    CHECK(bytes(2, "new"));
    plc_stream_flush();
    CHECK(sent(2, "new", 3));
}

int main(void)
{
    test_printf();
    test_errors();
    test_streams();
    test_log();
    test_uart();

    printf("%d passed, %d failed\n", passed, failed);
    printf(failed ? "FAILED\n" : "ALL PASSED\n");
    return failed ? 1 : 0;
}
