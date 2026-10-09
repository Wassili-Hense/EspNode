/*
 * Persistent settings of the Object Dictionary (NVS namespace "x13").
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "objdict.h"

// Open storage; a missing or different cfg_ver resets everything to defaults
esp_err_t od_storage_init(uint16_t cfg_ver);

// Node name, returns length
size_t    od_storage_name(uint8_t *buf, size_t max);
esp_err_t od_storage_set_name(const uint8_t *name, size_t len);

uint8_t   od_storage_node_id(void);
esp_err_t od_storage_set_node_id(uint8_t id);

uint8_t   od_storage_gate_id(void);
esp_err_t od_storage_set_gate_id(uint8_t id);

// Saved dynamic objects, CONFIG_OD_MAX_OBJECTS slots, free slots have place 0x00 or 0xFF
const od_subidx_t *od_storage_list(size_t *n);
bool      od_storage_slot_free(const od_subidx_t *sidx);
esp_err_t od_storage_list_add(const od_subidx_t *sidx);
esp_err_t od_storage_list_remove(const od_subidx_t *sidx);
