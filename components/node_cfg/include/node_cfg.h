/*
 * Node configuration written by the installer page into the "cfg" partition: interfaces, node name
 * and address. Format: docs/cfg-format.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_CFG_SIZE           4096    // Partition size
#define NODE_CFG_MAX_IFS        4       // X13: PHY1..PHY4
#define NODE_CFG_NAME_MAX       23
#define NODE_CFG_ADDR_DHCP      0xFF
#define NODE_CFG_NO_PIN         0xFF

typedef enum {
    NODE_CFG_IF_UART = 1,
    NODE_CFG_IF_WIFI,
} node_cfg_if_type_t;

typedef enum {
    NODE_CFG_FRAMING_SLIP   = 0,        // X13 serial
    NODE_CFG_FRAMING_RS485  = 1,        // X13 RS-485, F0 1C
} node_cfg_framing_t;

typedef struct {
    uint8_t     type;                   // node_cfg_if_type_t
    // UART
    uint8_t     port;
    uint8_t     tx;
    uint8_t     rx;
    uint8_t     de;                     // NODE_CFG_NO_PIN - plain UART
    uint8_t     framing;                // node_cfg_framing_t
    uint32_t    baud;
} node_cfg_if_t;

typedef struct {
    char        ssid[33];
    char        password[65];           // Empty - open network
    bool        static_ip;              // false - DHCP
    uint8_t     ip[4];
    uint8_t     mask[4];
    uint8_t     gateway[4];
    bool        mqttsn_gw_set;          // false - search with broadcast SEARCHGW
    uint8_t     mqttsn_gw_ip[4];
    uint16_t    mqttsn_gw_port;
} node_cfg_wifi_t;

typedef struct {
    uint32_t        generation;         // 0 - no cfg (Kconfig defaults)
    char            name[NODE_CFG_NAME_MAX + 1];    // Empty - "<declarer>_<address>"
    uint8_t         node_addr;          // NODE_CFG_ADDR_DHCP - request via DHCP
    size_t          n_ifs;
    node_cfg_if_t   ifs[NODE_CFG_MAX_IFS];          // First - client, following - gateway (FORWARD)
    node_cfg_wifi_t wifi;               // Parameters of the NODE_CFG_IF_WIFI interface
} node_cfg_t;

// CRC-32 as zlib / IEEE 802.3
uint32_t  node_cfg_crc32(uint32_t crc, const uint8_t *buf, size_t len);

// Parse a cfg image. Returns ESP_ERR_NOT_FOUND (no magic, erased partition), ESP_ERR_INVALID_VERSION,
// ESP_ERR_INVALID_SIZE (length or a record out of bounds, more than NODE_CFG_MAX_IFS interfaces),
// ESP_ERR_INVALID_CRC, ESP_ERR_INVALID_ARG (wrong length of a known record)
esp_err_t node_cfg_parse(const uint8_t *buf, size_t size, node_cfg_t *cfg);

// Check the parameters against the chip: pins and ports exist, are free and not shared, bit n - GPIOn /
// UARTn. Returns ESP_ERR_INVALID_ARG and the reason in *why.
esp_err_t node_cfg_check(const node_cfg_t *cfg, uint64_t valid_pins, uint64_t output_pins,
                         uint8_t reserved_uarts, const char **why);

// Read and parse the "cfg" partition, see node_cfg_parse()
esp_err_t node_cfg_read(node_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
