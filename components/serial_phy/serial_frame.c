/*
 * X13 serial framing.
 *
 * Based on X13.Home uart_phy, Copyright (c) 2011-2017 <comparator@gmx.de>,
 * BSD 3-Clause License. See LICENSE.X13 for details.
 */
#include "serial_frame.h"

void serial_frame_decoder_reset(serial_frame_decoder_t *dec)
{
    dec->len = 0;
    dec->pos = 0;
    dec->esc = false;
}

size_t serial_frame_decode(serial_frame_decoder_t *dec, uint8_t byte)
{
    if (dec->len == 0) {
        // Waiting for the length byte, everything else is noise
        if ((byte >= 2) && (byte <= MQTTSN_LINK_MAX_LEN)) {
            dec->len = byte;
            dec->pos = 0;
            dec->esc = false;
        }
        return 0;
    }

    if (byte == SERIAL_FRAME_END) {
        size_t len = (dec->pos == dec->len) ? dec->len : 0;     // Drop on length mismatch
        dec->len = 0;
        return len;
    }

    if (byte == SERIAL_FRAME_ESC) {
        dec->esc = true;
        return 0;
    }

    if (dec->esc) {
        dec->esc = false;
        byte ^= SERIAL_FRAME_ESC_XOR;
    }

    // Count overflowing bytes too, the frame is dropped at the delimiter
    if (dec->pos < dec->len) {
        dec->buf[dec->pos] = byte;
    }
    if (dec->pos < UINT8_MAX) {
        dec->pos++;
    }
    return 0;
}

size_t serial_frame_encode(const uint8_t *msg, size_t len, uint8_t *out, size_t out_max)
{
    if ((len < 2) || (len > MQTTSN_LINK_MAX_LEN) || (out_max < 2)) {
        return 0;
    }

    size_t limit = out_max - 1;         // Room for the delimiter
    size_t n = 0;

    out[n++] = (uint8_t)len;
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = msg[i];
        if ((byte == SERIAL_FRAME_END) || (byte == SERIAL_FRAME_ESC)) {
            if (n + 2 > limit) {
                return 0;
            }
            out[n++] = SERIAL_FRAME_ESC;
            out[n++] = byte ^ SERIAL_FRAME_ESC_XOR;
        } else {
            if (n + 1 > limit) {
                return 0;
            }
            out[n++] = byte;
        }
    }
    out[n++] = SERIAL_FRAME_END;
    return n;
}
