// Fake plc_io_hw.h for host tests: ESP32 pin capabilities, pin levels, a log of the calls that
// change the hardware, failure injection
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "plc_io_hw.h"

#define FAKE_UART_BUF   64

extern int64_t  fake_now_ms;                        // plc_hw_now_ms()
extern bool     fake_level[PLC_HW_PINS];            // Inputs read it, outputs write it
extern int32_t  fake_mv[PLC_HW_PINS];               // ADC
extern uint8_t  fake_dac[PLC_HW_PINS];
extern uint32_t fake_duty[PLC_HW_PWM_CHANNELS];
extern int32_t  fake_count[PLC_HW_COUNTERS];

extern int      fake_twi_status;                    // Result of the next transfers
extern uint8_t  fake_twi_rx[32];                    // Data the device sends
extern uint8_t  fake_twi_tx[32];                    // Data the last transfer wrote
extern size_t   fake_twi_tx_len;

extern uint8_t  fake_uart_rx[PLC_HW_UART_PORTS][FAKE_UART_BUF];     // Received, not read yet
extern size_t   fake_uart_rx_len[PLC_HW_UART_PORTS];
extern uint8_t  fake_uart_tx[PLC_HW_UART_PORTS][FAKE_UART_BUF];     // Sent
extern size_t   fake_uart_tx_len[PLC_HW_UART_PORTS];
extern size_t   fake_uart_tx_room;                  // Bytes plc_hw_uart_put() still accepts

extern const char *fake_fail;                       // Call name to fail, e.g. "pwm_timer"; NULL - none

// Hardware-changing calls since fake_clear(), "name args\n" each, e.g. "gpio_output 2 0 0"
extern char     fake_calls[16384];

void fake_reset(void);                              // Everything to power-on state
void fake_clear(void);                              // Clear the call log
bool fake_called(const char *call);                 // The log has a line starting with call
