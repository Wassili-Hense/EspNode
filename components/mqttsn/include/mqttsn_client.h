/*
 * MQTT-SN client (node role) with X13 extensions (DHCP address assignment).
 *
 * Not thread-safe: all functions must be called from the same task.
 *
 * Based on X13.Home device firmware, Copyright (c) 2011-2017 <comparator@gmx.de>,
 * BSD 3-Clause License. See LICENSE.X13 for details.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mqttsn_defs.h"
#include "mqttsn_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MQTTSN_STATUS_DISCONNECTED = 0,
    MQTTSN_STATUS_DHCP,             // Requesting own address
    MQTTSN_STATUS_SEARCHGW,         // Searching gateway
    MQTTSN_STATUS_OFFLINE,          // Gateway known, connecting
    MQTTSN_STATUS_PRE_CONNECT,      // Connected, restoring session (register/publish info/subscribe)
    MQTTSN_STATUS_CONNECT,          // Session established
} mqttsn_status_t;

typedef struct {
    // REGISTER from gateway; returns MQTTSN_RC_*
    uint8_t (*on_register)(uint16_t topic_id, const uint8_t *name, size_t len);
    // REGACK for own REGISTER; topic_id == MQTTSN_TOPIC_ID_INVALID if rejected
    void    (*on_regack)(uint16_t topic_id);
    // PUBLISH from gateway; returns MQTTSN_RC_*
    uint8_t (*on_publish)(uint16_t topic_id, uint8_t flags, const uint8_t *data, size_t len);
    // Fill ClientId (not 0-terminated), returns length, max MQTTSN_CLIENT_ID_MAX
    size_t  (*get_client_id)(uint8_t *buf);
    // Load own and gateway addresses (undef if not configured)
    void    (*load_addrs)(mqttsn_addr_t *node, mqttsn_addr_t *gateway);
    // Store own address assigned by DHCP
    void    (*store_node_addr)(const mqttsn_addr_t *node);
    // Optional, for a gateway of nodes on further links (mqttsn_gateway), while connected:
    // FORWARD from the gateway, the message for a node behind this one (or a FORWARD for a node further away)
    void    (*on_forward)(const uint8_t *node_id, size_t id_len, const uint8_t *msg, size_t len);
    // ADVERTISE of the own gateway
    void    (*on_advertise)(const uint8_t *msg, size_t len);
} mqttsn_client_cb_t;

typedef struct {
    const mqttsn_transport_t   *transport;
    mqttsn_client_cb_t          cb;
    int64_t                   (*now_ms)(void);      // Monotonic time
    uint16_t                  (*rand16)(void);      // Random number generator
} mqttsn_client_config_t;

void mqttsn_client_init(const mqttsn_client_config_t *cfg);

// Periodic poll, call every 10 ms
void mqttsn_client_tick(void);

// Incoming message, msg[0] is the Length field.
// src NULL: point-to-point link without addresses, the message is attributed to the node's
// own current address (X13 convention, the gateway address learned from GWINFO is the same).
void mqttsn_client_handle_rx(const mqttsn_addr_t *src, const uint8_t *msg, size_t len);

mqttsn_status_t mqttsn_client_status(void);
const char *mqttsn_client_status_str(mqttsn_status_t status);

// True if no request is in flight and a new one may be queued
bool mqttsn_client_can_send(void);

// Queue a request, the client sends it and handles retries.
// Return false if another request is in flight or the arguments don't fit into a message.
bool mqttsn_client_register(uint16_t topic_id, const uint8_t *name, size_t len);
bool mqttsn_client_publish(uint8_t flags, uint16_t topic_id, const uint8_t *data, size_t len);
bool mqttsn_client_subscribe(uint8_t flags, const uint8_t *topic, size_t len);

// The message of a node behind this one to the gateway, in FORWARD; not repeated, the node repeats
// itself. Only while connected; false otherwise or if it does not fit.
bool mqttsn_client_forward(const uint8_t *node_id, size_t id_len, const uint8_t *msg, size_t len);

// The gateway: its id for GWINFO and the radius it was found with
uint8_t mqttsn_client_gw_id(void);
uint8_t mqttsn_client_radius(void);

#ifdef __cplusplus
}
#endif
