// Host tests of the PLC output streams (components/plc/plc_stream.c): numbers as text, the node log, UART
// on fake hardware
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

// The float as one Info line
static int float_is(uint32_t format, float f, const char *text)
{
    uint32_t ok = 0;
    n_lines = 0;
    if ((plc_stream_float(LOG_INFO, format, bits(f), &ok) != PLC_RUN) || !ok) {
        return 0;
    }
    plc_stream_flush();
    return (n_lines == 1) && line_is(0, 1, text);
}

static int int_is(int32_t v, const char *text)
{
    uint32_t ok = 0;
    n_lines = 0;
    if ((plc_stream_int(LOG_INFO, (uint32_t)v, &ok) != PLC_RUN) || !ok) {
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

static int sent(int port, const char *text)
{
    return (fake_uart_tx_len[port] == strlen(text)) && (memcmp(fake_uart_tx[port], text, strlen(text)) == 0);
}

static void test_numbers(void)
{
    uint32_t ok;
    setup();
    CHECK(int_is(0, "0"));
    CHECK(int_is(-1, "-1"));
    CHECK(int_is(INT32_MIN, "-2147483648"));
    CHECK(int_is(INT32_MAX, "2147483647"));

    // format cba: a - 1 digits (0 - %f), width b, c: 0 zeros, 1 spaces left, 2 spaces right
    CHECK(float_is(0, 21.5f, "21.500000"));
    CHECK(float_is(3, 21.456f, "21.46"));
    CHECK(float_is(1, 21.46f, "21"));
    CHECK(float_is(21, 7.4f, "07"));
    CHECK(float_is(21, 123.0f, "123"));            // Width is the minimum
    CHECK(float_is(31, -5.0f, "-05"));
    CHECK(float_is(152, 2.26f, "  2.3"));
    CHECK(float_is(252, 2.26f, "2.3  "));
    CHECK(float_is(141, 3.6f, "   4"));
    CHECK(float_is(9, 0.5f, "0.50000000"));
    CHECK(float_is(299, 1.0f, "1.00000000"));
    CHECK(float_is(3, -3.14159f, "-3.14"));
    CHECK(float_is(0, NAN, "nan"));
    CHECK(float_is(0, -NAN, "nan"));               // Windows: "-nan(ind)"
    CHECK(float_is(141, NAN, " nan"));
    CHECK(float_is(241, NAN, "nan "));
    CHECK(float_is(0, INFINITY, "inf"));
    CHECK(float_is(0, -INFINITY, "-inf"));
    CHECK(float_is(51, INFINITY, "  inf"));        // As printf: no zeros before inf
    CHECK(plc_stream_float(LOG_INFO, 300, bits(1.0f), &ok) == PLC_ERR_PARM);
    CHECK(plc_stream_float(LOG_INFO, 0xFFFFFFFF, bits(1.0f), &ok) == PLC_ERR_PARM);   // -1

    // Longer than a log line: whole to the UART
    n_lines = 0;
    CHECK((plc_stream_float(LOG_INFO, 0, bits(FLT_MAX), &ok) == PLC_RUN) && (ok == 0));
    plc_stream_flush();
    CHECK(n_lines == 0);
    CHECK((plc_stream_float(2, 0, bits(-FLT_MAX), &ok) == PLC_RUN) && (ok == 1));
    plc_stream_flush();
    CHECK(sent(2, "-340282346638528859811704183484516925440.000000"));
}

static void test_streams(void)
{
    uint32_t ok;
    setup();
    CHECK(plc_stream_byte(PLC_STREAM_LOG - 1, 'x', &ok) == PLC_ERR_PARM);
    CHECK(plc_stream_byte(PLC_STREAM_LOG + 4, 'x', &ok) == PLC_ERR_PARM);
    CHECK(plc_stream_byte(0, 'x', &ok) == PLC_ERR_PARM);       // UART0
    CHECK(plc_stream_byte(1, 'x', &ok) == PLC_ERR_PARM);       // MQTT-SN link
    CHECK(plc_stream_byte(3, 'x', &ok) == PLC_ERR_PARM);
    CHECK(plc_stream_int(1, 5, &ok) == PLC_ERR_PARM);
    CHECK(plc_stream_float(1, 0, 0, &ok) == PLC_ERR_PARM);
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
    CHECK((plc_stream_float(LOG_WARN, 2, bits(21.46f), &ok) == PLC_RUN) && ok);
    CHECK((plc_stream_byte(LOG_WARN, 0x100, &ok) == PLC_RUN) && ok);   // Low byte 0
    CHECK((n_lines == 1) && line_is(0, 2, "T=21.5"));
    CHECK((plc_stream_byte(LOG_WARN, 0, &ok) == PLC_RUN) && ok);
    CHECK(bytes(LOG_WARN, "n="));
    CHECK((plc_stream_int(LOG_WARN, 7, &ok) == PLC_RUN) && ok);
    CHECK((plc_stream_byte(LOG_INFO, 0x141, &ok) == PLC_RUN) && ok);   // Low byte 'A'
    CHECK(n_lines == 1);
    plc_stream_flush();
    CHECK((n_lines == 3) && line_is(1, 1, "A") && line_is(2, 2, "n=7"));
    plc_stream_flush();
    CHECK(n_lines == 3);

    // 43 bytes, the 44th does not fit; a number goes whole or not at all
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
    CHECK((plc_stream_int(LOG_INFO, 1234, &ok) == PLC_RUN) && (ok == 0));
    CHECK((plc_stream_int(LOG_INFO, 123, &ok) == PLC_RUN) && (ok == 1));
    plc_stream_flush();
    CHECK((n_lines == 1) && (lines[0].len == LINE_MAX_LEN) && (memcmp(&lines[0].text[40], "123", 3) == 0));

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
    CHECK(sent(2, "hel"));
    CHECK((plc_stream_int(2, -42, &ok) == PLC_RUN) && ok);
    fake_uart_tx_room = 100;
    plc_stream_flush();
    CHECK(sent(2, "hello-42"));

    // 256 bytes wait; when full, a byte is refused and a number is not cut
    fake_uart_tx_len[2] = 0;
    fake_uart_tx_room = 0;
    for (int i = 0; i < PLC_STREAM_UART_SIZE - 3; i++) {
        CHECK((plc_stream_byte(2, i, &ok) == PLC_RUN) && ok);
    }
    CHECK((plc_stream_int(2, 1000, &ok) == PLC_RUN) && (ok == 0));
    CHECK((plc_stream_int(2, 100, &ok) == PLC_RUN) && (ok == 1));
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

    // Closed port: its bytes are dropped
    fake_uart_tx_len[2] = 0;
    fake_uart_tx_room = 0;
    CHECK(bytes(2, "old"));
    CHECK(plc_io_config(4, PLC_PIN_OFF, 0) == PLC_RUN);
    CHECK(plc_stream_byte(2, 'x', &ok) == PLC_ERR_PARM);
    fake_uart_tx_room = 100;
    plc_stream_flush();
    CHECK(plc_io_config(4, PLC_PIN_UART2_TX, 9600) == PLC_RUN);
    plc_stream_flush();
    CHECK(fake_uart_tx_len[2] == 0);
    CHECK(bytes(2, "new"));
    plc_stream_flush();
    CHECK(sent(2, "new"));
}

int main(void)
{
    test_numbers();
    test_streams();
    test_log();
    test_uart();

    printf("%d passed, %d failed\n", passed, failed);
    printf(failed ? "FAILED\n" : "ALL PASSED\n");
    return failed ? 1 : 0;
}
