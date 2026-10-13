// Fake plc_io_hw.h for host tests
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fake_hw.h"

int64_t  fake_now_ms;
bool     fake_level[PLC_HW_PINS];
int32_t  fake_mv[PLC_HW_PINS];
uint8_t  fake_dac[PLC_HW_PINS];
uint32_t fake_duty[PLC_HW_PWM_CHANNELS];
int32_t  fake_count[PLC_HW_COUNTERS];
int      fake_twi_status;
uint8_t  fake_twi_rx[32];
uint8_t  fake_twi_tx[32];
size_t   fake_twi_tx_len;
uint8_t  fake_uart_rx[PLC_HW_UART_PORTS][FAKE_UART_BUF];
size_t   fake_uart_rx_len[PLC_HW_UART_PORTS];
uint8_t  fake_uart_tx[PLC_HW_UART_PORTS][FAKE_UART_BUF];
size_t   fake_uart_tx_len[PLC_HW_UART_PORTS];
size_t   fake_uart_tx_room;
const char *fake_fail;
char     fake_calls[16384];

static bool s_output[PLC_HW_PINS];

void fake_reset(void)
{
    fake_now_ms = 1000;
    memset(fake_level, 0, sizeof(fake_level));
    memset(fake_mv, 0, sizeof(fake_mv));
    memset(fake_dac, 0, sizeof(fake_dac));
    memset(fake_duty, 0, sizeof(fake_duty));
    memset(fake_count, 0, sizeof(fake_count));
    fake_twi_status = PLC_TWI_OK;
    memset(fake_twi_rx, 0, sizeof(fake_twi_rx));
    fake_twi_tx_len = 0;
    memset(fake_uart_rx_len, 0, sizeof(fake_uart_rx_len));
    memset(fake_uart_tx_len, 0, sizeof(fake_uart_tx_len));
    fake_uart_tx_room = FAKE_UART_BUF;
    fake_fail = NULL;
    memset(s_output, 0, sizeof(s_output));
    fake_clear();
}

void fake_clear(void)
{
    fake_calls[0] = 0;
}

bool fake_called(const char *call)
{
    size_t n = strlen(call);
    for (const char *p = fake_calls; *p; p = strchr(p, '\n') + 1) {
        if ((strncmp(p, call, n) == 0) && ((p[n] == ' ') || (p[n] == '\n'))) {
            return true;
        }
    }
    return false;
}

// Log the call; false if it is to fail
static bool call(const char *name, const char *fmt, ...)
{
    size_t len = strlen(fake_calls);
    snprintf(fake_calls + len, sizeof(fake_calls) - len, "%s", name);
    len = strlen(fake_calls);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fake_calls + len, sizeof(fake_calls) - len, fmt, ap);
    va_end(ap);
    len = strlen(fake_calls);
    snprintf(fake_calls + len, sizeof(fake_calls) - len, "\n");
    return (fake_fail == NULL) || (strcmp(fake_fail, name) != 0);
}

// ESP32: GPIO0-39 without 20, 24, 28-31; 34-39 input only; ADC1 on 32-39; DAC on 25, 26
uint32_t plc_hw_caps(uint32_t pin)
{
    if ((pin >= PLC_HW_PINS) || (pin == 20) || (pin == 24) || ((pin >= 28) && (pin <= 31))) {
        return 0;
    }
    uint32_t caps = PLC_HW_CAP_VALID;
    if (pin < 34) {
        caps |= PLC_HW_CAP_OUTPUT;
    }
    if (pin >= 32) {
        caps |= PLC_HW_CAP_ADC;
    }
    if ((pin == 25) || (pin == 26)) {
        caps |= PLC_HW_CAP_DAC;
    }
    return caps;
}

int64_t plc_hw_now_ms(void)
{
    return fake_now_ms;
}

void plc_hw_release(uint32_t pin)
{
    s_output[pin] = false;
    call("release", " %u", (unsigned)pin);
}

bool plc_hw_gpio_input(uint32_t pin, int pull)
{
    s_output[pin] = false;
    return call("gpio_input", " %u %d", (unsigned)pin, pull);
}

bool plc_hw_gpio_output(uint32_t pin, bool open_drain, bool level)
{
    if (!call("gpio_output", " %u %d %d", (unsigned)pin, open_drain, level)) {
        return false;
    }
    s_output[pin] = true;
    fake_level[pin] = level;
    return true;
}

bool plc_hw_gpio_get(uint32_t pin)
{
    return fake_level[pin];
}

void plc_hw_gpio_set(uint32_t pin, bool level)
{
    call("gpio_set", " %u %d", (unsigned)pin, level);
    if (s_output[pin]) {
        fake_level[pin] = level;
    }
}

bool plc_hw_adc_open(uint32_t pin, int atten)
{
    return call("adc_open", " %u %d", (unsigned)pin, atten);
}

bool plc_hw_adc_read(uint32_t pin, int atten, int32_t *mv)
{
    (void)atten;
    *mv = fake_mv[pin];
    return true;
}

bool plc_hw_dac_open(uint32_t pin)
{
    fake_dac[pin] = 0;
    return call("dac_open", " %u", (unsigned)pin);
}

void plc_hw_dac_write(uint32_t pin, uint8_t value)
{
    call("dac_write", " %u %u", (unsigned)pin, value);
    fake_dac[pin] = value;
}

void plc_hw_dac_close(uint32_t pin)
{
    call("dac_close", " %u", (unsigned)pin);
}

bool plc_hw_pwm_timer(int timer, uint32_t freq)
{
    return call("pwm_timer", " %d %u", timer, (unsigned)freq);
}

bool plc_hw_pwm_open(int channel, int timer, uint32_t pin, bool invert)
{
    fake_duty[channel] = 0;
    return call("pwm_open", " %d %d %u %d", channel, timer, (unsigned)pin, invert);
}

void plc_hw_pwm_write(int channel, int timer, uint32_t duty)
{
    call("pwm_write", " %d %d %u", channel, timer, (unsigned)duty);
    fake_duty[channel] = duty;
}

void plc_hw_pwm_close(int channel)
{
    call("pwm_close", " %d", channel);
}

bool plc_hw_cnt_open(int unit, uint32_t pin, int edges, uint32_t filter_ns)
{
    fake_count[unit] = 0;
    return call("cnt_open", " %d %u %d %u", unit, (unsigned)pin, edges, (unsigned)filter_ns);
}

int32_t plc_hw_cnt_read(int unit)
{
    return fake_count[unit];
}

void plc_hw_cnt_close(int unit)
{
    call("cnt_close", " %d", unit);
}

bool plc_hw_twi_open(int bus, uint32_t sda, uint32_t scl, uint32_t khz)
{
    return call("twi_open", " %d %u %u %u", bus, (unsigned)sda, (unsigned)scl, (unsigned)khz);
}

int plc_hw_twi_xfer(int bus, uint8_t addr, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
    call("twi_xfer", " %d 0x%02X %u %u", bus, addr, (unsigned)tx_len, (unsigned)rx_len);
    memcpy(fake_twi_tx, tx, tx_len);
    fake_twi_tx_len = tx_len;
    if (fake_twi_status == PLC_TWI_OK) {
        memcpy(rx, fake_twi_rx, rx_len);
    }
    return fake_twi_status;
}

void plc_hw_twi_close(int bus)
{
    call("twi_close", " %d", bus);
}

bool plc_hw_uart_open(int port, uint32_t tx, uint32_t rx, int de, uint32_t baud, uint32_t format)
{
    return call("uart_open", " %d %u %u %d %u %u", port, (unsigned)tx, (unsigned)rx, de, (unsigned)baud,
                (unsigned)format);
}

size_t plc_hw_uart_avail(int port)
{
    return fake_uart_rx_len[port];
}

int plc_hw_uart_get(int port)
{
    if (fake_uart_rx_len[port] == 0) {
        return -1;
    }
    int byte = fake_uart_rx[port][0];
    memmove(fake_uart_rx[port], &fake_uart_rx[port][1], --fake_uart_rx_len[port]);
    return byte;
}

bool plc_hw_uart_put(int port, uint8_t byte)
{
    if ((fake_uart_tx_room == 0) || (fake_uart_tx_len[port] >= FAKE_UART_BUF)) {
        return false;
    }
    fake_uart_tx_room--;
    fake_uart_tx[port][fake_uart_tx_len[port]++] = byte;
    return true;
}

void plc_hw_uart_close(int port)
{
    call("uart_close", " %d", port);
}
