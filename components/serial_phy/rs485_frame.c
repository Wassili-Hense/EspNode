/*
 * X13 RS-485 framing.
 *
 * Based on X13.Home rs485_phy, Copyright (c) 2011-2017 <comparator@gmx.de>,
 * BSD 3-Clause License. See LICENSE.X13 for details.
 */
#include "rs485_frame.h"

enum {
    RX_IDLE = 0,                        // rs485_frame_decoder_busy()
    RX_SOF2,
    RX_LEN,
    RX_DST,
    RX_SRC,
    RX_DATA,
};

void rs485_frame_decoder_reset(rs485_frame_decoder_t *dec)
{
    dec->state = RX_IDLE;
}

size_t rs485_frame_decode(rs485_frame_decoder_t *dec, uint8_t byte, uint8_t own)
{
    switch (dec->state) {
    case RX_IDLE:                       // Noise between frames is ignored
        if (byte == RS485_FRAME_SOF1) {
            dec->state = RX_SOF2;
        }
        break;

    case RX_SOF2:
        dec->state = (byte == RS485_FRAME_SOF2) ? RX_LEN : (byte == RS485_FRAME_SOF1) ? RX_SOF2 : RX_IDLE;
        break;

    case RX_LEN:
        if ((byte < 4) || (byte > MQTTSN_LINK_MAX_LEN + 2)) {
            dec->state = RX_IDLE;       // X13: bad length, arbitration lost
            break;
        }
        dec->len = (uint8_t)(byte - 2);
        dec->crc = byte;
        dec->state = RX_DST;
        break;

    case RX_DST:
        dec->mine = (byte == own) || (byte == RS485_FRAME_BROADCAST);
        dec->crc += byte;
        dec->state = RX_SRC;
        break;

    case RX_SRC:
        dec->rx_src = byte;
        dec->crc += byte;
        dec->pos = 0;
        dec->state = RX_DATA;
        break;

    case RX_DATA:
        if (dec->pos < dec->len) {
            dec->buf[dec->pos++] = byte;
            dec->crc += byte;
            break;
        }
        dec->state = RX_IDLE;           // CRC byte, the frame ends
        if (dec->mine && (byte == dec->crc)) {
            dec->src = dec->rx_src;
            return dec->len;
        }
        break;

    default:
        dec->state = RX_IDLE;
        break;
    }
    return 0;
}

size_t rs485_frame_encode(const uint8_t *msg, size_t len, uint8_t dst, uint8_t src, uint8_t *out, size_t out_max)
{
    if ((len < 2) || (len > MQTTSN_LINK_MAX_LEN) || (out_max < RS485_FRAME_SIZE(len))) {
        return 0;
    }

    uint8_t crc = (uint8_t)(len + 2 + dst + src);
    out[0] = RS485_FRAME_SOF1;
    out[1] = RS485_FRAME_SOF2;
    out[2] = (uint8_t)(len + 2);
    out[3] = dst;
    out[4] = src;
    for (size_t i = 0; i < len; i++) {
        out[5 + i] = msg[i];
        crc += msg[i];
    }
    out[5 + len] = crc;
    return RS485_FRAME_SIZE(len);
}
