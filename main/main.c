/*
 * EspNode: X13 MQTT-SN node over a serial link.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "mqttsn_client.h"
#include "objdict.h"
#include "serial_phy.h"

static const char *TAG = "espnode";

#define POLL_PERIOD_MS      10          // X13: POLL_TMR_FREQ = 100 Hz
#define RX_QUEUE_LEN        8
#define NODE_TASK_STACK     4096
#define NODE_TASK_PRIO      5

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

    static const od_config_t od_cfg = {
        .device_type    = CONFIG_ESPNODE_DEVICE_TYPE "." CONFIG_ESPNODE_SW_VERSION,
        .providers      = NULL,
        .n_providers    = 0,
    };
    ESP_ERROR_CHECK(od_init(&od_cfg));

    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_msg_t));
    ESP_ERROR_CHECK(s_rx_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    const serial_phy_config_t phy_cfg = {
        .uart_port  = CONFIG_ESPNODE_UART_PORT,
        .tx_pin     = CONFIG_ESPNODE_UART_TX_PIN,
        .rx_pin     = CONFIG_ESPNODE_UART_RX_PIN,
        .de_pin     = CONFIG_ESPNODE_UART_DE_PIN,
        .baud_rate  = CONFIG_ESPNODE_UART_BAUD,
    };
    ESP_ERROR_CHECK(serial_phy_init(&phy_cfg, on_serial_rx, NULL));

    BaseType_t ok = xTaskCreate(node_task, "node", NODE_TASK_STACK, NULL, NODE_TASK_PRIO, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
