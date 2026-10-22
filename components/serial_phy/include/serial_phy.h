/*
 * MQTT-SN transport over UART, optional RS485 half-duplex. One instance per UART port: the link of the
 * node to its gateway, and links of a gateway to X13 nodes (mqttsn_gateway).
 *
 * Framing: X13 serial (SLIP-like, point-to-point, no addresses) or X13 RS-485 (an addressed bus, frames
 * with the addresses of the receiver and the sender, 1 byte; 0 - all, 0xFF - a node without one).
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

#define SERIAL_PHY_ADDR_ALL         0x00    // RS-485: broadcast
#define SERIAL_PHY_ADDR_UNDEF       0xFF    // RS-485: no address yet (DHCP)

typedef enum {
    SERIAL_PHY_SLIP = 0,        // X13 serial
    SERIAL_PHY_RS485,           // X13 RS-485, F0 1C
} serial_phy_framing_t;

typedef struct {
    int uart_port;
    int tx_pin;
    int rx_pin;
    int de_pin;         // RS485 driver enable (RTS), -1 - plain UART
    int baud_rate;
    serial_phy_framing_t framing;
    uint8_t addr;       // RS-485: own address on the bus
} serial_phy_config_t;

typedef struct serial_phy serial_phy_t;

// Called from the RX task of the instance for every received message (msg[0] is the Length field).
// src - the sender on RS-485; NULL on a serial link, it is point-to-point: pass it to mqttsn_client_handle_rx()
typedef void (*serial_phy_rx_cb_t)(const mqttsn_addr_t *src, const uint8_t *msg, size_t len, void *arg);

esp_err_t serial_phy_init(const serial_phy_config_t *cfg, serial_phy_rx_cb_t rx_cb, void *arg, serial_phy_t **phy);

// Send one message (or a FORWARD with one) to node dst, on a serial link dst is not used. RS-485 waits for
// the bus to be free. Must be called from a single task.
bool serial_phy_send(serial_phy_t *phy, uint8_t dst, const uint8_t *msg, size_t len);

// RS-485: own address on the bus, the client of the node sets it with the transport
void serial_phy_set_addr(serial_phy_t *phy, uint8_t addr);

// Transport for mqttsn_client
const mqttsn_transport_t *serial_phy_transport(serial_phy_t *phy);

#ifdef __cplusplus
}
#endif
