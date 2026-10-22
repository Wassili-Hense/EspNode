/*
 * Gateway for X13 nodes on the further links of this node.
 *
 * Based on X13.Home device firmware (MQTT-SN Library v3.2.0), Copyright (c) 2011-2017
 * <comparator@gmx.de>, BSD 3-Clause License. See LICENSE.X13 for details.
 */
#include "esp_log.h"

#include "mqttsn_client.h"
#include "mqttsn_gateway.h"
#include "mqttsn_msg.h"

static const char *TAG = "mqttsn_gw";

static mqttsn_gateway_config_t s;

static bool is_gateway_phy(uint8_t phy)
{
    return (phy >= MQTTSN_GATEWAY_PHY_MIN) && (phy <= MQTTSN_GATEWAY_PHY_MAX) && (s.phys & (1u << phy));
}

void mqttsn_gateway_init(const mqttsn_gateway_config_t *cfg)
{
    s = *cfg;
}

void mqttsn_gateway_handle_rx(uint8_t phy, uint8_t src, const uint8_t *msg, size_t len)
{
    if (!is_gateway_phy(phy) || (len < 2) || (msg[0] < 2) || (msg[0] > len)) {
        return;
    }
    if (mqttsn_client_status() != MQTTSN_STATUS_CONNECT) {
        return;                         // X13: nodes behind this one wait for its session
    }

    uint8_t type = msg[1];
    switch (type) {
    case MQTTSN_SEARCHGW:
        // As X13: answered here, for the radius this node has to its gateway, to every node on the link
        if ((msg[0] >= MQTTSN_SIZEOF_SEARCHGW) && ((msg[2] == 0) || (msg[2] == mqttsn_client_radius()))) {
            uint8_t info[MQTTSN_SIZEOF_GWINFO];
            s.send(phy, MQTTSN_GATEWAY_ADDR_ALL, info, mqttsn_msg_gwinfo(info, mqttsn_client_gw_id()));
        }
        break;

    case MQTTSN_CONNECT:
    case MQTTSN_PINGREQ:
    case MQTTSN_REGISTER:
    case MQTTSN_REGACK:
    case MQTTSN_PUBLISH:
    case MQTTSN_PUBACK:
    case MQTTSN_SUBSCRIBE:
    case MQTTSN_DISCONNECT:
    case MQTTSN_DHCPREQ:
    case MQTTSN_FORWARD: {
        const uint8_t node_id[] = { phy, src };
        // A FORWARD of a node further away is longer than its Length field
        size_t n = (type == MQTTSN_FORWARD) ? len : msg[0];
        if (!mqttsn_client_forward(node_id, sizeof(node_id), msg, n)) {
            ESP_LOGW(TAG, "PHY%u: %s does not fit into FORWARD", phy, mqttsn_msg_type_str(type));
        }
        break;
    }

    default:                            // ADVERTISE and GWINFO of others, nothing to pass on
        break;
    }
}

void mqttsn_gateway_on_forward(const uint8_t *node_id, size_t id_len, const uint8_t *msg, size_t len)
{
    if ((id_len == 2) && is_gateway_phy(node_id[0])) {
        s.send(node_id[0], node_id[1], msg, len);
    }
}

void mqttsn_gateway_on_advertise(const uint8_t *msg, size_t len)
{
    for (uint8_t phy = MQTTSN_GATEWAY_PHY_MIN; phy <= MQTTSN_GATEWAY_PHY_MAX; phy++) {
        if (is_gateway_phy(phy)) {
            s.send(phy, MQTTSN_GATEWAY_ADDR_ALL, msg, len);
        }
    }
}
