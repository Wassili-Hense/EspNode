/*
 * MQTT-SN client (node role) with X13 extensions.
 *
 * Based on X13.Home device firmware (MQTT-SN Library v3.2.0), Copyright (c) 2011-2017
 * <comparator@gmx.de>, BSD 3-Clause License. See LICENSE.X13 for details.
 */
#include <string.h>

#include "esp_log.h"

#include "mqttsn_client.h"
#include "mqttsn_msg.h"

static const char *TAG = "mqttsn";

#define KEEPALIVE_S         300         // Keep alive time sent in CONNECT
#define N_RETRIES           3
#define MAX_RADIUS          3           // Hops to gateway

#define T_KEEPALIVE_MS      (KEEPALIVE_S * 1000)
#define T_FAST_MS           100
#define T_SHORT_MS          2000
#define T_DISCONNECT_MS     30000       // Pause after the gateway search failed

static struct {
    mqttsn_client_config_t  cfg;
    mqttsn_status_t         status;
    mqttsn_addr_t           node_addr;
    mqttsn_addr_t           gw_addr;
    uint8_t                 gw_id;
    uint8_t                 radius;         // Broadcast radius
    uint16_t                msg_id;         // Last outgoing message id, DHCP transaction id
    uint16_t                rx_msg_id;      // Last incoming PUBLISH id, duplicate detection
    int64_t                 deadline;       // Time of the next poll action, ms
    uint8_t                 retries;
    uint8_t                 pending_len;    // Request in flight, 0 - none
    uint8_t                 pending[MQTTSN_MAX_MSG_LEN];
} s;

static const char *const status_names[] = {
    [MQTTSN_STATUS_DISCONNECTED]    = "DISCONNECTED",
    [MQTTSN_STATUS_DHCP]            = "DHCP",
    [MQTTSN_STATUS_SEARCHGW]        = "SEARCHGW",
    [MQTTSN_STATUS_OFFLINE]         = "OFFLINE",
    [MQTTSN_STATUS_PRE_CONNECT]     = "PRE_CONNECT",
    [MQTTSN_STATUS_CONNECT]         = "CONNECT",
};

////////////////////////////////////////////////////////////////////////
// Helpers

static int64_t now_ms(void)
{
    return s.cfg.now_ms();
}

static void set_delay(uint32_t ms)
{
    s.deadline = now_ms() + ms;
}

// Act on the next tick
static void set_delay_none(void)
{
    s.deadline = now_ms();
}

static uint32_t random_delay(uint32_t min_ms, uint32_t max_ms)
{
    return min_ms + (uint32_t)(((uint64_t)(max_ms - min_ms) * s.cfg.rand16()) >> 16);
}

// Time between CONNECT's, 1-5 s
static uint32_t t_connect(void)
{
    return random_delay(1000, 5000);
}

// Gateway search / DHCP backoff, grows with every retry: 1-5 / 6-10 / 11-15 / 16-20 s
static uint32_t t_searchgw(void)
{
    uint32_t n = N_RETRIES - s.retries;
    return random_delay(1000 * (5 * n + 1), 5000 * (n + 1));
}

// Gateway found -> CONNECT, 1-2 s
static uint32_t t_accel(void)
{
    return random_delay(1000, 2000);
}

// Retransmission delay, grows with every retry: n * (120..250) ms
static uint32_t t_retry(void)
{
    uint32_t n = N_RETRIES - s.retries;
    return random_delay(n * 120, n * 250);
}

static uint16_t new_msg_id(void)
{
    s.msg_id++;
    if ((s.msg_id == 0) || (s.msg_id == 0xFFFF)) {
        s.msg_id = 1;
    }
    return s.msg_id;
}

static void set_status(mqttsn_status_t status)
{
    if (status != s.status) {
        ESP_LOGI(TAG, "%s -> %s", status_names[s.status], status_names[status]);
        s.status = status;
    }
}

static void send_msg(const mqttsn_addr_t *dst, const uint8_t *msg, size_t len)
{
    ESP_LOGD(TAG, "tx %s", mqttsn_msg_type_str(msg[1]));
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, msg, len, ESP_LOG_DEBUG);

    if (!s.cfg.transport->send(s.cfg.transport->ctx, dst, msg, len)) {
        ESP_LOGW(TAG, "tx %s failed", mqttsn_msg_type_str(msg[1]));
    }
}

static bool pending_matches(uint8_t type, const uint8_t *msg_id)
{
    if ((s.pending_len == 0) || (s.pending[1] != type)) {
        return false;
    }

    size_t offs = (type == MQTTSN_PUBLISH) ? 5 : (type == MQTTSN_SUBSCRIBE) ? 3 : 4;
    return memcmp(&s.pending[offs], msg_id, 2) == 0;
}

// Copy the pending request for (re)transmission.
// QoS1 PUBLISH stays pending until PUBACK, retransmissions carry DUP. Returns true if
// the request is still pending.
static bool take_pending(uint8_t *msg, size_t *len)
{
    memcpy(msg, s.pending, s.pending_len);
    *len = s.pending_len;

    if (s.pending[1] == MQTTSN_PUBLISH) {
        if ((s.pending[2] & MQTTSN_FL_QOS_MASK) != MQTTSN_FL_QOS1) {
            s.pending_len = 0;      // QoS0/QoS-1, fire and forget
            return false;
        }
        s.pending[2] |= MQTTSN_FL_DUP;
    }
    return true;
}

static bool queue_request(const uint8_t *msg, size_t len)
{
    if (len == 0) {
        return false;
    }

    memcpy(s.pending, msg, len);
    s.pending_len = (uint8_t)len;

    if ((s.status == MQTTSN_STATUS_PRE_CONNECT) || (s.status == MQTTSN_STATUS_CONNECT)) {
        set_delay_none();
        s.retries = N_RETRIES;
    }
    return true;
}

// (Re)start: load addresses and select the initial state
static void client_reset(void)
{
    const mqttsn_transport_t *phy = s.cfg.transport;

    s.cfg.cb.load_addrs(&s.node_addr, &s.gw_addr);
    if (phy->set_addr != NULL) {
        phy->set_addr(phy->ctx, &s.node_addr);
    }

    if (mqttsn_addr_eq(&s.node_addr, &phy->undef)) {
        set_status(MQTTSN_STATUS_DHCP);
    } else if (mqttsn_addr_eq(&s.gw_addr, &phy->undef)) {
        set_status(MQTTSN_STATUS_SEARCHGW);
    } else {
        set_status(MQTTSN_STATUS_OFFLINE);
    }

    s.gw_id = 0;
    s.radius = 1;
    s.retries = N_RETRIES;
    s.msg_id = 0;
    s.rx_msg_id = 0xFFFF;
    s.pending_len = 0;
    set_delay(T_SHORT_MS);
}

////////////////////////////////////////////////////////////////////////
// Incoming messages

static void gateway_found(const mqttsn_addr_t *addr, uint8_t gw_id)
{
    ESP_LOGI(TAG, "gateway %u found", gw_id);

    s.gw_addr = *addr;
    s.gw_id = gw_id;
    s.retries = N_RETRIES;
    set_delay(t_accel());
    set_status(MQTTSN_STATUS_OFFLINE);
}

static void handle_register(const uint8_t *msg, size_t len)
{
    uint16_t topic_id = mqttsn_get_u16(&msg[2]);
    uint16_t msg_id = mqttsn_get_u16(&msg[4]);

    if (s.pending_len == 0) {
        set_delay(T_SHORT_MS);      // Postpone PINGREQ, the link is alive
    }

    uint8_t rc = s.cfg.cb.on_register(topic_id, &msg[MQTTSN_SIZEOF_REGISTER],
                                      len - MQTTSN_SIZEOF_REGISTER);

    uint8_t ack[MQTTSN_MAX_MSG_LEN];
    send_msg(&s.gw_addr, ack, mqttsn_msg_regack(ack, topic_id, msg_id, rc));
}

static void handle_publish(const uint8_t *msg, size_t len)
{
    uint8_t flags = msg[2];
    uint16_t topic_id = mqttsn_get_u16(&msg[3]);
    uint16_t msg_id = mqttsn_get_u16(&msg[5]);
    uint8_t rc = MQTTSN_RC_ACCEPTED;

    if (((flags & MQTTSN_FL_DUP) == 0) || (msg_id != s.rx_msg_id)) {
        rc = s.cfg.cb.on_publish(topic_id, flags, &msg[MQTTSN_SIZEOF_PUBLISH],
                                 len - MQTTSN_SIZEOF_PUBLISH);
        s.rx_msg_id = msg_id;
    }

    // QoS2 is not supported
    if ((flags & MQTTSN_FL_QOS_MASK) == MQTTSN_FL_QOS1) {
        if (s.pending_len == 0) {
            set_delay(T_SHORT_MS);
        }

        uint8_t ack[MQTTSN_MAX_MSG_LEN];
        send_msg(&s.gw_addr, ack, mqttsn_msg_puback(ack, topic_id, msg_id, rc));
    }
}

static void handle_dhcpresp(const uint8_t *msg, size_t len)
{
    const mqttsn_transport_t *phy = s.cfg.transport;

    if ((len != (size_t)(MQTTSN_SIZEOF_DHCPRESP + phy->undef.len)) ||
        (mqttsn_get_u16(&msg[3]) != s.msg_id)) {
        return;                     // Not our transaction
    }

    mqttsn_addr_t addr = { .len = phy->undef.len };
    memcpy(addr.b, &msg[MQTTSN_SIZEOF_DHCPRESP], addr.len);

    if (mqttsn_addr_eq(&addr, &phy->undef) || mqttsn_addr_eq(&addr, &phy->broadcast)) {
        ESP_LOGW(TAG, "DHCP: invalid address assigned");
        return;
    }

    ESP_LOGI(TAG, "DHCP: address assigned");
    ESP_LOG_BUFFER_HEX(TAG, addr.b, addr.len);

    s.cfg.cb.store_node_addr(&addr);
    set_delay_none();
    set_status(MQTTSN_STATUS_DISCONNECTED);     // Restart with the new address
}

void mqttsn_client_handle_rx(const mqttsn_addr_t *src, const uint8_t *msg, size_t len)
{
    if ((len < 2) || (msg[0] < 2) || (msg[0] > len)) {
        ESP_LOGW(TAG, "rx: malformed message");
        return;
    }

    if (src == NULL) {
        src = &s.node_addr;             // Point-to-point link
    }

    len = msg[0];
    uint8_t type = msg[1];
    bool from_gw = mqttsn_addr_eq(src, &s.gw_addr);

    ESP_LOGD(TAG, "rx %s", mqttsn_msg_type_str(type));
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, msg, len, ESP_LOG_DEBUG);

    switch (type) {
    case MQTTSN_ADVERTISE:
        if ((s.status == MQTTSN_STATUS_SEARCHGW) && (len >= MQTTSN_SIZEOF_ADVERTISE)) {
            gateway_found(src, msg[2]);
            s.radius = 1;
        }
        break;

    case MQTTSN_GWINFO:
        if ((s.status == MQTTSN_STATUS_SEARCHGW) && (len >= MQTTSN_SIZEOF_GWINFO)) {
            gateway_found(src, msg[2]);
        }
        break;

    case MQTTSN_SEARCHGW:
        // Another node is searching too, back off
        if (s.status == MQTTSN_STATUS_SEARCHGW) {
            set_delay(t_searchgw());
        }
        break;

    case MQTTSN_CONNACK:
        if (from_gw && (s.status == MQTTSN_STATUS_OFFLINE) && (len >= MQTTSN_SIZEOF_CONNACK) &&
            (msg[2] == MQTTSN_RC_ACCEPTED)) {
            s.retries = N_RETRIES;
            set_delay(T_SHORT_MS);
            set_status(MQTTSN_STATUS_PRE_CONNECT);
        }
        break;

    case MQTTSN_REGISTER:
        if (from_gw && (s.status == MQTTSN_STATUS_CONNECT) && (len >= MQTTSN_SIZEOF_REGISTER)) {
            handle_register(msg, len);
        }
        break;

    case MQTTSN_REGACK:
        if (from_gw && ((s.status == MQTTSN_STATUS_PRE_CONNECT) ||
                        (s.status == MQTTSN_STATUS_CONNECT)) &&
            (len >= MQTTSN_SIZEOF_REGACK) && pending_matches(MQTTSN_REGISTER, &msg[4])) {
            s.pending_len = 0;
            s.retries = N_RETRIES;
            set_delay(T_SHORT_MS);

            uint16_t topic_id = (msg[6] == MQTTSN_RC_ACCEPTED) ? mqttsn_get_u16(&msg[2])
                                                               : MQTTSN_TOPIC_ID_INVALID;
            s.cfg.cb.on_regack(topic_id);
        }
        break;

    case MQTTSN_PUBLISH:
        if (from_gw && (s.status == MQTTSN_STATUS_CONNECT) && (len >= MQTTSN_SIZEOF_PUBLISH)) {
            handle_publish(msg, len);
        }
        break;

    case MQTTSN_PUBACK:
        if (from_gw && ((s.status == MQTTSN_STATUS_PRE_CONNECT) ||
                        (s.status == MQTTSN_STATUS_CONNECT)) &&
            (len >= MQTTSN_SIZEOF_PUBACK) && pending_matches(MQTTSN_PUBLISH, &msg[4])) {
            s.pending_len = 0;
            s.retries = N_RETRIES;
            set_delay(T_SHORT_MS);
        }
        break;

    case MQTTSN_SUBACK:
        if (from_gw && (s.status == MQTTSN_STATUS_PRE_CONNECT) &&
            (len >= MQTTSN_SIZEOF_SUBACK) && pending_matches(MQTTSN_SUBSCRIBE, &msg[5])) {
            s.pending_len = 0;
            s.retries = N_RETRIES;
            set_delay(T_SHORT_MS);
            set_status(MQTTSN_STATUS_CONNECT);
        }
        break;

    case MQTTSN_PINGRESP:
        if (from_gw && (s.status == MQTTSN_STATUS_CONNECT)) {
            s.retries = N_RETRIES;
            set_delay(T_KEEPALIVE_MS);
        }
        break;

    case MQTTSN_DISCONNECT:
        if (s.status < MQTTSN_STATUS_OFFLINE) {
            s.radius = 1;
            s.retries = N_RETRIES;
            set_delay_none();
        } else if (from_gw) {
            s.retries = N_RETRIES;
            set_delay(T_FAST_MS);
            set_status(MQTTSN_STATUS_DISCONNECTED);
        }
        break;

    case MQTTSN_DHCPREQ:
        // Another node is requesting an address, back off
        if (s.status == MQTTSN_STATUS_DHCP) {
            set_delay(t_searchgw());
        }
        break;

    case MQTTSN_DHCPRESP:
        if (s.status == MQTTSN_STATUS_DHCP) {
            handle_dhcpresp(msg, len);
        }
        break;

    default:
        break;
    }
}

////////////////////////////////////////////////////////////////////////
// Poll

void mqttsn_client_tick(void)
{
    if (now_ms() < s.deadline) {
        return;
    }

    uint8_t msg[MQTTSN_MAX_MSG_LEN];
    size_t len = 0;
    const mqttsn_addr_t *dst = &s.gw_addr;

    switch (s.status) {
    case MQTTSN_STATUS_CONNECT:
        if (s.retries > 0) {
            s.retries--;
        } else {
            ESP_LOGW(TAG, "connection lost");
            set_delay_none();
            set_status(MQTTSN_STATUS_DISCONNECTED);
            return;
        }

        if (s.pending_len != 0) {
            if (take_pending(msg, &len)) {
                set_delay(t_retry());
            } else {
                set_delay(T_KEEPALIVE_MS);
            }
        } else {
            len = mqttsn_msg_pingreq(msg);
            set_delay(T_KEEPALIVE_MS);
        }
        break;

    case MQTTSN_STATUS_DISCONNECTED:
        client_reset();
        set_delay(T_FAST_MS);
        return;

    case MQTTSN_STATUS_DHCP:
    case MQTTSN_STATUS_SEARCHGW:
        if (s.retries > 0) {
            s.retries--;
        } else if (s.radius < MAX_RADIUS) {
            s.radius++;
            s.retries = N_RETRIES;
            return;
        } else {
            set_delay(T_DISCONNECT_MS);
            set_status(MQTTSN_STATUS_DISCONNECTED);
            return;
        }

        set_delay(t_searchgw());
        dst = &s.cfg.transport->broadcast;

        if (s.status == MQTTSN_STATUS_DHCP) {
            uint8_t hlen = s.cfg.transport->undef.len;
            s.msg_id = s.cfg.rand16();
            len = mqttsn_msg_dhcpreq(msg, s.radius, s.msg_id, &hlen, 1);
        } else {
            len = mqttsn_msg_searchgw(msg, s.radius);
        }
        break;

    case MQTTSN_STATUS_OFFLINE:
        if (s.retries > 0) {
            s.retries--;
        } else {
            s.msg_id = 0;
            s.radius = 1;
            s.retries = N_RETRIES;
            set_delay(t_searchgw());
            set_status(MQTTSN_STATUS_SEARCHGW);
            return;
        }

        set_delay(t_connect());
        {
            uint8_t client_id[MQTTSN_CLIENT_ID_MAX];
            size_t id_len = s.cfg.cb.get_client_id(client_id);
            if (id_len > MQTTSN_CLIENT_ID_MAX) {
                id_len = MQTTSN_CLIENT_ID_MAX;
            }
            len = mqttsn_msg_connect(msg, MQTTSN_FL_CLEANSESSION, KEEPALIVE_S, client_id, id_len);
        }
        break;

    case MQTTSN_STATUS_PRE_CONNECT:
        if (s.retries > 0) {
            s.retries--;
        } else {
            set_delay_none();
            set_status(MQTTSN_STATUS_DISCONNECTED);
            return;
        }

        set_delay(t_retry());
        if (s.pending_len != 0) {
            take_pending(msg, &len);
        }
        break;
    }

    if (len > 0) {
        send_msg(dst, msg, len);
    }
}

////////////////////////////////////////////////////////////////////////
// API

void mqttsn_client_init(const mqttsn_client_config_t *cfg)
{
    memset(&s, 0, sizeof(s));
    s.cfg = *cfg;
    s.status = MQTTSN_STATUS_DISCONNECTED;
    client_reset();
}

mqttsn_status_t mqttsn_client_status(void)
{
    return s.status;
}

const char *mqttsn_client_status_str(mqttsn_status_t status)
{
    return ((unsigned)status < sizeof(status_names) / sizeof(status_names[0])) ? status_names[status] : "?";
}

bool mqttsn_client_can_send(void)
{
    return s.pending_len == 0;
}

bool mqttsn_client_register(uint16_t topic_id, const uint8_t *name, size_t len)
{
    if (!mqttsn_client_can_send()) {
        return false;
    }

    uint8_t msg[MQTTSN_MAX_MSG_LEN];
    return queue_request(msg, mqttsn_msg_register(msg, topic_id, new_msg_id(), name, len));
}

bool mqttsn_client_publish(uint8_t flags, uint16_t topic_id, const uint8_t *data, size_t len)
{
    if (!mqttsn_client_can_send()) {
        return false;
    }

    uint8_t msg[MQTTSN_MAX_MSG_LEN];
    return queue_request(msg, mqttsn_msg_publish(msg, flags, topic_id, new_msg_id(), data, len));
}

bool mqttsn_client_subscribe(uint8_t flags, const uint8_t *topic, size_t len)
{
    if (!mqttsn_client_can_send()) {
        return false;
    }

    uint8_t msg[MQTTSN_MAX_MSG_LEN];
    return queue_request(msg, mqttsn_msg_subscribe(msg, flags, new_msg_id(), topic, len));
}
