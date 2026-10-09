/*
 * MQTT-SN transport over UART (X13 serial framing), optional RS485 half-duplex.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mqttsn_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int uart_port;
    int tx_pin;
    int rx_pin;
    int de_pin;         // RS485 driver enable (RTS), -1 - plain UART
    int baud_rate;
} serial_phy_config_t;

// Called from the RX task for every received message (msg[0] is the Length field).
// The link is point-to-point without addresses: pass src = NULL to mqttsn_client_handle_rx().
typedef void (*serial_phy_rx_cb_t)(const uint8_t *msg, size_t len, void *arg);

esp_err_t serial_phy_init(const serial_phy_config_t *cfg, serial_phy_rx_cb_t rx_cb, void *arg);

// Transport for mqttsn_client; send() must be called from a single task
const mqttsn_transport_t *serial_phy_transport(void);

#ifdef __cplusplus
}
#endif
