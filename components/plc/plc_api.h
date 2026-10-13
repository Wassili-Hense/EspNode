/*
 * PLC API functions (opcode 0xC6) and the IN/OUT opcodes. Numbers: docs/plc-api.md.
 */
#pragma once

#include <stdint.h>

#include "plc_vm.h"

// API function numbers, shared with the Enviriot compiler (LoadNativeFunctions)
enum {
    PLC_API_NODE_STATUS     = 5,    // int NodeStatus()
    PLC_API_MILLISECONDS    = 6,    // int getMilliseconds()
    PLC_API_SECONDS         = 7,    // int getSeconds()
    PLC_API_RANDOM          = 8,    // int Random()
    PLC_API_CONFIG_PIN1     = 30,   // void ConfigPin1(int pin, int mode)
    PLC_API_CONFIG_PIN2     = 31,   // void ConfigPin2(int pin, int mode, int option)
    PLC_API_TWI_PUT         = 40,   // void TwiPut(int bus, int byte)
    PLC_API_TWI_XFER        = 41,   // int TwiXfer(int bus, int addr, int rlen)
    PLC_API_TWI_GET         = 42,   // int TwiGet(int bus, int idx)
    PLC_API_UART_AVAIL      = 50,   // int UartAvail(int port)
    PLC_API_UART_GET        = 51,   // int UartGet(int port)
    PLC_API_UART_PUT        = 52,   // int UartPut(int port, int byte)
};

// plc_vm_api_t
plc_status_t plc_api_call(uint8_t func);

// plc_vm_in_t / plc_vm_out_t: the channel is the operand base, a GPIO number
plc_status_t plc_api_in(uint32_t operand, uint32_t *value);
plc_status_t plc_api_out(uint32_t operand, uint32_t value);
