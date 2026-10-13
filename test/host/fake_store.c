// Host fake of plc_store.h: program storage in RAM, survives "reboot" of host_node
#include <stdio.h>
#include <string.h>

#include "plc_store.h"

static uint8_t flash[65536];
static int flash_valid;
static uint32_t flash_stack_bottom;

esp_err_t plc_store_load(uint8_t *prg, size_t size, uint32_t *stack_bottom)
{
    memset(prg, 0xFF, size);
    if (flash_valid) {
        memcpy(prg, flash, size);
    }
    *stack_bottom = flash_stack_bottom;
    return ESP_OK;
}

esp_err_t plc_store_save_program(const uint8_t *prg, size_t size)
{
    memcpy(flash, prg, size);
    flash_valid = 1;
    fprintf(stderr, "store program\n");
    return ESP_OK;
}

esp_err_t plc_store_save_stack_bottom(uint32_t words)
{
    flash_stack_bottom = words;
    fprintf(stderr, "store stack %u\n", (unsigned)words);
    return ESP_OK;
}
