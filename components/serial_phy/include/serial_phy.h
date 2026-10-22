/*
 * MQTT-SN transport over UART (X13 serial framing), optional RS485 half-duplex. One instance per UART port:
 * the link of the node to its gateway, and links of a gateway to X13 nodes (mqttsn_gateway).
 */
#pragma once

#include <stdbool.h>
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

typedef struct serial_phy serial_phy_t;

// Called from the RX task of the instance for every received message (msg[0] is the Length field).
// The link is point-to-point without addresses: pass src = NULL to mqttsn_client_handle_rx().
typedef void (*serial_phy_rx_cb_t)(const uint8_t *msg, size_t len, void *arg);

esp_err_t serial_phy_init(const serial_phy_config_t *cfg, serial_phy_rx_cb_t rx_cb, void *arg, serial_phy_t **phy);

// Send one message (or a FORWARD with one); must be called from a single task
bool serial_phy_send(serial_phy_t *phy, const uint8_t *msg, size_t len);

// Transport for mqttsn_client
const mqttsn_transport_t *serial_phy_transport(serial_phy_t *phy);

#ifdef __cplusplus
}
#endif
