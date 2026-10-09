/*
 * Packing of numeric values in PUBLISH payloads: little-endian, redundant high bytes stripped.
 */
#pragma once

#include <stdint.h>

#include "objdict.h"

#define OD_PACK_SIGNED      0x80
#define OD_PACK_SIZE_MASK   0x7F

// Packing descriptor of an object: native size | OD_PACK_SIGNED, 0 - sent as is
uint8_t od_pack_width(const od_subidx_t *sidx);

// Strip redundant high bytes of a native value; buf must hold width + 1 bytes.
// Returns the packed length.
uint8_t od_pack(uint8_t width, uint8_t *buf, uint8_t len);

// Extend a packed value to the native size; buf must hold width bytes.
// Returns the native length.
uint8_t od_unpack(uint8_t width, uint8_t *buf, uint8_t len);
