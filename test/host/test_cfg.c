// Host tests of the cfg partition: format (docs/cfg-format.md) and checks of the parameters
#include <stdio.h>
#include <string.h>

#include "node_cfg.h"

static int failed, passed;

#define CHECK(cond) do { \
    if (cond) { passed++; } else { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// ESP32: as SOC_GPIO_VALID_GPIO_MASK without flash GPIO6-11, SOC_GPIO_VALID_OUTPUT_GPIO_MASK
#define VALID_PINS      (0xFFFFFFFFFFull & ~(0xF1000000ull | 0xFC0ull))
#define OUTPUT_PINS     (VALID_PINS & ~0xFC00000000ull)

static uint8_t img[NODE_CFG_SIZE];

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

// Partition image: header, records, FF up to the end
static esp_err_t parse(const uint8_t *rec, size_t len, node_cfg_t *cfg)
{
    memset(img, 0xFF, sizeof(img));
    memcpy(img, "ECFG\x01\x00", 6);
    img[6] = (uint8_t)len;
    img[7] = (uint8_t)(len >> 8);
    put32(&img[8], 1000);
    memcpy(&img[16], rec, len);
    uint32_t crc = node_cfg_crc32(0, img, 12);
    put32(&img[12], node_cfg_crc32(crc, &img[16], len));
    return node_cfg_parse(img, sizeof(img), cfg);
}

#define PARSE(cfg, ...) parse((const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ }), cfg)

static const char *check(const node_cfg_t *cfg)
{
    const char *why = NULL;
    if (node_cfg_check(cfg, VALID_PINS, OUTPUT_PINS, 0, &why) == ESP_OK) {
        return NULL;
    }
    return why ? why : "?";
}

static node_cfg_t uart_cfg(uint8_t port, uint8_t tx, uint8_t rx, uint8_t de, uint32_t baud)
{
    node_cfg_t cfg = { .node_addr = NODE_CFG_ADDR_DHCP, .n_ifs = 1 };
    cfg.ifs[0] = (node_cfg_if_t){ .type = NODE_CFG_IF_UART, .port = port, .tx = tx, .rx = rx, .de = de,
                                  .baud = baud, .framing = NODE_CFG_FRAMING_SLIP };
    return cfg;
}

int main(void)
{
    node_cfg_t cfg;

    // CRC-32 as zlib, also in parts
    CHECK(node_cfg_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(node_cfg_crc32(node_cfg_crc32(0, (const uint8_t *)"1234", 4), (const uint8_t *)"56789", 5) ==
          0xCBF43926u);

    // Example of docs/cfg-format.md, as is and in an erased partition
    static const uint8_t example[] = {
        0x45, 0x43, 0x46, 0x47, 0x01, 0x00, 0x17, 0x00, 0x00, 0xF0, 0x08, 0x67, 0x4C, 0x79, 0x26, 0xE2,
        0x01, 0x07, 0x6B, 0x69, 0x74, 0x63, 0x68, 0x65, 0x6E, 0x02, 0x01, 0xFF, 0x10, 0x09, 0x01, 0x11,
        0x10, 0xFF, 0x00, 0x96, 0x00, 0x00, 0x00,
    };
    CHECK(node_cfg_parse(example, sizeof(example), &cfg) == ESP_OK);
    CHECK(cfg.generation == 0x6708F000u);
    CHECK(strcmp(cfg.name, "kitchen") == 0);
    CHECK(cfg.node_addr == NODE_CFG_ADDR_DHCP);
    CHECK(cfg.n_ifs == 1);
    CHECK((cfg.ifs[0].type == NODE_CFG_IF_UART) && (cfg.ifs[0].port == 1) && (cfg.ifs[0].tx == 17) &&
          (cfg.ifs[0].rx == 16) && (cfg.ifs[0].de == NODE_CFG_NO_PIN) && (cfg.ifs[0].baud == 38400) &&
          (cfg.ifs[0].framing == NODE_CFG_FRAMING_SLIP));
    CHECK(check(&cfg) == NULL);
    memset(img, 0xFF, sizeof(img));
    memcpy(img, example, sizeof(example));
    CHECK(node_cfg_parse(img, sizeof(img), &cfg) == ESP_OK);

    // Header
    memset(img, 0xFF, sizeof(img));
    CHECK(node_cfg_parse(img, sizeof(img), &cfg) == ESP_ERR_NOT_FOUND);         // Erased
    CHECK(node_cfg_parse(example, 15, &cfg) == ESP_ERR_NOT_FOUND);              // Shorter than the header
    memcpy(img, example, sizeof(example));
    img[4] = 2;
    CHECK(node_cfg_parse(img, sizeof(img), &cfg) == ESP_ERR_INVALID_VERSION);
    memcpy(img, example, sizeof(example));
    img[20] ^= 0x01;
    CHECK(node_cfg_parse(img, sizeof(img), &cfg) == ESP_ERR_INVALID_CRC);
    memcpy(img, example, sizeof(example));
    img[8] ^= 0x01;                                                             // Generation is covered too
    CHECK(node_cfg_parse(img, sizeof(img), &cfg) == ESP_ERR_INVALID_CRC);
    memcpy(img, example, sizeof(example));
    img[6] = 0xF1;                                                              // 4081 bytes
    img[7] = 0x0F;
    CHECK(node_cfg_parse(img, sizeof(img), &cfg) == ESP_ERR_INVALID_SIZE);
    CHECK(node_cfg_parse(example, sizeof(example) - 1, &cfg) == ESP_ERR_INVALID_SIZE);

    // Records
    CHECK(PARSE(&cfg, 0x01, 0x05, 'a', 'b') == ESP_ERR_INVALID_SIZE);           // Beyond the length
    CHECK(PARSE(&cfg, 0x02) == ESP_ERR_INVALID_SIZE);                           // Header of a record cut
    CHECK(PARSE(&cfg, 0x02, 0x02, 0x10, 0x00) == ESP_ERR_INVALID_ARG);          // NODE_ADDR is 1 byte
    CHECK(PARSE(&cfg, 0x01, 0x00) == ESP_ERR_INVALID_ARG);                      // Empty name
    CHECK(PARSE(&cfg, 0x01, 24, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
                'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x') == ESP_ERR_INVALID_ARG);
    CHECK(PARSE(&cfg, 0x10, 0x08, 1, 17, 16, 0xFF, 0x00, 0x96, 0x00, 0x00) == ESP_ERR_INVALID_ARG);
    CHECK(PARSE(&cfg, 0x20, 0x01, 0x00) == ESP_ERR_INVALID_ARG);                // IF_WIFI has no value

    CHECK(parse((const uint8_t[]){ 0 }, 0, &cfg) == ESP_OK);                    // No records
    CHECK((cfg.generation == 1000) && (cfg.n_ifs == 0) && (cfg.node_addr == NODE_CFG_ADDR_DHCP) &&
          (cfg.name[0] == '\0'));
    CHECK(check(&cfg) != NULL);                                                 // No interfaces

    CHECK(PARSE(&cfg, 0x7F, 0x03, 1, 2, 3, 0x02, 0x01, 0x21) == ESP_OK);       // Unknown skipped
    CHECK(cfg.node_addr == 0x21);
    CHECK(PARSE(&cfg, 0x01, 0x01, 'a', 0x02, 0x01, 0x05, 0x01, 0x02, 'b', 'c', 0x02, 0x01, 0x06) == ESP_OK);
    CHECK((strcmp(cfg.name, "bc") == 0) && (cfg.node_addr == 0x06));            // Repeated: the last

    CHECK(PARSE(&cfg, 0x20, 0, 0x20, 0, 0x20, 0, 0x20, 0, 0x20, 0) == ESP_ERR_INVALID_SIZE);   // 5 interfaces

    // WiFi + RS-485: interfaces in order, WiFi parameters
    CHECK(PARSE(&cfg,
                0x20, 0x00,
                0x10, 0x09, 2, 17, 16, 4, 0x40, 0x0D, 0x03, 0x00, 1,
                0x21, 0x04, 'h', 'o', 'm', 'e',
                0x22, 0x00,
                0x23, 0x0C, 192, 168, 1, 10, 255, 255, 255, 0, 192, 168, 1, 1,
                0x24, 0x06, 192, 168, 1, 2, 0x5B, 0x07) == ESP_OK);
    CHECK(cfg.n_ifs == 2);
    CHECK(cfg.ifs[0].type == NODE_CFG_IF_WIFI);
    CHECK((cfg.ifs[1].type == NODE_CFG_IF_UART) && (cfg.ifs[1].port == 2) && (cfg.ifs[1].de == 4) &&
          (cfg.ifs[1].baud == 200000) && (cfg.ifs[1].framing == NODE_CFG_FRAMING_RS485));
    CHECK((strcmp(cfg.wifi.ssid, "home") == 0) && (cfg.wifi.password[0] == '\0'));
    CHECK(cfg.wifi.static_ip && (memcmp(cfg.wifi.ip, (const uint8_t[]){ 192, 168, 1, 10 }, 4) == 0) &&
          (cfg.wifi.mask[3] == 0) && (cfg.wifi.gateway[3] == 1));
    CHECK(cfg.wifi.mqttsn_gw_set && (cfg.wifi.mqttsn_gw_ip[3] == 2) && (cfg.wifi.mqttsn_gw_port == 1883));
    CHECK(check(&cfg) == NULL);
    cfg.wifi.ssid[0] = '\0';
    CHECK(check(&cfg) != NULL);                                                 // WiFi without SSID
    CHECK(PARSE(&cfg, 0x20, 0x00) == ESP_OK);
    CHECK(!cfg.wifi.static_ip && !cfg.wifi.mqttsn_gw_set);                      // DHCP, SEARCHGW

    // Checks
    cfg = uart_cfg(0, 1, 3, NODE_CFG_NO_PIN, 38400);                            // Kconfig default
    CHECK(check(&cfg) == NULL);
    cfg = uart_cfg(1, 17, 34, NODE_CFG_NO_PIN, 38400);                          // RX on an input-only pin
    CHECK(check(&cfg) == NULL);
    cfg = uart_cfg(1, 34, 16, NODE_CFG_NO_PIN, 38400);                          // TX on an input-only pin
    CHECK(check(&cfg) != NULL);
    cfg = uart_cfg(1, 17, 16, 35, 38400);
    CHECK(check(&cfg) != NULL);
    cfg = uart_cfg(1, 7, 16, NODE_CFG_NO_PIN, 38400);                           // Flash
    CHECK(check(&cfg) != NULL);
    cfg = uart_cfg(1, 17, 24, NODE_CFG_NO_PIN, 38400);                          // No GPIO24
    CHECK(check(&cfg) != NULL);
    cfg = uart_cfg(1, 17, 200, NODE_CFG_NO_PIN, 38400);
    CHECK(check(&cfg) != NULL);
    cfg = uart_cfg(1, 17, 17, NODE_CFG_NO_PIN, 38400);
    CHECK(check(&cfg) != NULL);
    cfg = uart_cfg(1, 17, 16, 16, 38400);
    CHECK(check(&cfg) != NULL);
    cfg = uart_cfg(3, 17, 16, NODE_CFG_NO_PIN, 38400);
    CHECK(check(&cfg) != NULL);
    cfg = uart_cfg(1, 17, 16, NODE_CFG_NO_PIN, 1200);
    CHECK(check(&cfg) == NULL);
    cfg = uart_cfg(1, 17, 16, NODE_CFG_NO_PIN, 1199);
    CHECK(check(&cfg) != NULL);
    cfg = uart_cfg(1, 17, 16, NODE_CFG_NO_PIN, 5000001);
    CHECK(check(&cfg) != NULL);
    cfg = uart_cfg(1, 17, 16, NODE_CFG_NO_PIN, 38400);
    cfg.ifs[0].framing = 2;
    CHECK(check(&cfg) != NULL);
    cfg = uart_cfg(1, 17, 16, NODE_CFG_NO_PIN, 38400);
    cfg.node_addr = 0x00;                                                       // Broadcast
    CHECK(check(&cfg) != NULL);
    cfg.node_addr = 0x01;
    CHECK(check(&cfg) == NULL);

    const char *why = NULL;
    cfg = uart_cfg(0, 1, 3, NODE_CFG_NO_PIN, 38400);
    CHECK(node_cfg_check(&cfg, VALID_PINS, OUTPUT_PINS, 1u << 0, &why) == ESP_ERR_INVALID_ARG);   // Console
    CHECK(why != NULL);

    cfg = uart_cfg(1, 17, 16, NODE_CFG_NO_PIN, 38400);                          // Two UARTs
    cfg.n_ifs = 2;
    cfg.ifs[1] = cfg.ifs[0];
    CHECK(check(&cfg) != NULL);                                                 // Same port
    cfg.ifs[1].port = 2;
    CHECK(check(&cfg) != NULL);                                                 // Same pins
    cfg.ifs[1].tx = 25;
    cfg.ifs[1].rx = 26;
    CHECK(check(&cfg) == NULL);

    cfg = uart_cfg(1, 17, 16, NODE_CFG_NO_PIN, 38400);                          // Two WiFi
    strcpy(cfg.wifi.ssid, "home");
    cfg.n_ifs = 3;
    cfg.ifs[1].type = NODE_CFG_IF_WIFI;
    cfg.ifs[2].type = NODE_CFG_IF_WIFI;
    CHECK(check(&cfg) != NULL);
    cfg.n_ifs = 2;
    CHECK(check(&cfg) == NULL);

    printf("%d passed, %d failed\n", passed, failed);
    printf(failed ? "FAILED\n" : "ALL PASSED\n");
    return failed ? 1 : 0;
}
