/*
 * cfg partition: read from flash.
 */
#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "node_cfg.h"

static const char *TAG = "node_cfg";

#define PARTITION_LABEL     "cfg"

esp_err_t node_cfg_read(node_cfg_t *cfg)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_ANY, PARTITION_LABEL);
    ESP_RETURN_ON_FALSE(part != NULL, ESP_ERR_NOT_FOUND, TAG, "partition '%s' not found",
                        PARTITION_LABEL);

    size_t size = (part->size < NODE_CFG_SIZE) ? part->size : NODE_CFG_SIZE;
    uint8_t *buf = malloc(size);        // Too big for the stack of app_main
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_NO_MEM, TAG, "no memory");

    esp_err_t err = esp_partition_read(part, 0, buf, size);
    if (err == ESP_OK) {
        err = node_cfg_parse(buf, size, cfg);
    }
    free(buf);
    return err;
}
