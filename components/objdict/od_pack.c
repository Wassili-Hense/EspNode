/*
 * Packing of numeric values in PUBLISH payloads.
 *
 * Based on X13.Home ObjDict (ReadODpack/WriteODpack), Copyright (c) 2011-2017 <comparator@gmx.de>,
 * BSD 3-Clause License. See LICENSE.X13 for details.
 */
#include "od_pack.h"

uint8_t od_pack_width(const od_subidx_t *sidx)
{
    switch (sidx->place) {
    case OD_PLACE_AIN:
        return 2;
    case OD_PLACE_PWM:
        return (sidx->type == OD_TYPE_PWM_FREQ) ? 4 : 2;
    case OD_PLACE_DIN:
    case OD_PLACE_DOUT:
    case OD_PLACE_SER:
        return 0;
    default:
        break;
    }

    switch (sidx->type) {
    case OD_TYPE_UINT8:     return 1;
    case OD_TYPE_INT16:     return OD_PACK_SIGNED | 2;
    case OD_TYPE_UINT16:    return 2;
    case OD_TYPE_INT32:     return OD_PACK_SIGNED | 4;
    case OD_TYPE_UINT32:    return 4;
    case OD_TYPE_INT64:     return OD_PACK_SIGNED | 8;
    case OD_TYPE_FLOAT:     return 0;   // Always 4 bytes, not packed
    default:                return 0;
    }
}

uint8_t od_pack(uint8_t width, uint8_t *buf, uint8_t len)
{
    uint8_t n = width & OD_PACK_SIZE_MASK;

    if (n == 0) {
        return len;
    }

    if (width & OD_PACK_SIGNED) {
        // Drop high bytes that only repeat the sign
        while ((n > 1) &&
               (((buf[n - 1] == 0x00) && ((buf[n - 2] & 0x80) == 0)) ||
                ((buf[n - 1] == 0xFF) && ((buf[n - 2] & 0x80) != 0)))) {
            n--;
        }
    } else if (buf[n - 1] & 0x80) {
        buf[n++] = 0;               // Keep the value positive
    } else {
        while ((n > 1) && (buf[n - 1] == 0) && ((buf[n - 2] & 0x80) == 0)) {
            n--;
        }
    }
    return n;
}

uint8_t od_unpack(uint8_t width, uint8_t *buf, uint8_t len)
{
    uint8_t n = width & OD_PACK_SIZE_MASK;

    if (n == 0) {
        return len;
    }

    if (len < n) {
        uint8_t fill = ((len > 0) && (buf[len - 1] & 0x80)) ? 0xFF : 0x00;
        while (len < n) {
            buf[len++] = fill;
        }
    }
    return n;
}
