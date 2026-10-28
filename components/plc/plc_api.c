/*
 * PLC API functions.
 */
#include <stdbool.h>

#include "esp_random.h"
#include "esp_timer.h"

#include "mqttsn_client.h"
#include "objdict.h"
#include "plc_api.h"
#include "plc_io.h"
#include "plc_stream.h"

#define pop     plc_vm_pop
#define push    plc_vm_push

// Pop n arguments, first to last; false - stack underflow, the VM has stopped already
static bool args(uint32_t *arg, int n)
{
    for (int i = 0; i < n; i++) {
        arg[i] = pop();
    }
    return plc_vm_status() == PLC_RUN;
}

// Push the result of a function that succeeded. The value by pointer: read after the call that
// writes it, the order of evaluating arguments is unspecified (gcc on x86-64 goes right to left)
static plc_status_t result(plc_status_t stat, const uint32_t *val)
{
    if (stat == PLC_RUN) {
        push(*val);
    }
    return stat;
}

// TWI functions 1-4 on bus 0, or with the bus as the first argument
static plc_status_t twi_call(uint8_t func, bool with_bus)
{
    uint32_t a[2] = { 0, 0 };
    bool has_arg = (func == PLC_API_TWI_CONTROL) || (func == PLC_API_TWI_PUT_BYTE);
    int n = (with_bus ? 1 : 0) + (has_arg ? 1 : 0);
    if (!args(a, n)) {
        return PLC_ERR_OFR_SP;
    }
    uint32_t bus = with_bus ? a[0] : 0;
    uint32_t arg = with_bus ? a[1] : a[0];
    uint32_t val = 0;

    switch (func) {
    case PLC_API_TWI_CONTROL:
        return plc_io_twi_control(bus, arg);
    case PLC_API_TWI_STATUS:
        return result(plc_io_twi_status(bus, &val), &val);
    case PLC_API_TWI_PUT_BYTE:
        return plc_io_twi_put(bus, arg);
    default:
        return result(plc_io_twi_get(bus, &val), &val);
    }
}

plc_status_t plc_api_call(uint8_t func)
{
    uint32_t a[3];
    uint32_t val = 0;

    switch (func) {
    case PLC_API_TWI_CONTROL:
    case PLC_API_TWI_STATUS:
    case PLC_API_TWI_PUT_BYTE:
    case PLC_API_TWI_GET_BYTE:
        return twi_call(func, false);
    case PLC_API_TWI_BUS + PLC_API_TWI_CONTROL:
    case PLC_API_TWI_BUS + PLC_API_TWI_STATUS:
    case PLC_API_TWI_BUS + PLC_API_TWI_PUT_BYTE:
    case PLC_API_TWI_BUS + PLC_API_TWI_GET_BYTE:
        return twi_call(func - PLC_API_TWI_BUS, true);

    case PLC_API_NODE_STATUS:
        push(mqttsn_client_status() == MQTTSN_STATUS_CONNECT);
        return PLC_RUN;
    case PLC_API_MILLISECONDS:
        push((uint32_t)(esp_timer_get_time() / 1000));
        return PLC_RUN;
    case PLC_API_SECONDS:
        push((uint32_t)(esp_timer_get_time() / 1000000));
        return PLC_RUN;
    case PLC_API_RANDOM:
        push(esp_random());
        return PLC_RUN;
    case PLC_API_NOW_SECONDS:
    case PLC_API_TODAY: {
        od_clock_t c;
        od_clock_now(&c);
        if (func == PLC_API_NOW_SECONDS) {
            push((uint32_t)c.hour * 3600 + (uint32_t)c.min * 60 + c.sec);
        } else {            // Weekday 1 - Monday .. 7 - Sunday
            push(((uint32_t)(c.year % 100) << 24) | ((uint32_t)c.month << 16) | ((uint32_t)c.day << 8) | c.wday);
        }
        return PLC_RUN;
    }

    case PLC_API_UART_AVAIL:
        if (!args(a, 1)) {
            return PLC_ERR_OFR_SP;
        }
        return result(plc_io_uart_avail(a[0], &val), &val);
    case PLC_API_UART_GET:
        if (!args(a, 1)) {
            return PLC_ERR_OFR_SP;
        }
        return result(plc_io_uart_get(a[0], &val), &val);
    case PLC_API_SEND_BYTE:
        if (!args(a, 2)) {
            return PLC_ERR_OFR_SP;
        }
        return result(plc_stream_byte(a[0], a[1], &val), &val);
    case PLC_API_PRINT_INT:
        if (!args(a, 2)) {
            return PLC_ERR_OFR_SP;
        }
        return result(plc_stream_int(a[0], a[1], &val), &val);
    case PLC_API_PRINT_FLOAT:
        if (!args(a, 3)) {
            return PLC_ERR_OFR_SP;
        }
        return result(plc_stream_float(a[0], a[1], a[2], &val), &val);

    case PLC_API_CONFIG_PIN:
        if (!args(a, 3)) {
            return PLC_ERR_OFR_SP;
        }
        return plc_io_config(a[0], a[1], (int32_t)a[2]);

    default:
        return PLC_ERR_UNK_API;
    }
}

plc_status_t plc_api_in(uint32_t operand, uint32_t *value)
{
    return plc_io_in(operand >> 16, value);
}

plc_status_t plc_api_out(uint32_t operand, uint32_t value)
{
    return plc_io_out(operand >> 16, value);
}
