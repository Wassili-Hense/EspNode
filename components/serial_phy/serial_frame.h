/*
 * X13 serial framing: [Length] [escaped message] [0xC0]
 *
 * Length is the length of the contents: the MQTT-SN message, or a FORWARD with the message of a node
 * behind this one (sent as is, always < 0xC0). Message bytes 0xC0 and 0xDB
 * are sent as 0xDB, byte ^ 0x20. Compatible with X13 uart_phy (UART_PHY_SLEEP variant).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mqttsn_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERIAL_FRAME_END            0xC0
#define SERIAL_FRAME_ESC            0xDB
#define SERIAL_FRAME_ESC_XOR        0x20

// Worst case encoded size of a message: length byte, every byte escaped, delimiter
#define SERIAL_FRAME_ENCODED_MAX(len)   (2 + 2 * (len))

typedef struct {
    uint8_t len;                        // Expected message length, 0 - waiting for a frame
    uint8_t pos;                        // Message bytes received
    bool    esc;
    uint8_t buf[MQTTSN_LINK_MAX_LEN];
} serial_frame_decoder_t;

void serial_frame_decoder_reset(serial_frame_decoder_t *dec);

static inline bool serial_frame_decoder_busy(const serial_frame_decoder_t *dec)
{
    return dec->len != 0;
}

// Feed one received byte. Returns the message length when a frame is complete
// (the message is in dec->buf until the next call), 0 otherwise.
size_t serial_frame_decode(serial_frame_decoder_t *dec, uint8_t byte);

// Returns the frame length, 0 if the message length is out of range or out_max is too small
size_t serial_frame_encode(const uint8_t *msg, size_t len, uint8_t *out, size_t out_max);

#ifdef __cplusplus
}
#endif
