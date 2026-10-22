/*
 * MQTT-SN message builders.
 */
#include <string.h>

#include "mqttsn_msg.h"

// Append the variable part, returns total length or 0 on overflow
static size_t put_tail(uint8_t *buf, size_t fixed, const uint8_t *data, size_t len)
{
    if (fixed + len > MQTTSN_MAX_MSG_LEN) {
        return 0;
    }
    if (len > 0) {
        memcpy(&buf[fixed], data, len);
    }
    return fixed + len;
}

static size_t finish(uint8_t *buf, uint8_t type, size_t len)
{
    if (len > 0) {
        buf[0] = (uint8_t)len;
        buf[1] = type;
    }
    return len;
}

size_t mqttsn_msg_searchgw(uint8_t *buf, uint8_t radius)
{
    buf[2] = radius;
    return finish(buf, MQTTSN_SEARCHGW, MQTTSN_SIZEOF_SEARCHGW);
}

size_t mqttsn_msg_gwinfo(uint8_t *buf, uint8_t gw_id)
{
    buf[2] = gw_id;
    return finish(buf, MQTTSN_GWINFO, MQTTSN_SIZEOF_GWINFO);
}

size_t mqttsn_msg_forward(uint8_t *buf, const uint8_t *node_id, size_t id_len, const uint8_t *msg, size_t len)
{
    size_t hdr = MQTTSN_SIZEOF_FORWARD + id_len;
    if ((id_len == 0) || (id_len > MQTTSN_FWD_NODE_ID_MAX) || (hdr + len > MQTTSN_LINK_MAX_LEN)) {
        return 0;
    }
    buf[0] = (uint8_t)hdr;                  // Length of the header only, the message follows
    buf[1] = MQTTSN_FORWARD;
    buf[2] = 0;                             // Ctrl, X13: radius / TTL, unused
    memcpy(&buf[MQTTSN_SIZEOF_FORWARD], node_id, id_len);
    memcpy(&buf[hdr], msg, len);
    return hdr + len;
}

size_t mqttsn_msg_connect(uint8_t *buf, uint8_t flags, uint16_t duration,
                          const uint8_t *client_id, size_t id_len)
{
    buf[2] = flags;
    buf[3] = MQTTSN_PROTOCOL_ID;
    mqttsn_put_u16(&buf[4], duration);
    return finish(buf, MQTTSN_CONNECT, put_tail(buf, MQTTSN_SIZEOF_CONNECT, client_id, id_len));
}

size_t mqttsn_msg_register(uint8_t *buf, uint16_t topic_id, uint16_t msg_id,
                           const uint8_t *name, size_t len)
{
    mqttsn_put_u16(&buf[2], topic_id);
    mqttsn_put_u16(&buf[4], msg_id);
    return finish(buf, MQTTSN_REGISTER, put_tail(buf, MQTTSN_SIZEOF_REGISTER, name, len));
}

size_t mqttsn_msg_regack(uint8_t *buf, uint16_t topic_id, uint16_t msg_id, uint8_t rc)
{
    mqttsn_put_u16(&buf[2], topic_id);
    mqttsn_put_u16(&buf[4], msg_id);
    buf[6] = rc;
    return finish(buf, MQTTSN_REGACK, MQTTSN_SIZEOF_REGACK);
}

size_t mqttsn_msg_publish(uint8_t *buf, uint8_t flags, uint16_t topic_id, uint16_t msg_id,
                          const uint8_t *data, size_t len)
{
    buf[2] = flags;
    mqttsn_put_u16(&buf[3], topic_id);
    mqttsn_put_u16(&buf[5], msg_id);
    return finish(buf, MQTTSN_PUBLISH, put_tail(buf, MQTTSN_SIZEOF_PUBLISH, data, len));
}

size_t mqttsn_msg_puback(uint8_t *buf, uint16_t topic_id, uint16_t msg_id, uint8_t rc)
{
    mqttsn_put_u16(&buf[2], topic_id);
    mqttsn_put_u16(&buf[4], msg_id);
    buf[6] = rc;
    return finish(buf, MQTTSN_PUBACK, MQTTSN_SIZEOF_PUBACK);
}

size_t mqttsn_msg_subscribe(uint8_t *buf, uint8_t flags, uint16_t msg_id,
                            const uint8_t *topic, size_t len)
{
    buf[2] = flags;
    mqttsn_put_u16(&buf[3], msg_id);
    return finish(buf, MQTTSN_SUBSCRIBE, put_tail(buf, MQTTSN_SIZEOF_SUBSCRIBE, topic, len));
}

size_t mqttsn_msg_pingreq(uint8_t *buf)
{
    return finish(buf, MQTTSN_PINGREQ, MQTTSN_SIZEOF_PINGREQ);
}

size_t mqttsn_msg_dhcpreq(uint8_t *buf, uint8_t radius, uint16_t msg_id,
                          const uint8_t *hlen, size_t n)
{
    buf[2] = radius;
    mqttsn_put_u16(&buf[3], msg_id);
    return finish(buf, MQTTSN_DHCPREQ, put_tail(buf, MQTTSN_SIZEOF_DHCPREQ, hlen, n));
}

const char *mqttsn_msg_type_str(uint8_t type)
{
    switch (type) {
    case MQTTSN_ADVERTISE:      return "ADVERTISE";
    case MQTTSN_SEARCHGW:       return "SEARCHGW";
    case MQTTSN_GWINFO:         return "GWINFO";
    case MQTTSN_CONNECT:        return "CONNECT";
    case MQTTSN_CONNACK:        return "CONNACK";
    case MQTTSN_REGISTER:       return "REGISTER";
    case MQTTSN_REGACK:         return "REGACK";
    case MQTTSN_PUBLISH:        return "PUBLISH";
    case MQTTSN_PUBACK:         return "PUBACK";
    case MQTTSN_SUBSCRIBE:      return "SUBSCRIBE";
    case MQTTSN_SUBACK:         return "SUBACK";
    case MQTTSN_PINGREQ:        return "PINGREQ";
    case MQTTSN_PINGRESP:       return "PINGRESP";
    case MQTTSN_DISCONNECT:     return "DISCONNECT";
    case MQTTSN_DHCPREQ:        return "DHCPREQ";
    case MQTTSN_DHCPRESP:       return "DHCPRESP";
    case MQTTSN_FORWARD:        return "FORWARD";
    default:                    return "?";
    }
}
