/*
 * X13 Object Dictionary.
 *
 * Based on X13.Home device firmware (ObjDict, ext), Copyright (c) 2011-2017 <comparator@gmx.de>,
 * BSD 3-Clause License. See LICENSE.X13 for details.
 */
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "mqttsn_client.h"
#include "objdict.h"
#include "od_clock.h"
#include "od_pack.h"
#include "od_storage.h"

static const char *TAG = "objdict";

#define OD_MAX              CONFIG_OD_MAX_OBJECTS
#define STORAGE_LAYOUT      1           // Bump to reset stored settings of all devices
#define SESSION_DONE        0xFFFF      // Session position: device info sent, subscribe next
#define NO_RECORD           (-1)

_Static_assert(OD_MAX < OD_ID_RTC, "Session position overlaps predefined ids");

static struct {
    const char                 *device_type;
    const char                 *name_suffix;
    size_t                    (*phy1_addr)(uint8_t *buf, size_t max);
    const od_provider_t *const *providers;
    size_t                      n_providers;
    od_entry_t                  list[OD_MAX];       // Dynamic objects
    uint16_t                    idx;                // Session / poll position (X13: idxUpdate)
    int                         reg_rec;            // Record waiting for REGACK
} s;

////////////////////////////////////////////////////////////////////////
// Predefined objects

static uint8_t read_node_name(const od_subidx_t *sidx, uint8_t *len, uint8_t *buf)
{
    (void)sidx;
    *len = (uint8_t)od_storage_name(buf, *len);
    return MQTTSN_RC_ACCEPTED;
}

static uint8_t write_node_name(const od_subidx_t *sidx, uint8_t len, const uint8_t *buf)
{
    (void)sidx;
    if (len > MQTTSN_CLIENT_ID_MAX) {
        return MQTTSN_RC_REJ_NOT_SUPP;
    }
    ESP_LOGI(TAG, "node name: '%.*s'", len, (const char *)buf);
    return (od_storage_set_name(buf, len) == ESP_OK) ? MQTTSN_RC_ACCEPTED : MQTTSN_RC_REJ_NOT_SUPP;
}

static uint8_t read_node_id(const od_subidx_t *sidx, uint8_t *len, uint8_t *buf)
{
    (void)sidx;
    if (*len < 1) {
        return MQTTSN_RC_REJ_NOT_SUPP;
    }
    buf[0] = od_storage_node_id();
    *len = 1;
    return MQTTSN_RC_ACCEPTED;
}

static uint8_t write_node_id(const od_subidx_t *sidx, uint8_t len, const uint8_t *buf)
{
    (void)sidx;
    if (len < 1) {
        return MQTTSN_RC_REJ_NOT_SUPP;
    }
    ESP_LOGI(TAG, "node id: 0x%02X, applied on reconnect", buf[0]);
    return (od_storage_set_node_id(buf[0]) == ESP_OK) ? MQTTSN_RC_ACCEPTED : MQTTSN_RC_REJ_NOT_SUPP;
}

static uint8_t read_gate_id(const od_subidx_t *sidx, uint8_t *len, uint8_t *buf)
{
    (void)sidx;
    if (*len < 1) {
        return MQTTSN_RC_REJ_NOT_SUPP;
    }
    buf[0] = od_storage_gate_id();
    *len = 1;
    return MQTTSN_RC_ACCEPTED;
}

static uint8_t write_gate_id(const od_subidx_t *sidx, uint8_t len, const uint8_t *buf)
{
    (void)sidx;
    if (len < 1) {
        return MQTTSN_RC_REJ_NOT_SUPP;
    }
    ESP_LOGI(TAG, "gateway id: 0x%02X, applied on reconnect", buf[0]);
    return (od_storage_set_gate_id(buf[0]) == ESP_OK) ? MQTTSN_RC_ACCEPTED : MQTTSN_RC_REJ_NOT_SUPP;
}

// PHY1 address: the 1-byte X13 address, or that of the link (WiFi: IPv4, as X13 LAN nodes)
static uint8_t read_phy1_addr(const od_subidx_t *sidx, uint8_t *len, uint8_t *buf)
{
    if (s.phy1_addr == NULL) {
        return read_node_id(sidx, len, buf);
    }
    *len = (uint8_t)s.phy1_addr(buf, *len);
    return MQTTSN_RC_ACCEPTED;
}

static uint8_t read_device_type(const od_subidx_t *sidx, uint8_t *len, uint8_t *buf)
{
    (void)sidx;
    size_t n = strlen(s.device_type);
    if (n > *len) {
        n = *len;
    }
    memcpy(buf, s.device_type, n);
    *len = (uint8_t)n;
    return MQTTSN_RC_ACCEPTED;
}

// X13 RTC: Enviriot learns from the publication at connect that the node keeps time, and writes
// its local time when the node is more than 2 s off, then every hour
static uint8_t read_rtc(const od_subidx_t *sidx, uint8_t *len, uint8_t *buf)
{
    (void)sidx;
    if (*len < OD_CLOCK_BCD_SIZE) {
        return MQTTSN_RC_REJ_NOT_SUPP;
    }
    od_clock_get_bcd(buf);
    *len = OD_CLOCK_BCD_SIZE;
    return MQTTSN_RC_ACCEPTED;
}

static uint8_t write_rtc(const od_subidx_t *sidx, uint8_t len, const uint8_t *buf)
{
    (void)sidx;
    if ((len != OD_CLOCK_BCD_SIZE) || !od_clock_set_bcd(buf)) {
        return MQTTSN_RC_REJ_NOT_SUPP;
    }
    od_clock_t c;
    od_clock_now(&c);
    ESP_LOGI(TAG, "clock: %04u-%02u-%02u %02u:%02u:%02u", c.year, c.month, c.day, c.hour, c.min, c.sec);
    return MQTTSN_RC_ACCEPTED;
}

// X13 PLC nodes disable polling of inputs used by the PLC program. Inputs are
// owned by the PLC here, there is nothing to mute.
static uint8_t write_in_mute(const od_subidx_t *sidx, uint8_t len, const uint8_t *buf)
{
    (void)sidx;
    (void)len;
    (void)buf;
    return MQTTSN_RC_ACCEPTED;
}

static const od_entry_t s_predef[] = {
    { { OD_PLACE_EEMEM,   OD_TYPE_STRING, 0 }, OD_ID_NODE_NAME,   read_node_name,   write_node_name, NULL },
    { { OD_PLACE_EEMEM,   OD_TYPE_ARRAY,  0 }, OD_ID_RTC,         read_rtc,         write_rtc,       NULL },
    { { OD_PLACE_EEMEM,   OD_TYPE_ARRAY,  0 }, OD_ID_IN_MUTE,     NULL,             write_in_mute,   NULL },
    { { OD_PLACE_EEMEM,   OD_TYPE_UINT8,  0 }, OD_ID_NODE_ID,     read_node_id,     write_node_id,   NULL },
    { { OD_PLACE_EEMEM,   OD_TYPE_UINT8,  0 }, OD_ID_GATE_ID,     read_gate_id,     write_gate_id,   NULL },
    { { OD_PLACE_PROGMEM, OD_TYPE_STRING, 0 }, OD_ID_DEVICE_TYPE, read_device_type, NULL,            NULL },
    { { OD_PLACE_PROGMEM, OD_TYPE_ARRAY,  1 }, OD_ID_PHY1_ADDR,   read_phy1_addr,   NULL,            NULL },
};

////////////////////////////////////////////////////////////////////////
// Helpers

static const od_entry_t *find_entry(uint16_t id, uint8_t flags)
{
    switch (flags & MQTTSN_FL_TOPICID_MASK) {
    case MQTTSN_FL_TOPICID_NORM:
        if ((id == OD_TOPIC_NEW) || (id == OD_TOPIC_FREE)) {
            break;
        }
        for (size_t i = 0; i < OD_MAX; i++) {
            if (s.list[i].topic_id == id) {
                return &s.list[i];
            }
        }
        break;

    case MQTTSN_FL_TOPICID_PREDEF:
        for (size_t i = 0; i < sizeof(s_predef) / sizeof(s_predef[0]); i++) {
            if (s_predef[i].topic_id == id) {
                return &s_predef[i];
            }
        }
        break;

    default:
        break;
    }
    return NULL;
}

static const od_provider_t *find_provider(uint8_t place)
{
    for (size_t i = 0; i < s.n_providers; i++) {
        if (s.providers[i]->place == place) {
            return s.providers[i];
        }
    }
    return NULL;
}

static uint8_t attach_entry(od_entry_t *entry)
{
    const od_provider_t *p = find_provider(entry->sidx.place);

    entry->read = NULL;
    entry->write = NULL;
    entry->poll = NULL;
    return (p != NULL) ? p->attach(entry) : MQTTSN_RC_REJ_NOT_SUPP;
}

static void delete_entry(size_t rec)
{
    od_entry_t *entry = &s.list[rec];
    const od_provider_t *p = find_provider(entry->sidx.place);

    entry->topic_id = OD_TOPIC_FREE;
    od_storage_list_remove(&entry->sidx);
    if ((p != NULL) && (p->detach != NULL)) {
        p->detach(&entry->sidx);
    }
}

// Topic name: place, type, decimal base (X13: MakeTopicName)
static size_t make_topic_name(const od_subidx_t *sidx, uint8_t *buf, size_t max)
{
    char tmp[8];
    int n = snprintf(tmp, sizeof(tmp), "%c%c%u", sidx->place, sidx->type, (unsigned)sidx->base);
    if ((n < 0) || ((size_t)n > max)) {
        return 0;
    }
    memcpy(buf, tmp, (size_t)n);
    return (size_t)n;
}

static void register_entry(size_t rec)
{
    uint8_t name[8];
    size_t len = make_topic_name(&s.list[rec].sidx, name, sizeof(name));

    if ((len > 0) && mqttsn_client_register(s.list[rec].topic_id, name, len)) {
        s.reg_rec = (int)rec;
    }
}

static void publish_entry(const od_entry_t *entry, uint8_t flags)
{
    uint8_t data[MQTTSN_MSG_SIZE - 5];
    uint8_t len = sizeof(data) - 1;     // Room for the packing sign byte

    if ((entry->read == NULL) || (entry->read(&entry->sidx, &len, data) != MQTTSN_RC_ACCEPTED)) {
        len = 0;                        // Publish empty payload as X13 does
    } else {
        len = od_pack(od_pack_width(&entry->sidx), data, len);
    }

    mqttsn_client_publish(flags, entry->topic_id, data, len);
}

// Settings are reset when the device type changes (X13: "Flag" in InitOD). Counted are the uC family
// and subtype (first two characters of the declarer) and the HW version (last two). Excluded are the
// software version after '.', settings survive updates, and the PHY letters between: they follow the
// interfaces of cfg, a new cfg resets the name and addresses but keeps the objects.
static uint16_t cfg_version(const char *device_type)
{
    size_t len = strcspn(device_type, ".");
    uint16_t ver = STORAGE_LAYOUT << 12;
    for (size_t i = 0; i < len; i++) {
        if ((i < 2) || (i >= len - 2)) {
            ver += (uint8_t)device_type[i];
        }
    }
    return ver;
}

////////////////////////////////////////////////////////////////////////
// Session

// PRE_CONNECT: restore registrations, publish device info, subscribe (X13: OD_Poll)
static void session_step(void)
{
    if (s.idx < OD_MAX) {
        if (s.list[s.idx].topic_id < OD_TOPIC_PREDEF_MIN) {
            if (mqttsn_client_can_send()) {
                register_entry(s.idx);          // REGACK advances idx
            }
        } else {
            s.idx++;
        }
    } else if (s.idx != SESSION_DONE) {
        // Device info: RTC, then the objects from the device type on (X13: objRTC, objDeviceTyp)
        if (s.idx < OD_ID_RTC) {
            s.idx = OD_ID_RTC;
        } else if ((s.idx > OD_ID_RTC) && (s.idx < OD_ID_DEVICE_TYPE)) {
            s.idx = OD_ID_DEVICE_TYPE;
        }
        if (mqttsn_client_can_send()) {
            const od_entry_t *entry = find_entry(s.idx, MQTTSN_FL_TOPICID_PREDEF);
            if (entry != NULL) {
                publish_entry(entry, MQTTSN_FL_QOS1 | MQTTSN_FL_RETAIN | MQTTSN_FL_TOPICID_PREDEF);
                s.idx++;
            } else {
                s.idx = SESSION_DONE;
                for (size_t i = 0; i < s.n_providers; i++) {
                    if (s.providers[i]->on_session != NULL) {
                        s.providers[i]->on_session();
                    }
                }
            }
        }
    } else if (mqttsn_client_can_send()) {
        static const uint8_t topic[] = { '#' };
        mqttsn_client_subscribe(MQTTSN_FL_QOS1 | MQTTSN_FL_TOPICID_NORM, topic, sizeof(topic));
    }
}

// CONNECT: register new objects, publish changed values
static void poll_objects(void)
{
    if (s.idx >= OD_MAX) {
        s.idx = 0;
    }

    while (mqttsn_client_can_send() && (s.idx < OD_MAX)) {
        od_entry_t *entry = &s.list[s.idx];

        if (entry->topic_id == OD_TOPIC_NEW) {
            register_entry(s.idx);
        } else if ((entry->topic_id != OD_TOPIC_FREE) && (entry->poll != NULL) &&
                   entry->poll(&entry->sidx)) {
            uint8_t flags = (entry->topic_id < OD_TOPIC_PREDEF_MIN)
                            ? (MQTTSN_FL_QOS1 | MQTTSN_FL_TOPICID_NORM)
                            : (MQTTSN_FL_QOS0 | MQTTSN_FL_TOPICID_PREDEF);
            publish_entry(entry, flags);
        }
        s.idx++;
    }
}

////////////////////////////////////////////////////////////////////////
// API

esp_err_t od_init(const od_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg->device_type != NULL, ESP_ERR_INVALID_ARG, TAG, "device_type");

    memset(&s, 0, sizeof(s));
    s.device_type = cfg->device_type;
    s.name_suffix = cfg->name_suffix;
    s.phy1_addr = cfg->phy1_addr;
    s.providers = cfg->providers;
    s.n_providers = cfg->n_providers;
    s.reg_rec = NO_RECORD;

    ESP_RETURN_ON_ERROR(od_storage_init(cfg_version(cfg->device_type), cfg), TAG, "storage");

    for (size_t i = 0; i < OD_MAX; i++) {
        s.list[i].topic_id = OD_TOPIC_FREE;
    }

    for (size_t i = 0; i < s.n_providers; i++) {
        if (s.providers[i]->init != NULL) {
            s.providers[i]->init();
        }
    }

    // Restore saved objects into slots left by the providers, topic ids are assigned
    // in the next session
    size_t n;
    const od_subidx_t *saved = od_storage_list(&n);
    size_t restored = 0;
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        if (od_storage_slot_free(&saved[i])) {
            continue;
        }
        while ((pos < OD_MAX) && (s.list[pos].topic_id != OD_TOPIC_FREE)) {
            pos++;
        }
        if (pos == OD_MAX) {
            ESP_LOGW(TAG, "object table full");
            break;
        }

        s.list[pos].sidx = saved[i];
        if (attach_entry(&s.list[pos]) != MQTTSN_RC_ACCEPTED) {
            ESP_LOGW(TAG, "saved object %c%c%u not supported", saved[i].place, saved[i].type,
                     (unsigned)saved[i].base);
            continue;
        }
        s.list[pos].topic_id = OD_TOPIC_NEW;
        restored++;
    }

    ESP_LOGI(TAG, "device %s, %u objects restored", s.device_type, (unsigned)restored);
    return ESP_OK;
}

od_entry_t *od_add_object(const od_subidx_t *sidx)
{
    for (size_t i = 0; i < OD_MAX; i++) {
        od_entry_t *entry = &s.list[i];
        if (entry->topic_id == OD_TOPIC_FREE) {
            entry->sidx = *sidx;
            entry->topic_id = OD_TOPIC_NEW;
            entry->read = NULL;
            entry->write = NULL;
            entry->poll = NULL;
            return entry;
        }
    }
    return NULL;
}

void od_poll(void)
{
    for (size_t i = 0; i < s.n_providers; i++) {
        if (s.providers[i]->proc != NULL) {
            s.providers[i]->proc();
        }
    }

    switch (mqttsn_client_status()) {
    case MQTTSN_STATUS_CONNECT:
        poll_objects();
        break;
    case MQTTSN_STATUS_PRE_CONNECT:
        session_step();
        break;
    default:
        s.idx = 0;
        s.reg_rec = NO_RECORD;
        break;
    }
}

// REGISTER from gateway, topic name "<place><type><base>" (X13: RegisterOD)
uint8_t od_on_register(uint16_t topic_id, const uint8_t *name, size_t len)
{
    if ((len < 3) || (len > 7)) {       // place, type, 1..5 digits
        return MQTTSN_RC_REJ_NOT_SUPP;
    }

    od_subidx_t sidx = { .place = name[0], .type = name[1], .base = 0 };
    uint32_t base = 0;
    for (size_t i = 2; i < len; i++) {
        if ((name[i] < '0') || (name[i] > '9')) {
            return MQTTSN_RC_REJ_NOT_SUPP;
        }
        base = base * 10 + (name[i] - '0');
    }
    if (base > UINT16_MAX) {
        return MQTTSN_RC_REJ_NOT_SUPP;
    }
    sidx.base = (uint16_t)base;

    const od_provider_t *p = find_provider(sidx.place);
    if ((p == NULL) || !p->check(&sidx)) {
        ESP_LOGW(TAG, "register %.*s: not supported", (int)len, (const char *)name);
        return MQTTSN_RC_REJ_NOT_SUPP;
    }

    // Existing object or the first free slot
    int rec = NO_RECORD;
    for (size_t i = 0; i < OD_MAX; i++) {
        if (s.list[i].topic_id == OD_TOPIC_FREE) {
            if (rec == NO_RECORD) {
                rec = (int)i;
            }
        } else if (memcmp(&s.list[i].sidx, &sidx, sizeof(sidx)) == 0) {
            rec = (int)i;
            break;
        } else if (s.list[i].topic_id == topic_id) {
            return MQTTSN_RC_REJ_INV_ID;            // Topic id in use by another object
        }
    }
    if (rec == NO_RECORD) {
        return MQTTSN_RC_REJ_CONG;                  // Table is full
    }

    od_entry_t *entry = &s.list[rec];
    if (entry->topic_id == OD_TOPIC_FREE) {         // New object
        if (topic_id == OD_TOPIC_FREE) {
            return MQTTSN_RC_REJ_INV_ID;            // Delete of a not existing object
        }
        entry->sidx = sidx;
        if (attach_entry(entry) != MQTTSN_RC_ACCEPTED) {
            return MQTTSN_RC_REJ_INV_ID;            // Resource busy
        }
        entry->topic_id = topic_id;
        od_storage_list_add(&sidx);
        ESP_LOGI(TAG, "%.*s registered, topic %u", (int)len, (const char *)name, topic_id);
    } else if (topic_id == OD_TOPIC_FREE) {         // Delete object
        delete_entry((size_t)rec);
        ESP_LOGI(TAG, "%.*s deleted", (int)len, (const char *)name);
    } else {                                        // New topic id or duplicate
        entry->topic_id = topic_id;
    }
    return MQTTSN_RC_ACCEPTED;
}

void od_on_regack(uint16_t topic_id)
{
    if (s.reg_rec == NO_RECORD) {
        return;
    }

    size_t rec = (size_t)s.reg_rec;
    s.reg_rec = NO_RECORD;

    if (topic_id != MQTTSN_TOPIC_ID_INVALID) {
        s.list[rec].topic_id = topic_id;
    } else {
        ESP_LOGW(TAG, "registration rejected, object deleted");
        delete_entry(rec);
    }

    if (rec == s.idx) {
        s.idx++;                        // PRE_CONNECT, next object
    }
}

uint8_t od_on_publish(uint16_t topic_id, uint8_t flags, const uint8_t *data, size_t len)
{
    const od_entry_t *entry = find_entry(topic_id, flags);
    if (entry == NULL) {
        return MQTTSN_RC_REJ_INV_ID;
    }
    if (entry->write == NULL) {
        return MQTTSN_RC_REJ_NOT_SUPP;
    }

    uint8_t buf[MQTTSN_MSG_SIZE];
    if (len > sizeof(buf)) {
        return MQTTSN_RC_REJ_NOT_SUPP;
    }
    memcpy(buf, data, len);

    uint8_t n = od_unpack(od_pack_width(&entry->sidx), buf, (uint8_t)len);
    return entry->write(&entry->sidx, n, buf);
}

// Node name or "<declarer>_<address hex>" (X13: mqttsn_build_node_name)
size_t od_get_client_id(uint8_t *buf)
{
    size_t len = od_storage_name(buf, MQTTSN_CLIENT_ID_MAX);
    while ((len > 0) && (buf[len - 1] == '\0')) {
        len--;
    }
    if (len > 0) {
        return len;
    }

    // "<declarer>_<suffix>": the node address in hex, or the suffix of the link (WiFi: from the MAC)
    static const char hex[] = "0123456789ABCDEF";
    char addr_hex[3];
    const char *suffix = s.name_suffix;
    if (suffix == NULL) {
        uint8_t addr = od_storage_node_id();
        addr_hex[0] = hex[addr >> 4];
        addr_hex[1] = hex[addr & 0x0F];
        addr_hex[2] = '\0';
        suffix = addr_hex;
    }
    size_t n = strlen(suffix);
    if (n > MQTTSN_CLIENT_ID_MAX - 2) {
        n = MQTTSN_CLIENT_ID_MAX - 2;
    }

    const char *dt = s.device_type;
    while ((dt[len] != '\0') && (dt[len] != '.') && (len < MQTTSN_CLIENT_ID_MAX - 1 - n)) {
        buf[len] = (uint8_t)dt[len];
        len++;
    }
    buf[len++] = '_';
    memcpy(&buf[len], suffix, n);
    return len + n;
}

void od_load_addrs(mqttsn_addr_t *node, mqttsn_addr_t *gateway)
{
    node->len = 1;
    node->b[0] = od_storage_node_id();
    gateway->len = 1;
    gateway->b[0] = od_storage_gate_id();
}

void od_store_node_addr(const mqttsn_addr_t *node)
{
    od_storage_set_node_id(node->b[0]);
}
