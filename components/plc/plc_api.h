/*
 * PLC API functions (opcode 0xC6). Numbers: docs/plc-api.md.
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
};

// plc_vm_api_t
plc_status_t plc_api_call(uint8_t func);
