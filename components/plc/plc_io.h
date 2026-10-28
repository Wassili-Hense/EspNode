/*
 * PLC IO: pins configured by the program (ConfigPin in the init block), read and written in the
 * cycle with the IN/OUT opcodes, buses by the TWI/UART API functions. A channel is a GPIO number.
 *
 * Not thread-safe: runs in the PLC task. Mode numbers are part of the contract with Enviriot,
 * see docs/plc-api.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "plc_vm.h"

enum {
    PLC_PIN_OFF         = 0,    // Release the pin
    PLC_PIN_IN          = 1,    // Digital input; option - debounce, ms
    PLC_PIN_IN_PU       = 2,    // ... with pull-up
    PLC_PIN_IN_PD       = 3,    // ... with pull-down
    PLC_PIN_IN_INV_PU   = 4,    // Inverted input with pull-up: contact to ground reads 1
    PLC_PIN_OUT         = 5,    // Digital output, push-pull
    PLC_PIN_OUT_INV     = 6,    // ... inverted
    PLC_PIN_OUT_OD      = 7,    // ... open drain
    PLC_PIN_OUT_OD_INV  = 8,    // ... open drain, inverted
    PLC_PIN_ANALOG      = 9,    // ADC1, ADC2 without WiFi; mV; option - attenuation 0..3 (0 / 2.5 / 6 / 12 dB)
    PLC_PIN_DAC         = 10,   // DAC, 0..255
    PLC_PIN_PWM         = 11,   // PWM, duty 0..65535; option - frequency, Hz (0 - 1000)
    PLC_PIN_PWM_INV     = 12,   // ... inverted
    PLC_PIN_CNT_RISE    = 13,   // Pulse counter, rising edges; option - glitch filter, ns
    PLC_PIN_CNT_FALL    = 14,   // ... falling edges
    PLC_PIN_CNT_BOTH    = 15,   // ... both edges
    PLC_PIN_TWI0_SDA    = 16,   // TWI bus 0
    PLC_PIN_TWI0_SCL    = 17,   // ... option - clock, kHz (0 - 100)
    PLC_PIN_TWI1_SDA    = 18,   // TWI bus 1
    PLC_PIN_TWI1_SCL    = 19,
    PLC_PIN_UART1_TX    = 20,   // UART1; option - baud rate (0 - 9600)
    PLC_PIN_UART1_RX    = 21,   // ... option - format: bits 0-1 parity (0 none, 1 even, 2 odd), bit 2 two stop bits
    PLC_PIN_UART1_DE    = 22,   // ... RS-485 driver enable
    PLC_PIN_UART2_TX    = 23,   // UART2
    PLC_PIN_UART2_RX    = 24,
    PLC_PIN_UART2_DE    = 25,
    PLC_PIN_MODES
};

typedef struct {
    uint64_t reserved_pins;     // Pins the program must not use, bit n - GPIOn
    uint8_t  reserved_uarts;    // UART ports the program must not use, bit n - UARTn
    bool     adc2;              // ADC2 pins may be analog inputs: no WiFi, its driver takes ADC2
} plc_io_config_t;

void         plc_io_init(const plc_io_config_t *cfg);

// Program (re)start: pins configured again with the same mode and option keep running untouched,
// pins not configured again by the end of the first (init) cycle are released
void         plc_io_restart_begin(void);
void         plc_io_restart_end(void);

plc_status_t plc_io_config(uint32_t pin, uint32_t mode, int32_t option);

// IN / OUT opcodes
plc_status_t plc_io_in(uint32_t pin, uint32_t *value);
plc_status_t plc_io_out(uint32_t pin, uint32_t value);

// TWI as in X13 (API 1-4 for bus 0, 40-43 with the bus number). Control word and status:
// address (7 bit) | access << 8 | write << 16 | read << 24. TwiControl starts a transaction (ignored
// while one is open), TwiPutByte supplies the bytes to write - the transfer starts with the last one,
// TwiStatus reports the progress (0 - no transaction), TwiGetByte returns the bytes read.
#define PLC_TWI_WRITE       0x01        // Access flags
#define PLC_TWI_READ        0x02
#define PLC_TWI_BUSY        0x08        // Transfer running
#define PLC_TWI_RDY         0x10        // Done, read the bytes
#define PLC_TWI_WD          0x20        // Timeout
#define PLC_TWI_SLANACK     0x40        // No answer
#define PLC_TWI_ERROR       0x80        // Bus error
#define PLC_TWI_TIMEOUT_MS  250         // From TwiControl to the end of the transfer

plc_status_t plc_io_twi_control(uint32_t bus, uint32_t ctrl);
plc_status_t plc_io_twi_status(uint32_t bus, uint32_t *status);
plc_status_t plc_io_twi_put(uint32_t bus, uint32_t byte);
plc_status_t plc_io_twi_get(uint32_t bus, uint32_t *byte);

// UART as in X13 (API 21-23), the port is the UART number. Bytes to read are reported after a pause:
// when nothing new arrived since the previous call (end of a frame) or the buffer is half full.
plc_status_t plc_io_uart_avail(uint32_t port, uint32_t *count);
plc_status_t plc_io_uart_get(uint32_t port, uint32_t *value);      // 0xFFFFFFFF (-1) - empty
plc_status_t plc_io_uart_put(uint32_t port, uint32_t byte, uint32_t *ok);
