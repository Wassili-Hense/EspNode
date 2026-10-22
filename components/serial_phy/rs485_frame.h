/*
 * X13 RS-485 framing: [0xF0] [0x1C] [LEN] [DST] [SRC] [DATA ...] [CRC]
 *
 * LEN = DATA length + 2, CRC = (LEN + DST + SRC + DATA bytes) & 0xFF, no escaping; DATA is a message or a
 * FORWARD. A node takes frames to its address and to 0 (broadcast); 0xFF is the address of a node without
 * one (DHCP). Compatible with X13 rs485_phy.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mqttsn_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RS485_FRAME_SOF1            0xF0
#define RS485_FRAME_SOF2            0x1C
#define RS485_FRAME_BROADCAST       0x00
#define RS485_FRAME_OVERHEAD        6           // SOF1, SOF2, LEN, DST, SRC, CRC
#define RS485_FRAME_SIZE(len)       ((len) + RS485_FRAME_OVERHEAD)

typedef struct {
    uint8_t state;                      // 0 - waiting for a frame
    uint8_t len;                        // DATA length
    uint8_t pos;                        // DATA bytes received
    uint8_t crc;
    bool    mine;                       // To this node or broadcast
    uint8_t rx_src;
    uint8_t src;                        // Sender of the frame returned last
    uint8_t buf[MQTTSN_LINK_MAX_LEN];
} rs485_frame_decoder_t;

void rs485_frame_decoder_reset(rs485_frame_decoder_t *dec);

// Inside a frame, also one to another node: it is read to its end by LEN
static inline bool rs485_frame_decoder_busy(const rs485_frame_decoder_t *dec)
{
    return dec->state != 0;
}

// Feed one received byte; own - the address of this node. Returns the message length when a frame to own or
// to 0 is complete and its CRC fits (the message is in dec->buf, the sender in dec->src), 0 otherwise.
size_t rs485_frame_decode(rs485_frame_decoder_t *dec, uint8_t byte, uint8_t own);

// Returns the frame length, 0 if the message length is out of range or out_max is too small
size_t rs485_frame_encode(const uint8_t *msg, size_t len, uint8_t dst, uint8_t src, uint8_t *out, size_t out_max);

#ifdef __cplusplus
}
#endif
