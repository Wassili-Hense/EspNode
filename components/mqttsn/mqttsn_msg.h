/*
 * MQTT-SN message builders. Messages are plain byte arrays, buf[0] is the Length field,
 * buf[1] is the MsgType; multi-byte fields are big-endian.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mqttsn_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline uint16_t mqttsn_get_u16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void mqttsn_put_u16(uint8_t *p, uint16_t val)
{
    p[0] = (uint8_t)(val >> 8);
    p[1] = (uint8_t)(val & 0xFF);
}

// Builders write a complete message into buf (MQTTSN_MAX_MSG_LEN bytes) and return its length,
// 0 if the variable part doesn't fit.
size_t mqttsn_msg_searchgw(uint8_t *buf, uint8_t radius);
size_t mqttsn_msg_gwinfo(uint8_t *buf, uint8_t gw_id);
// The message of a node behind this one in FORWARD; buf of MQTTSN_LINK_MAX_LEN bytes
size_t mqttsn_msg_forward(uint8_t *buf, const uint8_t *node_id, size_t id_len, const uint8_t *msg, size_t len);
size_t mqttsn_msg_connect(uint8_t *buf, uint8_t flags, uint16_t duration,
                          const uint8_t *client_id, size_t id_len);
size_t mqttsn_msg_register(uint8_t *buf, uint16_t topic_id, uint16_t msg_id,
                           const uint8_t *name, size_t len);
size_t mqttsn_msg_regack(uint8_t *buf, uint16_t topic_id, uint16_t msg_id, uint8_t rc);
size_t mqttsn_msg_publish(uint8_t *buf, uint8_t flags, uint16_t topic_id, uint16_t msg_id,
                          const uint8_t *data, size_t len);
size_t mqttsn_msg_puback(uint8_t *buf, uint16_t topic_id, uint16_t msg_id, uint8_t rc);
size_t mqttsn_msg_subscribe(uint8_t *buf, uint8_t flags, uint16_t msg_id,
                            const uint8_t *topic, size_t len);
size_t mqttsn_msg_pingreq(uint8_t *buf);
// X13 DHCP request, hlen[] - lengths of requested addresses
size_t mqttsn_msg_dhcpreq(uint8_t *buf, uint8_t radius, uint16_t msg_id,
                          const uint8_t *hlen, size_t n);

const char *mqttsn_msg_type_str(uint8_t type);

#ifdef __cplusplus
}
#endif
