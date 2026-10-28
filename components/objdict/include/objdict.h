/*
 * X13 Object Dictionary: maps MQTT-SN topics to device objects and drives the session
 * (register objects, publish device info, subscribe, publish changed values).
 *
 * Not thread-safe: all functions must be called from the MQTT-SN client task.
 *
 * Based on X13.Home device firmware, Copyright (c) 2011-2017 <comparator@gmx.de>,
 * BSD 3-Clause License. See LICENSE.X13 for details.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mqttsn_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

// Object place, 1st char of the topic name
#define OD_PLACE_PROGMEM        'F'     // Read-only constant
#define OD_PLACE_EEMEM          'E'     // Persistent setting
#define OD_PLACE_AIN            'A'     // Analog input
#define OD_PLACE_AOUT           'D'     // Analog output
#define OD_PLACE_DIN            'I'     // Digital input
#define OD_PLACE_DOUT           'O'     // Digital output
#define OD_PLACE_PWM            'P'     // PWM output
#define OD_PLACE_CNT            'C'     // Counter input
#define OD_PLACE_SER            'S'     // Serial port
#define OD_PLACE_MERKER         'M'     // PLC variable
#define OD_PLACE_PLC_CTRL       'p'     // PLC control
#define OD_PLACE_TWI            'T'     // I2C device
#define OD_PLACE_USR_EXT        'X'     // User extension

// Object type, 2nd char of the topic name
#define OD_TYPE_BOOL            'z'
#define OD_TYPE_INT8            'b'
#define OD_TYPE_UINT8           'B'
#define OD_TYPE_INT16           'w'
#define OD_TYPE_UINT16          'W'
#define OD_TYPE_INT32           'd'
#define OD_TYPE_UINT32          'D'
#define OD_TYPE_INT64           'q'
#define OD_TYPE_FLOAT           'f'     // IEEE-754, 4 bytes
#define OD_TYPE_STRING          's'     // UTF-8 string
#define OD_TYPE_ARRAY           'a'
#define OD_TYPE_PWM_FREQ        'g'     // PWM frequency

// Predefined objects (MQTTSN_FL_TOPICID_PREDEF)
#define OD_ID_NODE_NAME         0xFF00  // String, ClientId
#define OD_ID_RTC               0xFF07  // Array, local time from the gateway, see od_clock_now()
#define OD_ID_IN_MUTE           0xFF09  // Array, write only, sent by the gateway to PLC nodes
#define OD_ID_NODE_ID           0xFF10  // UInt8, own address, 0xFF - DHCP
#define OD_ID_GATE_ID           0xFF14  // UInt8, gateway address, 0xFF - search
#define OD_ID_DEVICE_TYPE       0xFFC0  // String, read only
#define OD_ID_PHY1_ADDR         0xFFC1  // Array, read only
#define OD_ID_FREE_PINS         0xFFD8  // Array, read only: GPIOs free for the PLC program (plc provider)
#define OD_ID_FREE_UARTS        0xFFD9  // UInt8, read only: UART ports free for the PLC program (plc provider)

// Topic id of a dynamic object
#define OD_TOPIC_NEW            0x0000  // Not registered yet
#define OD_TOPIC_FREE           0xFFFF  // Slot is free
#define OD_TOPIC_PREDEF_MIN     0xFF00  // Ids from here are predefined

typedef struct {
    uint8_t     place;      // OD_PLACE_*
    uint8_t     type;       // OD_TYPE_*
    uint16_t    base;       // Place specific: pin, register, ...
} od_subidx_t;

_Static_assert(sizeof(od_subidx_t) == 4, "od_subidx_t is stored in NVS");

// Read value, *len: buffer size on input, data length on output. Returns MQTTSN_RC_*
typedef uint8_t (*od_read_t)(const od_subidx_t *sidx, uint8_t *len, uint8_t *buf);
// Write value. Returns MQTTSN_RC_*
typedef uint8_t (*od_write_t)(const od_subidx_t *sidx, uint8_t len, const uint8_t *buf);
// Returns true if the value changed and must be published
typedef bool (*od_poll_t)(const od_subidx_t *sidx);

typedef struct {
    od_subidx_t sidx;
    uint16_t    topic_id;
    od_read_t   read;
    od_write_t  write;
    od_poll_t   poll;
} od_entry_t;

// Extension providing the objects of one place (e.g. PLC merkers)
typedef struct {
    uint8_t place;
    bool    (*check)(const od_subidx_t *sidx);  // Valid and not in use
    uint8_t (*attach)(od_entry_t *entry);       // Set callbacks, returns MQTTSN_RC_*
    void    (*detach)(const od_subidx_t *sidx); // Optional
    void    (*init)(void);                      // Optional, called from od_init()
    void    (*proc)(void);                      // Optional, called from od_poll()
    void    (*on_session)(void);                // Optional, device info published (PRE_CONNECT)
    // Optional, read-only predefined objects (ids from OD_ID_DEVICE_TYPE on), published with the device info
    const od_entry_t *info;
    size_t  n_info;
} od_provider_t;

typedef struct {
    const char                 *device_type;    // "<declarer>.<sw version>", e.g. "E1S10.400"
    const od_provider_t *const *providers;
    size_t                      n_providers;
    // Node settings from the installer (cfg partition). The values the gateway writes (name, node and
    // gateway address) override them until a new generation of cfg arrives.
    uint32_t                    cfg_generation; // 0 - no cfg, keep the saved values
    const char                 *node_name;      // NULL or empty - "<declarer>_<name_suffix>"
    uint8_t                     node_id;        // 0xFF - DHCP
    // Links where the node address is not the 1-byte X13 address (WiFi: IPv4), NULL for the X13 ones
    const char                 *name_suffix;    // Of the default name; NULL - the node address in hex
    size_t                    (*phy1_addr)(uint8_t *buf, size_t max);  // Object 0xFFC1; NULL - node address
} od_config_t;

// Wall clock: local time the gateway writes to OD_ID_RTC (Enviriot: at connect and every hour).
// Until then it counts from 1970-01-01 00:00:00 at boot.
typedef struct {
    uint16_t    year;
    uint8_t     month;      // 1..12
    uint8_t     day;        // 1..31
    uint8_t     wday;       // 1 - Monday .. 7 - Sunday
    uint8_t     hour;
    uint8_t     min;
    uint8_t     sec;
} od_clock_t;

void od_clock_now(od_clock_t *now);

// Requires nvs_flash_init()
esp_err_t od_init(const od_config_t *cfg);

// Add an object owned by the node (X13: PnP object), the node registers it with the gateway.
// Not saved, call it from od_provider_t.init(). Returns NULL if the table is full,
// otherwise the entry to set callbacks in.
od_entry_t *od_add_object(const od_subidx_t *sidx);

// Call every 10 ms, before mqttsn_client_tick()
void od_poll(void);

// mqttsn_client callbacks
uint8_t od_on_register(uint16_t topic_id, const uint8_t *name, size_t len);
void    od_on_regack(uint16_t topic_id);
uint8_t od_on_publish(uint16_t topic_id, uint8_t flags, const uint8_t *data, size_t len);
size_t  od_get_client_id(uint8_t *buf);
// X13 nodes use 1-byte addresses
void    od_load_addrs(mqttsn_addr_t *node, mqttsn_addr_t *gateway);
void    od_store_node_addr(const mqttsn_addr_t *node);

#ifdef __cplusplus
}
#endif
