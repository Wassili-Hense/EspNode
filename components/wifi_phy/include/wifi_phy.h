/*
 * MQTT-SN transport over UDP on WiFi (station), X13 LAN addressing: the address of a node is its IPv4.
 * Enviriot (MsGUdp) listens on port 1883 and sends to port 1883 of the node.
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

#define WIFI_PHY_PORT       1883

typedef struct {
    const char *ssid;
    const char *password;           // Empty - open network
    bool        static_ip;          // false - DHCP
    uint8_t     ip[4];
    uint8_t     mask[4];
    uint8_t     gateway[4];
    uint16_t    gw_port;            // UDP port of the MQTT-SN gateway
} wifi_phy_config_t;

// Called from the RX task for every received message (msg[0] is the Length field)
typedef void (*wifi_phy_rx_cb_t)(const mqttsn_addr_t *src, const uint8_t *msg, size_t len, void *arg);
// Called from the event task when the station gets an IPv4, NULL when it loses it
typedef void (*wifi_phy_ip_cb_t)(const uint8_t *ip, void *arg);

// Requires nvs_flash_init(). Connects in the background and reconnects with a growing pause.
esp_err_t wifi_phy_init(const wifi_phy_config_t *cfg, wifi_phy_rx_cb_t rx_cb, wifi_phy_ip_cb_t ip_cb, void *arg);

// Transport for mqttsn_client; send() must be called from a single task
const mqttsn_transport_t *wifi_phy_transport(void);

#ifdef __cplusplus
}
#endif
