/*
 * Gateway for X13 nodes on the further links of this node (X13 PHY2..4, the first link is PHY1): their
 * messages go to the gateway of this node in FORWARD, the answers come back unwrapped. Works only while
 * the own session of mqttsn_client is connected, as in X13.
 *
 * The links are point-to-point (UART with SLIP, one node): the node is addressed as [PHY, 1].
 * Not thread-safe: call from the task of the MQTT-SN client.
 *
 * Based on X13.Home device firmware (mqttsn_parser_phyS), Copyright (c) 2011-2017 <comparator@gmx.de>,
 * BSD 3-Clause License. See LICENSE.X13 for details.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MQTTSN_GATEWAY_PHY_MIN      2
#define MQTTSN_GATEWAY_PHY_MAX      4

typedef struct {
    // Send a message to the node on link phy
    bool    (*send)(uint8_t phy, const uint8_t *msg, size_t len);
    uint8_t phys;                   // Gateway links, bit n - PHY n
} mqttsn_gateway_config_t;

void mqttsn_gateway_init(const mqttsn_gateway_config_t *cfg);

// Message from the node on link phy (msg[0] is the Length field)
void mqttsn_gateway_handle_rx(uint8_t phy, const uint8_t *msg, size_t len);

// mqttsn_client callbacks on_forward and on_advertise
void mqttsn_gateway_on_forward(const uint8_t *node_id, size_t id_len, const uint8_t *msg, size_t len);
void mqttsn_gateway_on_advertise(const uint8_t *msg, size_t len);

#ifdef __cplusplus
}
#endif
