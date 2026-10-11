/*
 * PLC API functions.
 */
#include "esp_random.h"
#include "esp_timer.h"

#include "mqttsn_client.h"
#include "plc_api.h"

plc_status_t plc_api_call(uint8_t func)
{
    switch (func) {
    case PLC_API_NODE_STATUS:
        plc_vm_push(mqttsn_client_status() == MQTTSN_STATUS_CONNECT);
        return PLC_RUN;
    case PLC_API_MILLISECONDS:
        plc_vm_push((uint32_t)(esp_timer_get_time() / 1000));
        return PLC_RUN;
    case PLC_API_SECONDS:
        plc_vm_push((uint32_t)(esp_timer_get_time() / 1000000));
        return PLC_RUN;
    case PLC_API_RANDOM:
        plc_vm_push(esp_random());
        return PLC_RUN;
    default:
        return PLC_ERR_UNK_API;
    }
}
