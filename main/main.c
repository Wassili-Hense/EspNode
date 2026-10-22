/*
 * EspNode: X13 MQTT-SN node over a serial link, RS-485 or WiFi; with further interfaces, a gateway for
 * X13 nodes.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "soc/uart_pins.h"

#include "mqttsn_client.h"
#include "mqttsn_gateway.h"
#include "node_cfg.h"
#include "objdict.h"
#include "plc.h"
#include "serial_phy.h"
#include "wifi_phy.h"

static const char *TAG = "espnode";

#define POLL_PERIOD_MS      10          // X13: POLL_TMR_FREQ = 100 Hz
#define EVENT_QUEUE_LEN     8
#define NODE_TASK_STACK     4096
#define NODE_TASK_PRIO      5
#define FLASH_PINS          0xFC0ull    // GPIO6-11
#define DECLARER_UC         "E1"        // X13 declarer: uC family E - ESP32, subtype 1

#ifndef ESPNODE_SW_VERSION              // From the git tag, main/CMakeLists.txt
#define ESPNODE_SW_VERSION  CONFIG_ESPNODE_SW_VERSION
#endif

// A debug build may enable the console (sdkconfig.defaults: none), it must not share the UART
#if CONFIG_ESP_CONSOLE_UART_NUM == CONFIG_ESPNODE_UART_PORT
#error "The console uses the default MQTT-SN UART: choose another ESPNODE_UART_PORT or disable the console"
#endif

// For the node task: a message from a link, or a new IP of the WiFi station
typedef struct {
    enum { EV_RX, EV_IP } kind;
    uint8_t         phy;                // EV_RX: 1 - link to the gateway, 2.. - X13 nodes behind this one
    bool            has_src;            // EV_RX: false on a serial link (SLIP), it has no addresses
    mqttsn_addr_t   src;
    uint8_t         len;                // EV_IP: 4 - msg holds the IP, 0 - lost
    uint8_t         msg[MQTTSN_LINK_MAX_LEN];
} node_event_t;

static QueueHandle_t s_events;
static node_cfg_t s_cfg;                // Active configuration
static uint8_t s_ip[4];                 // IP of the WiFi station, node task only
static serial_phy_t *s_serial[NODE_CFG_MAX_IFS];   // UART of interface i, PHY i + 1

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint16_t rand16(void)
{
    return (uint16_t)esp_random();
}

static uint64_t pin_bit(int pin)
{
    return ((pin >= 0) && (pin < 64)) ? (1ull << pin) : 0;
}

static uint64_t console_pins(void)
{
#if CONFIG_ESP_CONSOLE_UART_CUSTOM
    return pin_bit(CONFIG_ESP_CONSOLE_UART_TX_GPIO) | pin_bit(CONFIG_ESP_CONSOLE_UART_RX_GPIO);
#elif CONFIG_ESP_CONSOLE_UART
    return pin_bit(U0TXD_GPIO_NUM) | pin_bit(U0RXD_GPIO_NUM);
#else
    return 0;
#endif
}

static uint8_t console_uarts(void)
{
#if CONFIG_ESP_CONSOLE_UART_NUM >= 0
    return (uint8_t)(1u << CONFIG_ESP_CONSOLE_UART_NUM);
#else
    return 0;
#endif
}

// Without cfg: one serial link from Kconfig, name and address from the gateway
static void default_cfg(node_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->node_addr = NODE_CFG_ADDR_DHCP;
    cfg->n_ifs = 1;
    cfg->ifs[0] = (node_cfg_if_t) {
        .type       = NODE_CFG_IF_UART,
        .port       = CONFIG_ESPNODE_UART_PORT,
        .tx         = CONFIG_ESPNODE_UART_TX_PIN,
        .rx         = CONFIG_ESPNODE_UART_RX_PIN,
        .de         = (CONFIG_ESPNODE_UART_DE_PIN >= 0) ? CONFIG_ESPNODE_UART_DE_PIN : NODE_CFG_NO_PIN,
        .framing    = NODE_CFG_FRAMING_SLIP,
        .baud       = CONFIG_ESPNODE_UART_BAUD,
    };
}

// Interfaces this version can start: X13 serial links and RS-485 buses anywhere, WiFi only as the first one
// (to the gateway): X13 nodes behind this one over WiFi are not supported
static const char *unsupported(const node_cfg_t *cfg)
{
    for (size_t i = 1; i < cfg->n_ifs; i++) {
        if (cfg->ifs[i].type == NODE_CFG_IF_WIFI) {
            return "WiFi as a gateway interface not supported";
        }
    }
    return NULL;
}

// cfg partition; missing, damaged or not usable - Kconfig: the node must stay reachable
static void load_cfg(node_cfg_t *cfg)
{
    const char *why = NULL;

    esp_err_t err = node_cfg_read(cfg);
    if (err != ESP_OK) {
        why = (err == ESP_ERR_NOT_FOUND) ? "not written" : esp_err_to_name(err);
    } else if (node_cfg_check(cfg, SOC_GPIO_VALID_GPIO_MASK & ~(FLASH_PINS | console_pins()),
                              SOC_GPIO_VALID_OUTPUT_GPIO_MASK, console_uarts(), &why) == ESP_OK) {
        why = unsupported(cfg);
    }

    if (why != NULL) {
        ESP_LOGW(TAG, "cfg: %s, using defaults", why);
        default_cfg(cfg);
    } else {
        ESP_LOGI(TAG, "cfg: generation %" PRIu32, cfg->generation);
    }
}

// "<declarer>.<sw version>"; PHY letters from the interfaces as X13: S - serial, M - RS-485, W - WiFi
static const char *device_type(const node_cfg_t *cfg)
{
    static char buf[sizeof(DECLARER_UC) + NODE_CFG_MAX_IFS + sizeof(CONFIG_ESPNODE_HW_VERSION) +
                    sizeof(ESPNODE_SW_VERSION)];
    char phy[NODE_CFG_MAX_IFS + 1] = { 0 };

    for (size_t i = 0; i < cfg->n_ifs; i++) {
        const node_cfg_if_t *itf = &cfg->ifs[i];
        phy[i] = (itf->type == NODE_CFG_IF_WIFI) ? 'W' :
                 (itf->framing == NODE_CFG_FRAMING_RS485) ? 'M' : 'S';
    }
    snprintf(buf, sizeof(buf), DECLARER_UC "%s" CONFIG_ESPNODE_HW_VERSION "." ESPNODE_SW_VERSION, phy);
    return buf;
}

static void post(const node_event_t *ev)
{
    if (xQueueSend(s_events, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped");
    }
}

// Called from the RX task of a link (serial, RS-485, UDP), arg - its PHY; src - the sender, NULL on a
// serial link
static void on_link_rx(const mqttsn_addr_t *src, const uint8_t *msg, size_t len, void *arg)
{
    node_event_t ev = { .kind = EV_RX, .phy = (uint8_t)(uintptr_t)arg, .has_src = (src != NULL),
                        .len = (uint8_t)len };
    if (src != NULL) {
        ev.src = *src;
    }
    memcpy(ev.msg, msg, len);
    post(&ev);
}

// Called from the event task: the station got an IP (NULL - lost it)
static void on_wifi_ip(const uint8_t *ip, void *arg)
{
    (void)arg;
    node_event_t ev = { .kind = EV_IP, .len = (ip != NULL) ? 4 : 0 };
    if (ip != NULL) {
        memcpy(ev.msg, ip, 4);
    }
    post(&ev);
}

// WiFi: the node address is its IP, the gateway the one of cfg or found with SEARCHGW
static void wifi_load_addrs(mqttsn_addr_t *node, mqttsn_addr_t *gateway)
{
    node->len = 4;
    memcpy(node->b, s_ip, 4);
    gateway->len = 4;
    if (s_cfg.wifi.mqttsn_gw_set) {
        memcpy(gateway->b, s_cfg.wifi.mqttsn_gw_ip, 4);
    } else {
        memset(gateway->b, 0, 4);
    }
}

// Gateway: to X13 node addr on link phy, from the node task
static bool gateway_send(uint8_t phy, uint8_t addr, const uint8_t *msg, size_t len)
{
    serial_phy_t *serial = s_serial[phy - 1];
    return (serial != NULL) && serial_phy_send(serial, addr, msg, len);
}

// The address from DHCP is also the one of this node on the RS-485 buses behind it, as X13 objRFNodeId
static void store_node_addr(const mqttsn_addr_t *node)
{
    od_store_node_addr(node);
    for (size_t i = 1; i < s_cfg.n_ifs; i++) {
        if (s_serial[i] != NULL) {
            serial_phy_set_addr(s_serial[i], node->b[0]);
        }
    }
}

// Object 0xFFC1 on WiFi: the IP, as X13 LAN nodes
static size_t wifi_phy1_addr(uint8_t *buf, size_t max)
{
    size_t n = (max < 4) ? max : 4;
    memcpy(buf, s_ip, n);
    return n;
}

// Owns the MQTT-SN client and the Object Dictionary
static void node_task(void *arg)
{
    (void)arg;

    const bool wifi = (s_cfg.ifs[0].type == NODE_CFG_IF_WIFI);
    const mqttsn_client_config_t cfg = {
        .transport = wifi ? wifi_phy_transport() : serial_phy_transport(s_serial[0]),
        .cb = {
            .on_register        = od_on_register,
            .on_regack          = od_on_regack,
            .on_publish         = od_on_publish,
            .get_client_id      = od_get_client_id,
            .load_addrs         = wifi ? wifi_load_addrs : od_load_addrs,
            .store_node_addr    = store_node_addr,
            .on_forward         = mqttsn_gateway_on_forward,
            .on_advertise       = mqttsn_gateway_on_advertise,
        },
        .now_ms = now_ms,
        .rand16 = rand16,
    };
    // WiFi: the client starts with the first IP; the PLC runs from the start anyway
    bool started = !wifi;
    if (started) {
        mqttsn_client_init(&cfg);
    }

    int64_t next_poll = now_ms();
    for (;;) {
        TickType_t wait = 0;
        int64_t left = next_poll - now_ms();
        if (left > 0) {
            wait = pdMS_TO_TICKS(left);
            if (wait == 0) {
                wait = 1;
            }
        }

        node_event_t ev;
        if (xQueueReceive(s_events, &ev, wait) == pdTRUE) {
            if (ev.kind == EV_IP) {
                // A new address restarts the session. A lost one does not: sends fail until the station
                // is back, and a long outage ends as with a silent gateway.
                if ((ev.len == 4) && (!started || (memcmp(s_ip, ev.msg, 4) != 0))) {
                    memcpy(s_ip, ev.msg, 4);
                    mqttsn_client_init(&cfg);
                    started = true;
                }
            } else if (ev.phy > 1) {
                mqttsn_gateway_handle_rx(ev.phy, ev.has_src ? ev.src.b[0] : MQTTSN_GATEWAY_ADDR_P2P, ev.msg,
                                         ev.len);
            } else if (started) {
                mqttsn_client_handle_rx(ev.has_src ? &ev.src : NULL, ev.msg, ev.len);
            }
        }

        int64_t now = now_ms();
        if (now >= next_poll) {
            od_poll();
            if (started) {
                mqttsn_client_tick();
            }

            next_poll += POLL_PERIOD_MS;
            if (next_poll <= now) {
                next_poll = now + POLL_PERIOD_MS;   // Overrun, don't catch up
            }
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    load_cfg(&s_cfg);
    const bool wifi = (s_cfg.ifs[0].type == NODE_CFG_IF_WIFI);

    // Pins and ports the PLC program must not touch: flash, console, interfaces
    plc_config_t plc_cfg = {
        .reserved_pins  = FLASH_PINS | console_pins(),
        .reserved_uarts = console_uarts(),
    };
    for (size_t i = 0; i < s_cfg.n_ifs; i++) {
        const node_cfg_if_t *itf = &s_cfg.ifs[i];
        if (itf->type == NODE_CFG_IF_UART) {
            plc_cfg.reserved_pins |= pin_bit(itf->tx) | pin_bit(itf->rx) | pin_bit(itf->de);
            plc_cfg.reserved_uarts |= (uint8_t)(1u << itf->port);
        }
    }
    ESP_ERROR_CHECK(plc_init(&plc_cfg));

    // WiFi: the default name from the MAC, an IP from DHCP would change it
    static char mac_suffix[7];
    if (wifi) {
        uint8_t mac[6];
        ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
        snprintf(mac_suffix, sizeof(mac_suffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);
    }

    static const od_provider_t *providers[1];
    providers[0] = plc_provider();
    const od_config_t od_cfg = {
        .device_type    = device_type(&s_cfg),
        .providers      = providers,
        .n_providers    = 1,
        .cfg_generation = s_cfg.generation,
        .node_name      = s_cfg.name,
        .node_id        = s_cfg.node_addr,
        .name_suffix    = wifi ? mac_suffix : NULL,
        .phy1_addr      = wifi ? wifi_phy1_addr : NULL,
    };
    ESP_ERROR_CHECK(od_init(&od_cfg));

    s_events = xQueueCreate(EVENT_QUEUE_LEN, sizeof(node_event_t));
    ESP_ERROR_CHECK(s_events != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    // Address on RS-485: the node address (DHCP may change it, store_node_addr), 0xFF - none yet
    mqttsn_addr_t node_addr, gw_addr;
    od_load_addrs(&node_addr, &gw_addr);

    // UART of every serial interface: the first links to the gateway, the further ones to X13 nodes
    mqttsn_gateway_config_t gw_cfg = { .send = gateway_send };
    for (size_t i = 0; i < s_cfg.n_ifs; i++) {
        const node_cfg_if_t *itf = &s_cfg.ifs[i];
        if (itf->type != NODE_CFG_IF_UART) {
            continue;
        }
        const serial_phy_config_t serial_cfg = {
            .uart_port  = itf->port,
            .tx_pin     = itf->tx,
            .rx_pin     = itf->rx,
            .de_pin     = (itf->de != NODE_CFG_NO_PIN) ? itf->de : -1,
            .baud_rate  = (int)itf->baud,
            .framing    = (itf->framing == NODE_CFG_FRAMING_RS485) ? SERIAL_PHY_RS485 : SERIAL_PHY_SLIP,
            .addr       = node_addr.b[0],
        };
        ESP_ERROR_CHECK(serial_phy_init(&serial_cfg, on_link_rx, (void *)(uintptr_t)(i + 1), &s_serial[i]));
        if (i > 0) {
            gw_cfg.phys |= (uint8_t)(1u << (i + 1));
        }
    }
    mqttsn_gateway_init(&gw_cfg);

    if (wifi) {
        const node_cfg_wifi_t *w = &s_cfg.wifi;
        wifi_phy_config_t wifi_cfg = {
            .ssid       = w->ssid,
            .password   = w->password,
            .static_ip  = w->static_ip,
            .gw_port    = w->mqttsn_gw_set ? w->mqttsn_gw_port : WIFI_PHY_PORT,
        };
        memcpy(wifi_cfg.ip, w->ip, 4);
        memcpy(wifi_cfg.mask, w->mask, 4);
        memcpy(wifi_cfg.gateway, w->gateway, 4);
        ESP_ERROR_CHECK(wifi_phy_init(&wifi_cfg, on_link_rx, on_wifi_ip, (void *)(uintptr_t)1));
    }

    BaseType_t ok = xTaskCreate(node_task, "node", NODE_TASK_STACK, NULL, NODE_TASK_PRIO, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
