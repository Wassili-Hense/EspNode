// Host tests of the PLC IO: plc_io.c logic on fake hardware, then bytecode through
// plc_vm.c -> plc_api.c -> plc_io.c
#include <stdio.h>
#include <string.h>

#include "fake_hw.h"
#include "mqttsn_client.h"
#include "plc_api.h"
#include "plc_io.h"
#include "plc_vm.h"

static int failed, passed;

#define CHECK(cond) do { \
    if (cond) { passed++; } else { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
                                    printf("  calls:\n%s", fake_calls); } \
} while (0)

#define CALLED(call)    fake_called(call)
#define CFG(pin, mode, opt)  plc_io_config(pin, PLC_PIN_##mode, opt)

// Platform used by plc_api.c
uint32_t esp_random(void) { return 4; }
int64_t esp_timer_get_time(void) { return fake_now_ms * 1000; }
mqttsn_status_t mqttsn_client_status(void) { return MQTTSN_STATUS_CONNECT; }

// As main.c: flash 6-11, console 1/3, MQTT-SN link on UART1 16/17
static void setup(void)
{
    const plc_io_config_t cfg = {
        .reserved_pins  = 0xFC0ull | (1ull << 1) | (1ull << 3) | (1ull << 16) | (1ull << 17),
        .reserved_uarts = 1u << 1,
    };
    fake_reset();
    plc_io_init(&cfg);
}

static uint32_t in(uint32_t pin)
{
    uint32_t val = 0xDEADBEEF;
    CHECK(plc_io_in(pin, &val) == PLC_RUN);
    return val;
}

static void test_pins(void)
{
    uint32_t v;
    setup();
    CHECK(CFG(40, IN, 0) == PLC_ERR_PARM);
    CHECK(CFG(20, IN, 0) == PLC_ERR_PARM);          // No GPIO20
    CHECK(CFG(6, IN, 0) == PLC_ERR_PARM);           // Flash
    CHECK(CFG(1, OUT, 0) == PLC_ERR_PARM);          // Console
    CHECK(CFG(16, IN, 0) == PLC_ERR_PARM);          // MQTT-SN link
    CHECK(CFG(4, ONEWIRE, 0) == PLC_ERR_PARM);      // Not implemented
    CHECK(CFG(4, MODES, 0) == PLC_ERR_PARM);
    CHECK(plc_io_config(4, 0xFFFFFFFF, 0) == PLC_ERR_PARM);
    CHECK(CFG(4, OFF, 0) == PLC_RUN);               // Free pin: nothing to do
    CHECK(fake_calls[0] == 0);
    CHECK(plc_io_in(4, &v) == PLC_ERR_PARM);        // Not configured
    CHECK(plc_io_out(4, 1) == PLC_ERR_PARM);
    CHECK(plc_io_in(40, &v) == PLC_ERR_PARM);
    CHECK(plc_io_out(0xFFFF, 1) == PLC_ERR_PARM);
}

static void test_digital(void)
{
    setup();

    // Inputs
    fake_level[4] = true;
    CHECK(CFG(4, IN_PU, 0) == PLC_RUN);
    CHECK(CALLED("gpio_input 4 1"));
    CHECK(in(4) == 1);
    fake_level[4] = false;
    CHECK(in(4) == 0);
    CHECK(CFG(5, IN_INV_PU, 0) == PLC_RUN);         // Contact to ground
    CHECK(in(5) == 1);
    CHECK(CFG(18, IN_PD, 0) == PLC_RUN);
    CHECK(CALLED("gpio_input 18 2"));
    CHECK(CFG(34, IN_PU, 0) == PLC_ERR_PARM);       // No pulls on 34-39
    CHECK(CFG(35, IN_PD, 0) == PLC_ERR_PARM);
    CHECK(CFG(34, IN, 0) == PLC_RUN);
    CHECK(CFG(19, IN, -1) == PLC_ERR_PARM);
    CHECK(CFG(19, IN, 60001) == PLC_ERR_PARM);

    // Debounce 50 ms, evaluated at IN
    CHECK(CFG(13, IN, 50) == PLC_RUN);
    CHECK(in(13) == 0);
    fake_level[13] = true;
    CHECK(in(13) == 0);
    fake_now_ms += 30;
    CHECK(in(13) == 0);
    fake_level[13] = false;                         // Glitch
    fake_now_ms += 10;
    CHECK(in(13) == 0);
    fake_level[13] = true;
    fake_now_ms += 10;
    CHECK(in(13) == 0);
    fake_now_ms += 49;
    CHECK(in(13) == 0);
    fake_now_ms += 1;
    CHECK(in(13) == 1);

    // Outputs start logically off
    CHECK(CFG(2, OUT, 0) == PLC_RUN);
    CHECK(CALLED("gpio_output 2 0 0"));
    CHECK(in(2) == 0);
    CHECK(plc_io_out(2, 5) == PLC_RUN);             // Any non-zero is 1
    CHECK(fake_level[2] && (in(2) == 1));
    fake_clear();
    CHECK(plc_io_out(2, 1) == PLC_RUN);             // Unchanged: no hardware access
    CHECK(fake_calls[0] == 0);
    CHECK(plc_io_out(2, 0) == PLC_RUN);
    CHECK(!fake_level[2]);
    CHECK(CFG(12, OUT_INV, 0) == PLC_RUN);
    CHECK(CALLED("gpio_output 12 0 1"));
    CHECK(plc_io_out(12, 1) == PLC_RUN);
    CHECK(!fake_level[12] && (in(12) == 1));
    CHECK(CFG(14, OUT_OD, 0) == PLC_RUN);
    CHECK(CALLED("gpio_output 14 1 0"));
    CHECK(CFG(15, OUT_OD_INV, 0) == PLC_RUN);
    CHECK(CALLED("gpio_output 15 1 1"));
    CHECK(CFG(35, OUT, 0) == PLC_ERR_PARM);         // Input only
    CHECK(plc_io_out(4, 1) == PLC_ERR_PARM);        // Input

    // One function per pin
    plc_io_out(2, 1);
    CHECK(CFG(2, IN, 0) == PLC_ERR_RSC_BUSY);
    CHECK(fake_level[2] && (in(2) == 1));           // Still the output
    CHECK(CFG(2, OUT, 3) == PLC_ERR_RSC_BUSY);      // Other option
    fake_clear();
    CHECK(CFG(2, OUT, 0) == PLC_RUN);               // Same setup again
    CHECK(fake_calls[0] == 0);

    // Release
    CHECK(CFG(2, OFF, 0) == PLC_RUN);
    CHECK(CALLED("release 2"));
    CHECK(plc_io_out(2, 1) == PLC_ERR_PARM);
    CHECK(CFG(2, IN, 0) == PLC_RUN);
}

static void test_analog(void)
{
    uint32_t v;
    setup();
    fake_mv[32] = 1234;
    CHECK(CFG(32, ANALOG, 3) == PLC_RUN);
    CHECK(CALLED("adc_open 32 3"));
    CHECK(in(32) == 1234);
    CHECK(plc_io_out(32, 1) == PLC_ERR_PARM);
    CHECK(CFG(33, ANALOG, 4) == PLC_ERR_PARM);
    CHECK(CFG(33, ANALOG, -1) == PLC_ERR_PARM);
    CHECK(CFG(4, ANALOG, 0) == PLC_ERR_PARM);       // Not ADC1
    CHECK(CFG(39, ANALOG, 0) == PLC_RUN);

    CHECK(CFG(25, DAC, 0) == PLC_RUN);
    CHECK(CALLED("dac_open 25"));
    CHECK(in(25) == 0);
    CHECK(plc_io_out(25, 100) == PLC_RUN);
    CHECK((fake_dac[25] == 100) && (in(25) == 100));
    CHECK(plc_io_out(25, 300) == PLC_RUN);          // Clamped
    CHECK((fake_dac[25] == 255) && (in(25) == 255));
    CHECK(CFG(4, DAC, 0) == PLC_ERR_PARM);
    CHECK(CFG(25, OFF, 0) == PLC_RUN);
    CHECK(CALLED("dac_close 25") && CALLED("release 25"));
    CHECK(plc_io_in(25, &v) == PLC_ERR_PARM);
}

static void test_pwm(void)
{
    setup();
    CHECK(CFG(2, PWM, 0) == PLC_RUN);               // 1000 Hz
    CHECK(CALLED("pwm_timer 0 1000") && CALLED("pwm_open 0 0 2 0"));
    CHECK(plc_io_out(2, 32768) == PLC_RUN);
    CHECK((fake_duty[0] == 32768) && (in(2) == 32768));
    CHECK(plc_io_out(2, 70000) == PLC_RUN);         // Clamped
    CHECK((fake_duty[0] == 65535) && (in(2) == 65535));
    CHECK(CALLED("pwm_write 0 0 65535"));

    fake_clear();
    CHECK(CFG(4, PWM_INV, 1000) == PLC_RUN);        // Same frequency, same timer
    CHECK(!CALLED("pwm_timer") && CALLED("pwm_open 1 0 4 1"));
    CHECK(CFG(5, PWM, 50) == PLC_RUN);
    CHECK(CALLED("pwm_timer 1 50"));
    CHECK(CFG(12, PWM, 20000) == PLC_RUN);
    CHECK(CFG(13, PWM, 5) == PLC_RUN);
    CHECK(CALLED("pwm_timer 3 5"));
    CHECK(CFG(14, PWM, 7) == PLC_ERR_RSC_BUSY);     // Fifth frequency
    CHECK(CFG(14, PWM, 1000) == PLC_RUN);           // The pin is free after the failure
    CHECK(CFG(15, PWM, 50) == PLC_RUN);
    CHECK(CFG(18, PWM, 50) == PLC_RUN);             // Eighth channel
    CHECK(CFG(19, PWM, 50) == PLC_ERR_RSC_BUSY);
    CHECK(CFG(34, PWM, 50) == PLC_ERR_PARM);        // Input only
    CHECK(CFG(21, PWM, -1) == PLC_ERR_PARM);

    fake_clear();
    CHECK(CFG(5, OFF, 0) == PLC_RUN);
    CHECK(CALLED("pwm_close 2") && CALLED("release 5"));
    CHECK(CFG(19, PWM, 50) == PLC_RUN);             // Channel 2, timer 1 still at 50 Hz
    CHECK(CALLED("pwm_open 2 1 19 0") && !CALLED("pwm_timer"));
    CHECK(CFG(12, OFF, 0) == PLC_RUN);              // Timer 2 unused
    CHECK(CFG(21, PWM, 300) == PLC_RUN);
    CHECK(CALLED("pwm_timer 2 300") && CALLED("pwm_open 3 2 21 0"));

    // Hardware refuses
    setup();
    fake_fail = "pwm_timer";
    CHECK(CFG(2, PWM, 40000000) == PLC_ERR_PARM);
    fake_fail = "pwm_open";
    CHECK(CFG(2, PWM, 1000) == PLC_ERR_PARM);
    fake_fail = NULL;
    fake_clear();
    CHECK(CFG(2, PWM, 1000) == PLC_RUN);            // Nothing leaked: channel 0, timer 0
    CHECK(CALLED("pwm_timer 0 1000") && CALLED("pwm_open 0 0 2 0"));
}

static void test_counter(void)
{
    setup();
    CHECK(CFG(4, CNT_RISE, 0) == PLC_RUN);
    CHECK(CALLED("cnt_open 0 4 0 0"));
    fake_count[0] = 10;
    CHECK(in(4) == 10);
    CHECK(plc_io_out(4, 100) == PLC_RUN);           // Set the count
    CHECK(in(4) == 100);
    fake_count[0] = 15;
    CHECK(in(4) == 105);
    CHECK(plc_io_out(4, 0) == PLC_RUN);
    CHECK(in(4) == 0);
    CHECK(CFG(5, CNT_BOTH, 1000) == PLC_RUN);
    CHECK(CALLED("cnt_open 1 5 2 1000"));
    CHECK(CFG(34, CNT_FALL, 0) == PLC_RUN);         // Input only is fine
    CHECK(CALLED("cnt_open 2 34 1 0"));
    CHECK(CFG(12, CNT_RISE, -1) == PLC_ERR_PARM);
    CHECK(CFG(12, CNT_RISE, 0) == PLC_RUN);
    CHECK(CFG(13, CNT_RISE, 0) == PLC_RUN);
    CHECK(CFG(14, CNT_RISE, 0) == PLC_RUN);
    CHECK(CFG(15, CNT_RISE, 0) == PLC_RUN);
    CHECK(CFG(18, CNT_RISE, 0) == PLC_RUN);         // Eighth unit
    CHECK(CFG(19, CNT_RISE, 0) == PLC_ERR_RSC_BUSY);
    CHECK(CFG(4, OFF, 0) == PLC_RUN);
    CHECK(CALLED("cnt_close 0"));
    CHECK(CFG(19, CNT_RISE, 0) == PLC_RUN);
    CHECK(CALLED("cnt_open 0 19 0 0"));
}

static void test_twi(void)
{
    uint32_t v, st;
    setup();
    CHECK(CFG(21, TWI0_SDA, 0) == PLC_RUN);
    CHECK(!CALLED("twi_open"));
    CHECK(plc_io_twi_put(0, 1) == PLC_ERR_PARM);    // Bus not ready
    CHECK(CFG(22, TWI0_SCL, 400) == PLC_RUN);
    CHECK(CALLED("twi_open 0 21 22 400"));
    CHECK(plc_io_in(21, &v) == PLC_ERR_PARM);       // Bus pins have no IN/OUT
    CHECK(plc_io_out(22, 1) == PLC_ERR_PARM);

    // Write 2 bytes, read 2
    fake_twi_rx[0] = 0xAB;
    fake_twi_rx[1] = 0xCD;
    CHECK(plc_io_twi_put(0, 0x10) == PLC_RUN);
    CHECK(plc_io_twi_put(0, 0x120) == PLC_RUN);     // Low byte
    CHECK((plc_io_twi_xfer(0, 0x48, 2, &st) == PLC_RUN) && (st == PLC_TWI_OK));
    CHECK(CALLED("twi_xfer 0 0x48 2 2"));
    CHECK((fake_twi_tx_len == 2) && (fake_twi_tx[0] == 0x10) && (fake_twi_tx[1] == 0x20));
    CHECK((plc_io_twi_get(0, 0, &v) == PLC_RUN) && (v == 0xAB));
    CHECK((plc_io_twi_get(0, 1, &v) == PLC_RUN) && (v == 0xCD));
    CHECK(plc_io_twi_get(0, 2, &v) == PLC_ERR_PARM);

    // Probe: nothing to write, nothing to read
    fake_clear();
    CHECK((plc_io_twi_xfer(0, 0x48, 0, &st) == PLC_RUN) && (st == PLC_TWI_OK));
    CHECK(CALLED("twi_xfer 0 0x48 0 0"));
    CHECK(plc_io_twi_get(0, 0, &v) == PLC_ERR_PARM);

    // NACK is a status, not an error
    fake_twi_status = PLC_TWI_NACK;
    plc_io_twi_put(0, 1);
    CHECK((plc_io_twi_xfer(0, 0x50, 1, &st) == PLC_RUN) && (st == PLC_TWI_NACK));
    CHECK(plc_io_twi_get(0, 0, &v) == PLC_ERR_PARM);
    fake_twi_status = PLC_TWI_OK;

    // Limits
    for (int i = 0; i < 32; i++) {
        CHECK(plc_io_twi_put(0, (uint32_t)i) == PLC_RUN);
    }
    CHECK(plc_io_twi_put(0, 32) == PLC_ERR_PARM);
    CHECK(plc_io_twi_xfer(0, 0x80, 0, &st) == PLC_ERR_PARM);
    CHECK(plc_io_twi_xfer(0, 0x48, 33, &st) == PLC_ERR_PARM);
    CHECK(plc_io_twi_xfer(2, 0x48, 0, &st) == PLC_ERR_PARM);
    CHECK(plc_io_twi_xfer(1, 0x48, 0, &st) == PLC_ERR_PARM);    // Bus 1 not configured
    CHECK((plc_io_twi_xfer(0, 0x48, 0, &st) == PLC_RUN) && (fake_twi_tx_len == 32));

    // Pins
    CHECK(CFG(23, TWI0_SDA, 0) == PLC_ERR_RSC_BUSY);   // Bus 0 SDA is 21
    CHECK(CFG(34, TWI1_SDA, 0) == PLC_ERR_PARM);        // Input only
    CHECK(CFG(19, TWI1_SCL, 1001) == PLC_ERR_PARM);
    CHECK(CFG(18, TWI1_SDA, 0) == PLC_RUN);
    CHECK(CFG(19, TWI1_SCL, 0) == PLC_RUN);
    CHECK(CALLED("twi_open 1 18 19 100"));

    // Releasing a pin closes the bus
    fake_clear();
    CHECK(CFG(21, OFF, 0) == PLC_RUN);
    CHECK(CALLED("twi_close 0") && CALLED("release 21"));
    CHECK(plc_io_twi_put(0, 1) == PLC_ERR_PARM);
    CHECK(CFG(23, TWI0_SDA, 0) == PLC_RUN);
    CHECK(CALLED("twi_open 0 23 22 400"));

    // Hardware refuses: the pin that completed the bus is released
    setup();
    fake_fail = "twi_open";
    CHECK(CFG(21, TWI0_SDA, 0) == PLC_RUN);
    CHECK(CFG(22, TWI0_SCL, 0) == PLC_ERR_PARM);
    CHECK(CALLED("release 22"));
    fake_fail = NULL;
    CHECK(CFG(22, TWI0_SCL, 0) == PLC_RUN);
    CHECK(CALLED("twi_open 0 21 22 100"));
}

static void test_uart(void)
{
    uint32_t v;
    setup();
    CHECK(CFG(4, UART1_TX, 0) == PLC_ERR_RSC_BUSY);    // MQTT-SN link
    CHECK(CFG(4, UART2_TX, 115200) == PLC_RUN);
    CHECK(!CALLED("uart_open"));
    CHECK(plc_io_uart_avail(2, &v) == PLC_ERR_PARM);    // Not open yet
    CHECK(CFG(5, UART2_RX, 1) == PLC_RUN);              // Even parity
    CHECK(CALLED("uart_open 2 4 5 -1 115200 1"));

    CHECK((plc_io_uart_avail(2, &v) == PLC_RUN) && (v == 0));
    fake_uart_rx[2][0] = 0x41;
    fake_uart_rx[2][1] = 0x42;
    fake_uart_rx_len[2] = 2;
    CHECK((plc_io_uart_avail(2, &v) == PLC_RUN) && (v == 2));
    CHECK((plc_io_uart_get(2, &v) == PLC_RUN) && (v == 0x41));
    CHECK((plc_io_uart_get(2, &v) == PLC_RUN) && (v == 0x42));
    CHECK((plc_io_uart_get(2, &v) == PLC_RUN) && (v == 0xFFFFFFFF));
    CHECK((plc_io_uart_put(2, 0x155, &v) == PLC_RUN) && (v == 1));
    CHECK((fake_uart_tx_len[2] == 1) && (fake_uart_tx[2][0] == 0x55));
    fake_uart_tx_room = 0;
    CHECK((plc_io_uart_put(2, 0x56, &v) == PLC_RUN) && (v == 0));
    CHECK(plc_io_uart_get(1, &v) == PLC_ERR_PARM);
    CHECK(plc_io_uart_get(0, &v) == PLC_ERR_PARM);
    CHECK(plc_io_uart_get(3, &v) == PLC_ERR_PARM);

    // RS-485: DE added and removed while open
    fake_clear();
    CHECK(CFG(18, UART2_DE, 0) == PLC_RUN);
    CHECK(CALLED("uart_close 2") && CALLED("uart_open 2 4 5 18 115200 1"));
    fake_clear();
    CHECK(CFG(18, OFF, 0) == PLC_RUN);
    CHECK(CALLED("uart_open 2 4 5 -1 115200 1") && CALLED("release 18"));

    // Options and pins
    CHECK(CFG(13, UART2_RX, 3) == PLC_ERR_PARM);        // Parity 3
    CHECK(CFG(13, UART2_RX, 8) == PLC_ERR_PARM);
    CHECK(CFG(13, UART2_TX, 5000001) == PLC_ERR_PARM);
    CHECK(CFG(34, UART2_TX, 0) == PLC_ERR_PARM);        // Input only
    CHECK(CFG(34, UART2_RX, 0) == PLC_ERR_RSC_BUSY);    // RX is 5
    fake_clear();
    CHECK(CFG(5, OFF, 0) == PLC_RUN);
    CHECK(CALLED("uart_close 2") && !CALLED("uart_open"));
    CHECK(plc_io_uart_avail(2, &v) == PLC_ERR_PARM);
    CHECK(CFG(34, UART2_RX, 6) == PLC_RUN);             // Odd parity, 2 stop bits
    CHECK(CALLED("uart_open 2 4 34 -1 115200 6"));

    // Hardware refuses
    setup();
    fake_fail = "uart_open";
    CHECK(CFG(4, UART2_TX, 0) == PLC_RUN);
    CHECK(CFG(5, UART2_RX, 0) == PLC_ERR_PARM);
    CHECK(CALLED("release 5"));
    CHECK(plc_io_uart_avail(2, &v) == PLC_ERR_PARM);
    fake_fail = NULL;
    CHECK(CFG(5, UART2_RX, 0) == PLC_RUN);
    CHECK(CALLED("uart_open 2 4 5 -1 9600 0"));
}

static void test_restart(void)
{
    uint32_t v;
    setup();
    CFG(2, OUT, 0);
    plc_io_out(2, 1);
    CFG(4, PWM, 1000);
    plc_io_out(4, 1000);
    CFG(5, IN_PU, 0);
    CFG(12, OUT, 0);
    CFG(13, CNT_RISE, 0);
    CFG(21, TWI0_SDA, 0);
    CFG(22, TWI0_SCL, 0);

    // New run: same setups are kept untouched, outputs don't glitch
    plc_io_restart_begin();
    fake_clear();
    CHECK(CFG(2, OUT, 0) == PLC_RUN);
    CHECK(CFG(4, PWM, 1000) == PLC_RUN);
    CHECK(CFG(21, TWI0_SDA, 0) == PLC_RUN);
    CHECK(fake_calls[0] == 0);
    CHECK(fake_level[2] && (in(2) == 1) && (in(4) == 1000));

    // Pins of the previous run are free for other functions
    CHECK(CFG(5, IN, 0) == PLC_RUN);
    CHECK(CALLED("release 5") && CALLED("gpio_input 5 0"));
    CHECK(CFG(12, PWM, 1000) == PLC_RUN);
    CHECK(CALLED("release 12") && CALLED("pwm_open 1 0 12 0"));
    fake_clear();
    CHECK(CFG(19, TWI0_SCL, 0) == PLC_RUN);             // SCL moves from 22 to 19
    CHECK(CALLED("twi_close 0") && CALLED("release 22") && CALLED("twi_open 0 21 19 100"));

    // After the init cycle, the pins not set up again are released
    fake_clear();
    plc_io_restart_end();
    CHECK(CALLED("cnt_close 0") && CALLED("release 13"));
    CHECK(!CALLED("release 2") && !CALLED("release 4") && !CALLED("release 21") && !CALLED("release 19"));
    CHECK(plc_io_in(13, &v) == PLC_ERR_PARM);
    CHECK(in(2) == 1);

    // The new run needs the resources of the previous one before its init cycle ends
    setup();
    const uint32_t old_pins[8] = { 2, 4, 5, 12, 13, 14, 15, 18 };
    for (int i = 0; i < 8; i++) {
        CFG(old_pins[i], PWM, 1000 + i % 4);            // All channels and timers
    }
    plc_io_restart_begin();
    fake_clear();
    CHECK(CFG(19, PWM, 50) == PLC_RUN);
    CHECK(CALLED("pwm_close 0") && CALLED("release 18"));   // All stale PWM pins released
    CHECK(plc_io_in(2, &v) == PLC_ERR_PARM);

    for (int i = 0; i < 8; i++) {
        CFG(old_pins[i], CNT_RISE, 0);                  // All counter units
    }
    plc_io_restart_begin();
    CHECK(CFG(21, CNT_RISE, 0) == PLC_RUN);
    CHECK(CFG(22, CNT_RISE, 0) == PLC_RUN);

    // TWI buffer of a stopped program is not sent by the next one
    setup();
    CFG(21, TWI0_SDA, 0);
    CFG(22, TWI0_SCL, 0);
    plc_io_twi_put(0, 0x77);
    plc_io_restart_begin();
    CFG(21, TWI0_SDA, 0);
    CFG(22, TWI0_SCL, 0);
    plc_io_twi_xfer(0, 0x48, 0, &v);
    CHECK(fake_twi_tx_len == 0);
}

////////////////////////////////////////////////////////////////////////
// Bytecode

enum {
    LDI_0 = 0x38, LDI_U1 = 0x3C, LDI_1 = 0x3E,
    STM_S4_C16 = 0xBB, IN = 0xC0, OUT = 0xC1, API = 0xC6, JMP = 0xF3, RET = 0xFF,
};

#define INIT_AT     64
#define RAM_WORDS   64
static uint8_t prg[256];
static uint32_t ram[RAM_WORDS];

// Address 0: jump to init, 4: cycle
static void load(const uint8_t *init, size_t init_len, const uint8_t *cycle, size_t cycle_len)
{
    memset(prg, 0xFF, sizeof(prg));
    prg[0] = JMP;
    prg[1] = INIT_AT;
    prg[2] = 0;
    memcpy(&prg[4], cycle, cycle_len);
    memcpy(&prg[INIT_AT], init, init_len);
    memset(ram, 0, sizeof(ram));
    const plc_vm_config_t cfg = {
        .prg = prg, .prg_size = sizeof(prg), .ram = ram, .ram_words = RAM_WORDS,
        .api = plc_api_call, .io_in = plc_api_in, .io_out = plc_api_out,
    };
    plc_vm_init(&cfg);
}

#define LOAD(init, cycle) load(init, sizeof(init), cycle, sizeof(cycle))

static void test_program(void)
{
    uint32_t v;

    // ConfigPin1(2, OUT); ConfigPin2(4, IN, 0); cycle: OUT 2 = IN 4. The compiler pushes the last
    // argument first; place and type of the IN/OUT operand are not checked.
    setup();
    {
        const uint8_t init[] = { LDI_U1, 5, LDI_U1, 2, API, 30,
                                 LDI_0, LDI_1, LDI_U1, 4, API, 31, RET };
        const uint8_t cycle[] = { IN, 'I', 'x', 4, 0, OUT, 'O', 'y', 2, 0, RET };
        LOAD(init, cycle);
    }
    CHECK(plc_vm_cycle() == PLC_OK);
    CHECK(CALLED("gpio_output 2 0 0") && CALLED("gpio_input 4 0"));
    fake_level[4] = true;
    CHECK((plc_vm_cycle() == PLC_OK) && fake_level[2]);
    fake_level[4] = false;
    CHECK((plc_vm_cycle() == PLC_OK) && !fake_level[2]);

    // TWI: ram[0] = TwiXfer(0, 0x48, 1) after TwiPut(0, 0x42), ram[1] = TwiGet(0, 0)
    setup();
    {
        const uint8_t init[] = { LDI_U1, 16, LDI_U1, 21, API, 30,
                                 LDI_U1, 100, LDI_U1, 17, LDI_U1, 22, API, 31, RET };
        const uint8_t cycle[] = { LDI_U1, 0x42, LDI_0, API, 40,
                                  LDI_1, LDI_U1, 0x48, LDI_0, API, 41, STM_S4_C16, 0, 0,
                                  LDI_0, LDI_0, API, 42, STM_S4_C16, 1, 0, RET };
        LOAD(init, cycle);
    }
    fake_twi_rx[0] = 0x99;
    ram[0] = 0xFFFF;
    CHECK(plc_vm_cycle() == PLC_OK);
    CHECK(CALLED("twi_open 0 21 22 100"));
    CHECK(plc_vm_cycle() == PLC_OK);
    CHECK((ram[0] == PLC_TWI_OK) && (ram[1] == 0x99) && (fake_twi_tx[0] == 0x42));
    fake_twi_status = PLC_TWI_NACK;
    CHECK(plc_vm_cycle() == PLC_ERR_PARM);              // TwiGet after a NACK
    CHECK(ram[0] == PLC_TWI_NACK);

    // UART: ram[0] = UartGet(2) on an empty buffer
    setup();
    {
        const uint8_t init[] = { LDI_0, LDI_U1, 23, LDI_U1, 4, API, 31,
                                 LDI_0, LDI_U1, 24, LDI_U1, 5, API, 31, RET };
        const uint8_t cycle[] = { LDI_U1, 2, API, 51, STM_S4_C16, 0, 0, RET };
        LOAD(init, cycle);
    }
    CHECK(plc_vm_cycle() == PLC_OK);
    CHECK((plc_vm_cycle() == PLC_OK) && (ram[0] == 0xFFFFFFFF));

    // Errors stop the program
    setup();
    {
        const uint8_t init[] = { LDI_U1, 5, LDI_U1, 6, API, 30, RET };     // Flash pin
        const uint8_t cycle[] = { RET };
        LOAD(init, cycle);
    }
    CHECK(plc_vm_cycle() == PLC_ERR_PARM);
    setup();
    {
        const uint8_t init[] = { RET };
        const uint8_t cycle[] = { IN, 0, 0, 4, 0, RET };                    // Not configured
        LOAD(init, cycle);
    }
    plc_vm_cycle();
    CHECK(plc_vm_cycle() == PLC_ERR_PARM);

    // Missing arguments: stack error, no side effects (args 0, 0 would release GPIO0)
    setup();
    CFG(0, OUT, 0);
    {
        const uint8_t init[] = { API, 30, RET };
        const uint8_t cycle[] = { RET };
        LOAD(init, cycle);
    }
    fake_clear();
    CHECK(plc_vm_cycle() == PLC_ERR_OFR_SP);
    CHECK(fake_calls[0] == 0);
    CHECK(plc_io_in(0, &v) == PLC_RUN);
}

int main(void)
{
    test_pins();
    test_digital();
    test_analog();
    test_pwm();
    test_counter();
    test_twi();
    test_uart();
    test_restart();
    test_program();

    printf("%d passed, %d failed\n", passed, failed);
    printf(failed ? "FAILED\n" : "ALL PASSED\n");
    return failed ? 1 : 0;
}
