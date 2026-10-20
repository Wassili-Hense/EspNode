/*
 * cfg partition: parser and checks, no hardware access (host tested).
 */
#include <string.h>

#include "node_cfg.h"

#define HDR_SIZE            16
#define MAX_LENGTH          (NODE_CFG_SIZE - HDR_SIZE)
#define FORMAT_VERSION      1

#define REC_NODE_NAME       0x01
#define REC_NODE_ADDR       0x02
#define REC_IF_UART         0x10
#define REC_IF_WIFI         0x20
#define REC_WIFI_SSID       0x21
#define REC_WIFI_PASSWORD   0x22
#define REC_WIFI_IP         0x23
#define REC_MQTTSN_GATEWAY  0x24

#define UART_PORTS          3
#define BAUD_MIN            1200
#define BAUD_MAX            5000000     // As UART_BAUD_MAX of the PLC

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t node_cfg_crc32(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    while (len-- > 0) {
        crc ^= *buf++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void copy_str(char *dst, const uint8_t *src, size_t len)
{
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// Returns false if the length does not fit the record
static bool parse_record(uint8_t type, const uint8_t *v, uint8_t len, node_cfg_t *cfg)
{
    node_cfg_wifi_t *wifi = &cfg->wifi;

    switch (type) {
    case REC_NODE_NAME:
        if ((len < 1) || (len > NODE_CFG_NAME_MAX)) {
            return false;
        }
        copy_str(cfg->name, v, len);
        break;

    case REC_NODE_ADDR:
        if (len != 1) {
            return false;
        }
        cfg->node_addr = v[0];
        break;

    case REC_IF_UART: {
        if (len != 9) {
            return false;
        }
        node_cfg_if_t *itf = &cfg->ifs[cfg->n_ifs++];  // Room checked by the caller
        itf->type = NODE_CFG_IF_UART;
        itf->port = v[0];
        itf->tx = v[1];
        itf->rx = v[2];
        itf->de = v[3];
        itf->baud = get32(&v[4]);
        itf->framing = v[8];
        break;
    }

    case REC_IF_WIFI:
        if (len != 0) {
            return false;
        }
        cfg->ifs[cfg->n_ifs++].type = NODE_CFG_IF_WIFI;
        break;

    case REC_WIFI_SSID:
        if ((len < 1) || (len > sizeof(wifi->ssid) - 1)) {
            return false;
        }
        copy_str(wifi->ssid, v, len);
        break;

    case REC_WIFI_PASSWORD:
        if (len > sizeof(wifi->password) - 1) {
            return false;
        }
        copy_str(wifi->password, v, len);
        break;

    case REC_WIFI_IP:
        if (len != 12) {
            return false;
        }
        wifi->static_ip = true;
        memcpy(wifi->ip, &v[0], 4);
        memcpy(wifi->mask, &v[4], 4);
        memcpy(wifi->gateway, &v[8], 4);
        break;

    case REC_MQTTSN_GATEWAY:
        if (len != 6) {
            return false;
        }
        wifi->mqttsn_gw_set = true;
        memcpy(wifi->mqttsn_gw_ip, &v[0], 4);
        wifi->mqttsn_gw_port = get16(&v[4]);
        break;

    default:
        break;                          // Unknown record, newer page
    }
    return true;
}

esp_err_t node_cfg_parse(const uint8_t *buf, size_t size, node_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->node_addr = NODE_CFG_ADDR_DHCP;

    if ((size < HDR_SIZE) || (memcmp(buf, "ECFG", 4) != 0)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (buf[4] != FORMAT_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    size_t length = get16(&buf[6]);
    if ((length > MAX_LENGTH) || (length > size - HDR_SIZE)) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t crc = node_cfg_crc32(0, buf, 12);
    if (node_cfg_crc32(crc, &buf[HDR_SIZE], length) != get32(&buf[12])) {
        return ESP_ERR_INVALID_CRC;
    }
    cfg->generation = get32(&buf[8]);

    const uint8_t *p = &buf[HDR_SIZE];
    const uint8_t *end = p + length;
    while (p < end) {
        if ((end - p < 2) || (p[1] > end - p - 2)) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t type = p[0];
        uint8_t len = p[1];
        if (((type == REC_IF_UART) || (type == REC_IF_WIFI)) && (cfg->n_ifs == NODE_CFG_MAX_IFS)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (!parse_record(type, &p[2], len, cfg)) {
            return ESP_ERR_INVALID_ARG;
        }
        p += 2 + len;
    }
    return ESP_OK;
}

static bool has_pin(uint64_t mask, uint8_t pin)
{
    return (pin < 64) && ((mask >> pin) & 1u);
}

static esp_err_t invalid(const char **why, const char *reason)
{
    *why = reason;
    return ESP_ERR_INVALID_ARG;
}

esp_err_t node_cfg_check(const node_cfg_t *cfg, uint64_t valid_pins, uint64_t output_pins,
                         uint8_t reserved_uarts, const char **why)
{
    const uint64_t out_pins = valid_pins & output_pins;
    uint64_t used_pins = 0;
    uint8_t used_uarts = reserved_uarts;
    size_t n_wifi = 0;

    if (cfg->node_addr == 0x00) {
        return invalid(why, "node address 0 is the broadcast address");
    }
    if (cfg->n_ifs == 0) {
        return invalid(why, "no interfaces");
    }

    for (size_t i = 0; i < cfg->n_ifs; i++) {
        const node_cfg_if_t *itf = &cfg->ifs[i];

        if (itf->type == NODE_CFG_IF_WIFI) {
            if (++n_wifi > 1) {
                return invalid(why, "more than one WiFi interface");
            }
            if (cfg->wifi.ssid[0] == '\0') {
                return invalid(why, "no WiFi SSID");
            }
            continue;
        }

        if (itf->port >= UART_PORTS) {
            return invalid(why, "no such UART port");
        }
        if (used_uarts & (1u << itf->port)) {
            return invalid(why, "UART port in use");
        }
        used_uarts |= (uint8_t)(1u << itf->port);

        if (!has_pin(out_pins, itf->tx) || !has_pin(valid_pins, itf->rx) ||
            ((itf->de != NODE_CFG_NO_PIN) && !has_pin(out_pins, itf->de))) {
            return invalid(why, "UART pin not available");
        }
        const uint8_t pins[] = { itf->tx, itf->rx, itf->de };
        for (size_t k = 0; k < sizeof(pins); k++) {
            if (pins[k] == NODE_CFG_NO_PIN) {
                continue;
            }
            if (used_pins & (1ull << pins[k])) {
                return invalid(why, "pin used twice");
            }
            used_pins |= 1ull << pins[k];
        }

        if ((itf->baud < BAUD_MIN) || (itf->baud > BAUD_MAX)) {
            return invalid(why, "baud rate out of range");
        }
        if (itf->framing > NODE_CFG_FRAMING_RS485) {
            return invalid(why, "unknown UART framing");
        }
    }
    return ESP_OK;
}
