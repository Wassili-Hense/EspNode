/*
 * MQTT-SN transport over UART.
 *
 * Based on X13.Home uart_phy and rs485_phy, Copyright (c) 2011-2017 <comparator@gmx.de>,
 * BSD 3-Clause License. See LICENSE.X13 for details.
 */
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rs485_frame.h"
#include "serial_frame.h"
#include "serial_phy.h"

static const char *TAG = "serial_phy";

#define RX_BUF_SIZE         512
#define TX_BUF_SIZE         256
#define RX_TIMEOUT_MS       100         // Max pause between bytes of one frame
#define RX_TASK_STACK       3072
#define RX_TASK_PRIO        10

// RS-485: a frame is written at once, the longest takes 3.2 ms at 200000 baud
#define RS485_RX_TIMEOUT_MS 10
// The driver passes received bytes on after this many symbols of silence or this many bytes: the RX task
// sees a busy bus within 0.4 ms at 200000 baud
#define RS485_RX_TOUT       2
#define RS485_RX_FULL       8
#define BUS_IDLE_US         1000        // Silence before sending, X13 waits 200 us
#define BUS_WAIT_MS         50          // The bus stays busy longer: the message is dropped

_Static_assert(RS485_FRAME_SIZE(MQTTSN_LINK_MAX_LEN) <= SERIAL_FRAME_ENCODED_MAX(MQTTSN_LINK_MAX_LEN),
               "frame buffer");

struct serial_phy {
    uart_port_t             port;
    serial_phy_framing_t    framing;
    serial_phy_rx_cb_t      rx_cb;
    void                   *rx_arg;
    mqttsn_transport_t      transport;
    // RS-485, shared by the RX task and the sending task
    volatile uint8_t        addr;           // Own address
    volatile bool           rx_busy;        // Inside a frame
    volatile uint32_t       last_rx_us;     // Time the last bytes were received
};

static serial_phy_t s_phys[UART_NUM_MAX];      // By port

// RS-485: no frame being received, nothing waiting in the driver and silence for BUS_IDLE_US
static bool bus_free(serial_phy_t *phy)
{
    size_t avail = 0;
    uart_get_buffered_data_len(phy->port, &avail);
    return !phy->rx_busy && (avail == 0) && ((uint32_t)esp_timer_get_time() - phy->last_rx_us >= BUS_IDLE_US);
}

bool serial_phy_send(serial_phy_t *phy, uint8_t dst, const uint8_t *msg, size_t len)
{
    uint8_t frame[SERIAL_FRAME_ENCODED_MAX(MQTTSN_LINK_MAX_LEN)];
    size_t n;

    if (phy->framing == SERIAL_PHY_RS485) {
        n = rs485_frame_encode(msg, len, dst, phy->addr, frame, sizeof(frame));
        // No collision detection, as X13: a damaged frame fails the CRC, MQTT-SN repeats the request
        for (int ms = 0; (n > 0) && !bus_free(phy); ms++) {
            if (ms >= BUS_WAIT_MS) {
                ESP_LOGD(TAG, "UART%d: bus busy, frame dropped", phy->port);
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    } else {
        n = serial_frame_encode(msg, len, frame, sizeof(frame));
    }
    if (n == 0) {
        return false;
    }
    return uart_write_bytes(phy->port, frame, n) == (int)n;
}

void serial_phy_set_addr(serial_phy_t *phy, uint8_t addr)
{
    phy->addr = addr;
}

static bool phy_send(void *ctx, const mqttsn_addr_t *dst, const uint8_t *msg, size_t len)
{
    return serial_phy_send(ctx, dst->b[0], msg, len);
}

static void phy_set_addr(void *ctx, const mqttsn_addr_t *addr)
{
    serial_phy_set_addr(ctx, addr->b[0]);
}

static void rx_task(void *arg)
{
    serial_phy_t *phy = arg;
    const bool rs485 = (phy->framing == SERIAL_PHY_RS485);
    const int64_t timeout_us = (rs485 ? RS485_RX_TIMEOUT_MS : RX_TIMEOUT_MS) * 1000;
    union {
        serial_frame_decoder_t  slip;
        rs485_frame_decoder_t   rs485;
    } dec;
    uint8_t chunk[64];
    int64_t last_rx_us = 0;

    serial_frame_decoder_reset(&dec.slip);
    rs485_frame_decoder_reset(&dec.rs485);

    for (;;) {
        // Wait for the first byte, then take everything already buffered
        int n = uart_read_bytes(phy->port, chunk, 1, pdMS_TO_TICKS(timeout_us / 1000));
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

        int64_t now_us = esp_timer_get_time();
        bool busy = rs485 ? rs485_frame_decoder_busy(&dec.rs485) : serial_frame_decoder_busy(&dec.slip);
        if (busy && ((now_us - last_rx_us) > timeout_us)) {
            ESP_LOGD(TAG, "UART%d: rx timeout, frame dropped", phy->port);
            serial_frame_decoder_reset(&dec.slip);
            rs485_frame_decoder_reset(&dec.rs485);
            phy->rx_busy = false;
        }
        if (n <= 0) {
            continue;
        }
        last_rx_us = now_us;
        phy->last_rx_us = (uint32_t)now_us;

        for (int i = 0; i < n; i++) {
            if (rs485) {
                size_t len = rs485_frame_decode(&dec.rs485, chunk[i], phy->addr);
                if (len > 0) {
                    const mqttsn_addr_t src = { .len = 1, .b = { dec.rs485.src } };
                    phy->rx_cb(&src, dec.rs485.buf, len, phy->rx_arg);
                }
            } else {
                size_t len = serial_frame_decode(&dec.slip, chunk[i]);
                if (len > 0) {
                    phy->rx_cb(NULL, dec.slip.buf, len, phy->rx_arg);
                }
            }
        }
        phy->rx_busy = rs485 && rs485_frame_decoder_busy(&dec.rs485);
    }
}

esp_err_t serial_phy_init(const serial_phy_config_t *cfg, serial_phy_rx_cb_t rx_cb, void *arg, serial_phy_t **out)
{
    ESP_RETURN_ON_FALSE(rx_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "rx_cb is required");
    ESP_RETURN_ON_FALSE((cfg->uart_port >= 0) && (cfg->uart_port < UART_NUM_MAX), ESP_ERR_INVALID_ARG, TAG,
                        "no UART%d", cfg->uart_port);
    serial_phy_t *phy = &s_phys[cfg->uart_port];
    ESP_RETURN_ON_FALSE(phy->rx_cb == NULL, ESP_ERR_INVALID_STATE, TAG, "UART%d in use", cfg->uart_port);

    const bool rs485 = (cfg->framing == SERIAL_PHY_RS485);
    const bool half_duplex = (cfg->de_pin >= 0);
    const uart_config_t uart_cfg = {
        .baud_rate  = cfg->baud_rate,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    phy->port = (uart_port_t)cfg->uart_port;
    phy->framing = cfg->framing;
    phy->rx_cb = rx_cb;
    phy->rx_arg = arg;
    phy->addr = cfg->addr;
    phy->rx_busy = false;
    phy->last_rx_us = (uint32_t)esp_timer_get_time();
    phy->transport = (mqttsn_transport_t) {
        .ctx        = phy,
        .broadcast  = { .len = 1, .b = { SERIAL_PHY_ADDR_ALL } },
        .undef      = { .len = 1, .b = { SERIAL_PHY_ADDR_UNDEF } },
        .send       = phy_send,
        .set_addr   = rs485 ? phy_set_addr : NULL,      // Serial: no addresses on the wire
    };

    ESP_RETURN_ON_ERROR(uart_param_config(phy->port, &uart_cfg), TAG, "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(phy->port, cfg->tx_pin, cfg->rx_pin,
                                     half_duplex ? cfg->de_pin : UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin");
    // Half-duplex: no TX ring buffer, the driver switches DE right after the FIFO is drained
    ESP_RETURN_ON_ERROR(uart_driver_install(phy->port, RX_BUF_SIZE, half_duplex ? 0 : TX_BUF_SIZE, 0, NULL, 0),
                        TAG, "uart_driver_install");
    if (half_duplex) {
        ESP_RETURN_ON_ERROR(uart_set_mode(phy->port, UART_MODE_RS485_HALF_DUPLEX), TAG, "uart_set_mode");
    }
    if (rs485) {
        ESP_RETURN_ON_ERROR(uart_set_rx_timeout(phy->port, RS485_RX_TOUT), TAG, "uart_set_rx_timeout");
        ESP_RETURN_ON_ERROR(uart_set_rx_full_threshold(phy->port, RS485_RX_FULL), TAG, "uart_set_rx_full_threshold");
    }

    char name[] = "serial_rx0";
    name[sizeof(name) - 2] = (char)('0' + cfg->uart_port);
    BaseType_t ok = xTaskCreate(rx_task, name, RX_TASK_STACK, phy, RX_TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "rx task");

    ESP_LOGI(TAG, "UART%d TX=%d RX=%d DE=%d, %d baud, %s", cfg->uart_port, cfg->tx_pin, cfg->rx_pin,
             cfg->de_pin, cfg->baud_rate, rs485 ? "RS-485" : "serial");
    *out = phy;
    return ESP_OK;
}

const mqttsn_transport_t *serial_phy_transport(serial_phy_t *phy)
{
    return &phy->transport;
}
