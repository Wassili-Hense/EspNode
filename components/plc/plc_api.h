/*
 * PLC API functions (opcode 0xC6) and the IN/OUT opcodes. Numbers: docs/plc-api.md.
 */
#pragma once

#include <stdint.h>

#include "plc_vm.h"

// API function numbers, shared with the Enviriot compiler (LoadNativeFunctions)
enum {
    PLC_API_TWI_CONTROL     = 1,    // void TwiControl(int ctrl), bus 0, as X13
    PLC_API_TWI_STATUS      = 2,    // int TwiStatus()
    PLC_API_TWI_PUT_BYTE    = 3,    // void TwiPutByte(int data)
    PLC_API_TWI_GET_BYTE    = 4,    // int TwiGetByte()
    PLC_API_NODE_STATUS     = 5,    // int NodeStatus()
    PLC_API_MILLISECONDS    = 6,    // int getMilliseconds()
    PLC_API_SECONDS         = 7,    // int getSeconds()
    PLC_API_RANDOM          = 8,    // int Random()
    PLC_API_UART_AVAIL      = 21,   // int UartBytesToRead(int port), as X13
    PLC_API_UART_GET        = 22,   // int UartGetByte(int port)
    PLC_API_UART_PUT        = 23,   // bool UartPutByte(int port, int data)
    PLC_API_CONFIG_PIN1     = 30,   // void ConfigPin1(int pin, int mode)
    PLC_API_CONFIG_PIN2     = 31,   // void ConfigPin2(int pin, int mode, int option)
    PLC_API_TWI_BUS         = 39,   // 40-43: TWI functions 1-4 with the bus as the first argument
};

// plc_vm_api_t
plc_status_t plc_api_call(uint8_t func);

// plc_vm_in_t / plc_vm_out_t: the channel is the operand base, a GPIO number
plc_status_t plc_api_in(uint32_t operand, uint32_t *value);
plc_status_t plc_api_out(uint32_t operand, uint32_t value);
