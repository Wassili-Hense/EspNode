/*
 * Hardware layer of the PLC IO: plc_io.c keeps the logic (modes, resources, restarts) and calls
 * these functions; plc_io_esp32.c implements them, host tests use a fake.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PLC_HW_PINS         40          // GPIO0..39
#define PLC_HW_PWM_CHANNELS 8
#define PLC_HW_PWM_TIMERS   4
#define PLC_HW_COUNTERS     8
#define PLC_HW_TWI_BUSES    2
#define PLC_HW_UART_PORTS   3           // Port numbers 0..2
#define PLC_HW_UART_RX_SIZE 512         // Receive buffer, bytes
#define PLC_HW_TWI_SIZE     32          // Max bytes to write / to read in one transfer

// Pin capabilities
#define PLC_HW_CAP_VALID    0x01
#define PLC_HW_CAP_OUTPUT   0x02        // Also internal pull-up / pull-down available
#define PLC_HW_CAP_ADC      0x04        // ADC1
#define PLC_HW_CAP_DAC      0x08
#define PLC_HW_CAP_ADC2     0x10        // ADC2: the WiFi driver takes it, plc_io decides

// TWI transfer result
#define PLC_HW_TWI_BUSY     (-1)        // Still running
#define PLC_HW_TWI_OK       0
#define PLC_HW_TWI_NACK     1
#define PLC_HW_TWI_TIMEOUT  2
#define PLC_HW_TWI_ERROR    3

uint32_t plc_hw_caps(uint32_t pin);
int64_t  plc_hw_now_ms(void);

// Return the pin to its reset state
void     plc_hw_release(uint32_t pin);

// GPIO; pull: 0 - none, 1 - up, 2 - down
bool     plc_hw_gpio_input(uint32_t pin, int pull);
bool     plc_hw_gpio_output(uint32_t pin, bool open_drain, bool level);
bool     plc_hw_gpio_get(uint32_t pin);
void     plc_hw_gpio_set(uint32_t pin, bool level);

// ADC1 or ADC2 by the pin, atten 0..3; reads millivolts
bool     plc_hw_adc_open(uint32_t pin, int atten);
bool     plc_hw_adc_read(uint32_t pin, int atten, int32_t *mv);

bool     plc_hw_dac_open(uint32_t pin);
void     plc_hw_dac_write(uint32_t pin, uint8_t value);
void     plc_hw_dac_close(uint32_t pin);

// PWM: timer per frequency, channel per pin; duty 0..65535
bool     plc_hw_pwm_timer(int timer, uint32_t freq);
bool     plc_hw_pwm_open(int channel, int timer, uint32_t pin, bool invert);
void     plc_hw_pwm_write(int channel, int timer, uint32_t duty);
void     plc_hw_pwm_close(int channel);

// Pulse counter; edges: 0 - rising, 1 - falling, 2 - both
bool     plc_hw_cnt_open(int unit, uint32_t pin, int edges, uint32_t filter_ns);
int32_t  plc_hw_cnt_read(int unit);
void     plc_hw_cnt_close(int unit);

// TWI master. A transfer (write tx, repeated START, read rx_len bytes; tx_len == 0 && rx_len == 0 -
// probe) runs in the background: start returns false while the previous one is still running, poll
// returns PLC_HW_TWI_BUSY until it ends, then its result once, with rx_len bytes in rx on success.
bool     plc_hw_twi_open(int bus, uint32_t sda, uint32_t scl, uint32_t khz);
bool     plc_hw_twi_start(int bus, uint8_t addr, const uint8_t *tx, size_t tx_len, size_t rx_len);
int      plc_hw_twi_poll(int bus, uint8_t *rx);
void     plc_hw_twi_close(int bus);     // Waits for a running transfer

// UART; de < 0 - no RS-485; format: bits 0-1 parity (0 none, 1 even, 2 odd), bit 2 - two stop bits
bool     plc_hw_uart_open(int port, uint32_t tx, uint32_t rx, int de, uint32_t baud, uint32_t format);
size_t   plc_hw_uart_avail(int port);
int      plc_hw_uart_get(int port);
size_t   plc_hw_uart_write(int port, const uint8_t *buf, size_t len);   // Into the FIFO as far as it fits, no wait
void     plc_hw_uart_close(int port);
