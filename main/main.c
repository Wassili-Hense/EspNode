/*
 * EspNode: X13 MQTT-SN node over a serial link.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
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
#include "node_cfg.h"
#include "objdict.h"
#include "plc.h"
#include "serial_phy.h"

static const char *TAG = "espnode";

#define POLL_PERIOD_MS      10          // X13: POLL_TMR_FREQ = 100 Hz
#define RX_QUEUE_LEN        8
#define NODE_TASK_STACK     4096
#define NODE_TASK_PRIO      5
#define FLASH_PINS          0xFC0ull    // GPIO6-11
#define DECLARER_UC         "E1"        // X13 declarer: uC family E - ESP32, subtype 1

// A debug build may enable the console (sdkconfig.defaults: none), it must not share the UART
#if CONFIG_ESP_CONSOLE_UART_NUM == CONFIG_ESPNODE_UART_PORT
#error "The console uses the default MQTT-SN UART: choose another ESPNODE_UART_PORT or disable the console"
#endif

typedef struct {
    uint8_t         len;
    uint8_t         msg[MQTTSN_MAX_MSG_LEN];
} rx_msg_t;

static QueueHandle_t s_rx_queue;

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

// Interfaces this version can start: one X13 serial link. WiFi, RS-485 and gateway interfaces - TODO.md
static const char *unsupported(const node_cfg_t *cfg)
{
    if (cfg->n_ifs > 1) {
        return "gateway interfaces not supported yet";
    }
    if (cfg->ifs[0].type != NODE_CFG_IF_UART) {
        return "WiFi not supported yet";
    }
    if (cfg->ifs[0].framing != NODE_CFG_FRAMING_SLIP) {
        return "RS-485 framing not supported yet";
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
                    sizeof(CONFIG_ESPNODE_SW_VERSION)];
    char phy[NODE_CFG_MAX_IFS + 1] = { 0 };

    for (size_t i = 0; i < cfg->n_ifs; i++) {
        const node_cfg_if_t *itf = &cfg->ifs[i];
        phy[i] = (itf->type == NODE_CFG_IF_WIFI) ? 'W' :
                 (itf->framing == NODE_CFG_FRAMING_RS485) ? 'M' : 'S';
    }
    snprintf(buf, sizeof(buf), DECLARER_UC "%s" CONFIG_ESPNODE_HW_VERSION "." CONFIG_ESPNODE_SW_VERSION, phy);
    return buf;
}

// Called from the serial RX task, hands the message over to the node task
static void on_serial_rx(const uint8_t *msg, size_t len, void *arg)
{
    (void)arg;

    rx_msg_t rx = { .len = (uint8_t)len };
    memcpy(rx.msg, msg, len);
    if (xQueueSend(s_rx_queue, &rx, 0) != pdTRUE) {
        ESP_LOGW(TAG, "rx queue full, message dropped");
    }
}

// Owns the MQTT-SN client and the Object Dictionary
static void node_task(void *arg)
{
    (void)arg;

    const mqttsn_client_config_t cfg = {
        .transport = serial_phy_transport(),
        .cb = {
            .on_register        = od_on_register,
            .on_regack          = od_on_regack,
            .on_publish         = od_on_publish,
            .get_client_id      = od_get_client_id,
            .load_addrs         = od_load_addrs,
            .store_node_addr    = od_store_node_addr,
        },
        .now_ms = now_ms,
        .rand16 = rand16,
    };
    mqttsn_client_init(&cfg);

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

        rx_msg_t rx;
        if (xQueueReceive(s_rx_queue, &rx, wait) == pdTRUE) {
            mqttsn_client_handle_rx(NULL, rx.msg, rx.len);  // Serial link, no addresses
        }

        int64_t now = now_ms();
        if (now >= next_poll) {
            od_poll();
            mqttsn_client_tick();

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

    node_cfg_t cfg;
    load_cfg(&cfg);

    // Pins and ports the PLC program must not touch: flash, console, interfaces
    plc_config_t plc_cfg = {
        .reserved_pins  = FLASH_PINS | console_pins(),
        .reserved_uarts = console_uarts(),
    };
    for (size_t i = 0; i < cfg.n_ifs; i++) {
        const node_cfg_if_t *itf = &cfg.ifs[i];
        if (itf->type == NODE_CFG_IF_UART) {
            plc_cfg.reserved_pins |= pin_bit(itf->tx) | pin_bit(itf->rx) | pin_bit(itf->de);
            plc_cfg.reserved_uarts |= (uint8_t)(1u << itf->port);
        }
    }
    ESP_ERROR_CHECK(plc_init(&plc_cfg));

    static const od_provider_t *providers[1];
    providers[0] = plc_provider();
    const od_config_t od_cfg = {
        .device_type    = device_type(&cfg),
        .providers      = providers,
        .n_providers    = 1,
        .cfg_generation = cfg.generation,
        .node_name      = cfg.name,
        .node_id        = cfg.node_addr,
    };
    ESP_ERROR_CHECK(od_init(&od_cfg));

    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_msg_t));
    ESP_ERROR_CHECK(s_rx_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    const node_cfg_if_t *phy = &cfg.ifs[0];
    const serial_phy_config_t phy_cfg = {
        .uart_port  = phy->port,
        .tx_pin     = phy->tx,
        .rx_pin     = phy->rx,
        .de_pin     = (phy->de != NODE_CFG_NO_PIN) ? phy->de : -1,
        .baud_rate  = (int)phy->baud,
    };
    ESP_ERROR_CHECK(serial_phy_init(&phy_cfg, on_serial_rx, NULL));

    BaseType_t ok = xTaskCreate(node_task, "node", NODE_TASK_STACK, NULL, NODE_TASK_PRIO, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
