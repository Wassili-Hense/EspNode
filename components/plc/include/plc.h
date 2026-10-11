/*
 * X13 PLC: runs programs compiled by Enviriot.
 *
 * Objects: "pa0" - control (program upload, start/stop, error reports), "M<type><n>" - merkers,
 * variables of the program shared with the gateway.
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

// Load the program and settings, requires nvs_flash_init(). Call before od_init().
esp_err_t plc_init(void);

// Object Dictionary provider: merkers, control object, PLC cycle on od_poll()
const od_provider_t *plc_provider(void);

#ifdef __cplusplus
}
#endif
