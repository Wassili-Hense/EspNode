/*
 * PLC program and settings storage.
 */
#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"

#include "plc_store.h"

static const char *TAG = "plc_store";

#define PARTITION_LABEL     "plc"
#define NVS_NAMESPACE       "plc"
#define KEY_STACK_BOTTOM    "stack_bot"
#define CHUNK_SIZE          256

static const esp_partition_t *s_part;

esp_err_t plc_store_load(uint8_t *prg, size_t size, uint32_t *stack_bottom)
{
    memset(prg, 0xFF, size);            // Empty program: RET
    *stack_bottom = 0;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, KEY_STACK_BOTTOM, stack_bottom);
        nvs_close(nvs);
    }

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                      PARTITION_LABEL);
    ESP_RETURN_ON_FALSE(s_part != NULL, ESP_ERR_NOT_FOUND, TAG, "partition '%s' not found",
                        PARTITION_LABEL);
    ESP_RETURN_ON_FALSE(s_part->size >= size, ESP_ERR_INVALID_SIZE, TAG,
                        "partition '%s' smaller than the program memory", PARTITION_LABEL);

    esp_err_t err = esp_partition_read(s_part, 0, prg, size);
    if (err != ESP_OK) {
        memset(prg, 0xFF, size);
        ESP_LOGE(TAG, "program read failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t plc_store_save_program(const uint8_t *prg, size_t size)
{
    ESP_RETURN_ON_FALSE(s_part != NULL, ESP_ERR_NOT_FOUND, TAG, "no partition");

    const size_t sector = s_part->erase_size;
    uint8_t buf[CHUNK_SIZE];
    unsigned written = 0;

    for (size_t offs = 0; offs < size; offs += sector) {
        size_t len = (size - offs < sector) ? (size - offs) : sector;

        bool same = true;
        for (size_t pos = 0; same && (pos < len); pos += CHUNK_SIZE) {
            size_t n = (len - pos < CHUNK_SIZE) ? (len - pos) : CHUNK_SIZE;
            ESP_RETURN_ON_ERROR(esp_partition_read(s_part, offs + pos, buf, n), TAG, "read");
            same = (memcmp(buf, &prg[offs + pos], n) == 0);
        }
        if (same) {
            continue;
        }

        ESP_RETURN_ON_ERROR(esp_partition_erase_range(s_part, offs, sector), TAG, "erase");
        ESP_RETURN_ON_ERROR(esp_partition_write(s_part, offs, &prg[offs], len), TAG, "write");
        written++;
    }

    ESP_LOGI(TAG, "program saved, %u sectors written", written);
    return ESP_OK;
}

esp_err_t plc_store_save_stack_bottom(uint32_t words)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open");

    esp_err_t err = nvs_set_u32(nvs, KEY_STACK_BOTTOM, words);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}
