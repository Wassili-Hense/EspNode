/*
 * X13 PLC virtual machine: stack machine executing the bytecode of the Enviriot PLC compiler.
 *
 * Not thread-safe, single instance.
 *
 * Based on X13.Home extplc_vm, Copyright (c) 2011-2017 <comparator@gmx.de>,
 * BSD 3-Clause License. See LICENSE.X13 for details.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// VM status, also the status byte of the PLC control protocol
typedef enum {
    PLC_OK                  = 0x00,
    PLC_RUN                 = 0x01,     // Cycle in progress

    PLC_ERR_UNK_COP         = 0x80,     // Unknown operation
    PLC_ERR_OFR_PC          = 0x81,     // Program counter out of range
    PLC_ERR_OFR_RAM         = 0x82,     // RAM address out of range
    PLC_ERR_TEST            = 0x83,     // Test failed
    PLC_ERR_WD              = 0x84,     // Watchdog, cycle too long
    PLC_ERR_OFR_SFP         = 0x85,     // Stack frame pointer out of range
    PLC_ERR_DIV0            = 0x86,     // Division by 0
    PLC_ERR_OFR_SP          = 0x87,     // Stack pointer out of range
    PLC_ERR_UNK_API         = 0x88,     // Unknown API function
    PLC_ERR_RSC_BUSY        = 0x89,     // Resource busy
    PLC_ERR_PARM            = 0x8A,     // Parameter out of range
    PLC_ERR_OFR_IDX         = 0x90,     // Index out of range
    PLC_ERR_STORAGE         = 0xA0,     // Program storage error (X13: EEPROM)
    PLC_ERR_WRS             = 0xFA,     // Wrong state
    PLC_ERR_CRC             = 0xFB,     // CRC error
    PLC_ERR_OFR             = 0xFC,     // Out of range
    PLC_ERR_FMT             = 0xFD,     // Format error
    PLC_ERR_CMD             = 0xFE,     // Unknown command
} plc_status_t;

// API function (opcode 0xC6 <func>): takes arguments with plc_vm_pop() (first argument first),
// returns results with plc_vm_push(). Returns PLC_RUN or an error.
typedef plc_status_t (*plc_vm_api_t)(uint8_t func);

typedef struct {
    const uint8_t  *prg;            // Program
    uint32_t        prg_size;       // Bytes, max 64K (16-bit addresses)
    uint32_t       *ram;            // Data, merkers and stack
    uint32_t        ram_words;
    plc_vm_api_t    api;
} plc_vm_config_t;

// Registers, reported to the gateway when the program stops on an error
typedef struct {
    uint32_t sp;
    uint32_t top;                   // RAM[SP - 1]
    uint32_t sfp;
    uint32_t pc;
} plc_vm_regs_t;

void         plc_vm_init(const plc_vm_config_t *cfg);

// The next cycle starts the program from address 0 (initialisation), later cycles from 4
void         plc_vm_restart(void);

// Run one cycle, returns PLC_OK or an error
plc_status_t plc_vm_cycle(void);

uint32_t     plc_vm_stack_bottom(void);
void         plc_vm_set_stack_bottom(uint32_t words);
void         plc_vm_regs(plc_vm_regs_t *regs);

// For API functions
uint32_t     plc_vm_pop(void);
void         plc_vm_push(uint32_t val);

#ifdef __cplusplus
}
#endif
