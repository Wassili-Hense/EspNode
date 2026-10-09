/*
 * Transport (PHY) interface used by the MQTT-SN client.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MQTTSN_ADDR_MAX     8

// PHY address, 1..MQTTSN_ADDR_MAX bytes
typedef struct {
    uint8_t len;
    uint8_t b[MQTTSN_ADDR_MAX];
} mqttsn_addr_t;

static inline bool mqttsn_addr_eq(const mqttsn_addr_t *a, const mqttsn_addr_t *b)
{
    return (a->len == b->len) && (memcmp(a->b, b->b, a->len) == 0);
}

typedef struct {
    void           *ctx;
    mqttsn_addr_t   broadcast;  // Broadcast address
    mqttsn_addr_t   undef;      // "Not configured" address, triggers DHCP

    // Send one complete MQTT-SN message (msg[0] is the Length field)
    bool (*send)(void *ctx, const mqttsn_addr_t *dst, const uint8_t *msg, size_t len);
    // Set own address, NULL for links without addresses on the wire
    void (*set_addr)(void *ctx, const mqttsn_addr_t *addr);
} mqttsn_transport_t;

#ifdef __cplusplus
}
#endif
