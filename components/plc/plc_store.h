/*
 * PLC program and settings storage: program in the "plc" data partition, stack bottom in NVS.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Load the program (0xFF - empty if there is none) and the stack bottom (0 if not set)
esp_err_t plc_store_load(uint8_t *prg, size_t size, uint32_t *stack_bottom);

// Write the sectors that differ from the flash
esp_err_t plc_store_save_program(const uint8_t *prg, size_t size);

esp_err_t plc_store_save_stack_bottom(uint32_t words);
