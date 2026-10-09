/*
 * Persistent settings of the Object Dictionary, NVS backed with a RAM copy.
 */
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "mqttsn_defs.h"
#include "od_storage.h"

static const char *TAG = "od_storage";

#define NVS_NAMESPACE       "x13"
#define KEY_CFG_VER         "cfg_ver"
#define KEY_NAME            "name"
#define KEY_NODE_ID         "node_id"
#define KEY_GATE_ID         "gate_id"
#define KEY_OD_LIST         "od_list"

#define ADDR_UNDEF          0xFF

static struct {
    nvs_handle_t    nvs;
    uint8_t         name[MQTTSN_CLIENT_ID_MAX];
    uint8_t         name_len;
    uint8_t         node_id;
    uint8_t         gate_id;
    od_subidx_t     list[CONFIG_OD_MAX_OBJECTS];
} s;

static esp_err_t commit(esp_err_t err)
{
    if (err == ESP_OK) {
        err = nvs_commit(s.nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t save_list(void)
{
    return commit(nvs_set_blob(s.nvs, KEY_OD_LIST, s.list, sizeof(s.list)));
}

static esp_err_t load_defaults(uint16_t cfg_ver)
{
    ESP_LOGW(TAG, "no valid configuration, loading defaults");

    s.name_len = 0;
    s.node_id = ADDR_UNDEF;             // Request address via DHCP
    s.gate_id = ADDR_UNDEF;             // Search gateway
    memset(s.list, 0xFF, sizeof(s.list));

    esp_err_t err = nvs_erase_all(s.nvs);       // Empty name is stored as no key
    if (err == ESP_OK) {
        err = nvs_set_u8(s.nvs, KEY_NODE_ID, s.node_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(s.nvs, KEY_GATE_ID, s.gate_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(s.nvs, KEY_OD_LIST, s.list, sizeof(s.list));
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(s.nvs, KEY_CFG_VER, cfg_ver);
    }
    return commit(err);
}

esp_err_t od_storage_init(uint16_t cfg_ver)
{
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s.nvs), TAG, "nvs_open");

    uint16_t ver = 0;
    if ((nvs_get_u16(s.nvs, KEY_CFG_VER, &ver) != ESP_OK) || (ver != cfg_ver)) {
        return load_defaults(cfg_ver);
    }

    size_t len = sizeof(s.name);
    if (nvs_get_blob(s.nvs, KEY_NAME, s.name, &len) != ESP_OK) {
        len = 0;
    }
    s.name_len = (uint8_t)len;

    if (nvs_get_u8(s.nvs, KEY_NODE_ID, &s.node_id) != ESP_OK) {
        s.node_id = ADDR_UNDEF;
    }
    if (nvs_get_u8(s.nvs, KEY_GATE_ID, &s.gate_id) != ESP_OK) {
        s.gate_id = ADDR_UNDEF;
    }

    // A shorter blob (CONFIG_OD_MAX_OBJECTS increased) leaves the tail free
    memset(s.list, 0xFF, sizeof(s.list));
    len = sizeof(s.list);
    esp_err_t err = nvs_get_blob(s.nvs, KEY_OD_LIST, s.list, &len);
    if ((err != ESP_OK) && (err != ESP_ERR_NVS_NOT_FOUND)) {
        ESP_LOGW(TAG, "object list dropped: %s", esp_err_to_name(err));
        memset(s.list, 0xFF, sizeof(s.list));
    }
    return ESP_OK;
}

size_t od_storage_name(uint8_t *buf, size_t max)
{
    size_t len = (s.name_len < max) ? s.name_len : max;
    memcpy(buf, s.name, len);
    return len;
}

esp_err_t od_storage_set_name(const uint8_t *name, size_t len)
{
    ESP_RETURN_ON_FALSE(len <= sizeof(s.name), ESP_ERR_INVALID_SIZE, TAG, "name too long");

    memcpy(s.name, name, len);
    s.name_len = (uint8_t)len;

    esp_err_t err;
    if (len > 0) {
        err = nvs_set_blob(s.nvs, KEY_NAME, s.name, len);
    } else {
        err = nvs_erase_key(s.nvs, KEY_NAME);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    return commit(err);
}

uint8_t od_storage_node_id(void)
{
    return s.node_id;
}

esp_err_t od_storage_set_node_id(uint8_t id)
{
    s.node_id = id;
    return commit(nvs_set_u8(s.nvs, KEY_NODE_ID, id));
}

uint8_t od_storage_gate_id(void)
{
    return s.gate_id;
}

esp_err_t od_storage_set_gate_id(uint8_t id)
{
    s.gate_id = id;
    return commit(nvs_set_u8(s.nvs, KEY_GATE_ID, id));
}

const od_subidx_t *od_storage_list(size_t *n)
{
    *n = CONFIG_OD_MAX_OBJECTS;
    return s.list;
}

bool od_storage_slot_free(const od_subidx_t *sidx)
{
    return (sidx->place == 0x00) || (sidx->place == 0xFF);
}

esp_err_t od_storage_list_add(const od_subidx_t *sidx)
{
    od_subidx_t *free_slot = NULL;

    for (size_t i = 0; i < CONFIG_OD_MAX_OBJECTS; i++) {
        if (memcmp(&s.list[i], sidx, sizeof(*sidx)) == 0) {
            return ESP_OK;              // Already saved
        }
        if ((free_slot == NULL) && od_storage_slot_free(&s.list[i])) {
            free_slot = &s.list[i];
        }
    }
    ESP_RETURN_ON_FALSE(free_slot != NULL, ESP_ERR_NO_MEM, TAG, "object list full");

    *free_slot = *sidx;
    return save_list();
}

esp_err_t od_storage_list_remove(const od_subidx_t *sidx)
{
    for (size_t i = 0; i < CONFIG_OD_MAX_OBJECTS; i++) {
        if (memcmp(&s.list[i], sidx, sizeof(*sidx)) == 0) {
            memset(&s.list[i], 0xFF, sizeof(s.list[i]));
            return save_list();
        }
    }
    return ESP_OK;
}
