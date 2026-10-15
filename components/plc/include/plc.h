/*
 * X13 PLC: runs programs compiled by Enviriot.
 *
 * Objects: "pa1" - control (program upload, start/stop, error reports), "M<type><n>" - merkers,
 * variables of the program shared with the gateway. Pins and buses are set up by the program,
 * see docs/plc-api.md.
 *
 * Not thread-safe: runs in the task of the MQTT-SN client and the Object Dictionary.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "objdict.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t reserved_pins;     // GPIOs the program must not use (flash, console, MQTT-SN link), bit n - GPIOn
    uint8_t  reserved_uarts;    // UART ports the program must not use, bit n - UARTn
} plc_config_t;

// Load the program and settings, requires nvs_flash_init(). Call before od_init().
esp_err_t plc_init(const plc_config_t *cfg);

// Object Dictionary provider: merkers, control object, PLC cycle on od_poll()
const od_provider_t *plc_provider(void);

#ifdef __cplusplus
}
#endif
