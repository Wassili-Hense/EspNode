/*
 * MQTT-SN transport over UART.
 *
 * Based on X13.Home uart_phy, Copyright (c) 2011-2017 <comparator@gmx.de>,
 * BSD 3-Clause License. See LICENSE.X13 for details.
 */
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "serial_frame.h"
#include "serial_phy.h"

static const char *TAG = "serial_phy";

#define ADDR_BROADCAST      0x00
#define ADDR_UNDEF          0xFF

#define RX_BUF_SIZE         512
#define TX_BUF_SIZE         256
#define RX_TIMEOUT_MS       100         // Max pause between bytes of one frame
#define RX_TASK_STACK       3072
#define RX_TASK_PRIO        10

struct serial_phy {
    uart_port_t         port;
    serial_phy_rx_cb_t  rx_cb;
    void               *rx_arg;
    mqttsn_transport_t  transport;
};

static serial_phy_t s_phys[UART_NUM_MAX];      // By port

bool serial_phy_send(serial_phy_t *phy, const uint8_t *msg, size_t len)
{
    uint8_t frame[SERIAL_FRAME_ENCODED_MAX(MQTTSN_LINK_MAX_LEN)];
    size_t n = serial_frame_encode(msg, len, frame, sizeof(frame));
    if (n == 0) {
        return false;
    }
    return uart_write_bytes(phy->port, frame, n) == (int)n;
}

static bool phy_send(void *ctx, const mqttsn_addr_t *dst, const uint8_t *msg, size_t len)
{
    (void)dst;                          // Point-to-point link, no addresses on the wire
    return serial_phy_send(ctx, msg, len);
}

static void rx_task(void *arg)
{
    serial_phy_t *phy = arg;
    serial_frame_decoder_t dec;
    uint8_t chunk[64];
    int64_t last_rx_ms = 0;

    serial_frame_decoder_reset(&dec);

    for (;;) {
        // Wait for the first byte, then take everything already buffered
        int n = uart_read_bytes(phy->port, chunk, 1, pdMS_TO_TICKS(RX_TIMEOUT_MS));
        if (n > 0) {
            size_t avail = 0;
            if ((uart_get_buffered_data_len(phy->port, &avail) == ESP_OK) && (avail > 0)) {
                if (avail > sizeof(chunk) - 1) {
                    avail = sizeof(chunk) - 1;
                }
                int more = uart_read_bytes(phy->port, &chunk[1], avail, 0);
                if (more > 0) {
                    n += more;
                }
            }
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        if (serial_frame_decoder_busy(&dec) && ((now_ms - last_rx_ms) > RX_TIMEOUT_MS)) {
            ESP_LOGD(TAG, "UART%d: rx timeout, frame dropped", phy->port);
            serial_frame_decoder_reset(&dec);
        }
        if (n <= 0) {
            continue;
        }
        last_rx_ms = now_ms;

        for (int i = 0; i < n; i++) {
            size_t len = serial_frame_decode(&dec, chunk[i]);
            if (len > 0) {
                phy->rx_cb(dec.buf, len, phy->rx_arg);
            }
        }
    }
}

esp_err_t serial_phy_init(const serial_phy_config_t *cfg, serial_phy_rx_cb_t rx_cb, void *arg, serial_phy_t **out)
{
    ESP_RETURN_ON_FALSE(rx_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "rx_cb is required");
    ESP_RETURN_ON_FALSE((cfg->uart_port >= 0) && (cfg->uart_port < UART_NUM_MAX), ESP_ERR_INVALID_ARG, TAG,
                        "no UART%d", cfg->uart_port);
    serial_phy_t *phy = &s_phys[cfg->uart_port];
    ESP_RETURN_ON_FALSE(phy->rx_cb == NULL, ESP_ERR_INVALID_STATE, TAG, "UART%d in use", cfg->uart_port);

    const bool rs485 = (cfg->de_pin >= 0);
    const uart_config_t uart_cfg = {
        .baud_rate  = cfg->baud_rate,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    phy->port = (uart_port_t)cfg->uart_port;
    phy->rx_cb = rx_cb;
    phy->rx_arg = arg;
    phy->transport = (mqttsn_transport_t) {
        .ctx        = phy,
        .broadcast  = { .len = 1, .b = { ADDR_BROADCAST } },
        .undef      = { .len = 1, .b = { ADDR_UNDEF } },
        .send       = phy_send,
        .set_addr   = NULL,             // No addresses on the wire
    };

    ESP_RETURN_ON_ERROR(uart_param_config(phy->port, &uart_cfg), TAG, "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(phy->port, cfg->tx_pin, cfg->rx_pin,
                                     rs485 ? cfg->de_pin : UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin");
    // RS485: no TX ring buffer, the driver switches DE right after the FIFO is drained
    ESP_RETURN_ON_ERROR(uart_driver_install(phy->port, RX_BUF_SIZE, rs485 ? 0 : TX_BUF_SIZE, 0, NULL, 0),
                        TAG, "uart_driver_install");
    if (rs485) {
        ESP_RETURN_ON_ERROR(uart_set_mode(phy->port, UART_MODE_RS485_HALF_DUPLEX), TAG, "uart_set_mode");
    }

    char name[] = "serial_rx0";
    name[sizeof(name) - 2] = (char)('0' + cfg->uart_port);
    BaseType_t ok = xTaskCreate(rx_task, name, RX_TASK_STACK, phy, RX_TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "rx task");

    ESP_LOGI(TAG, "UART%d TX=%d RX=%d DE=%d, %d baud", cfg->uart_port, cfg->tx_pin, cfg->rx_pin,
             cfg->de_pin, cfg->baud_rate);
    *out = phy;
    return ESP_OK;
}

const mqttsn_transport_t *serial_phy_transport(serial_phy_t *phy)
{
    return &phy->transport;
}
