/*
 * Log of the node for the gateway: lines of the ESP-IDF log go to Enviriot as PUBLISH QoS0 to the predefined
 * objects 0xFFE0..0xFFE3 (X13 objLogD..objLogE), Enviriot writes them to its log with that level. The
 * console is off, this is how warnings and errors of the firmware reach anyone. The PLC program writes its own
 * lines there too (log streams of plc_stream.h).
 *
 * The hook only queues the line ("tag: text"), the node task publishes it when the objects have nothing to
 * send: publishing from the hook could recurse through MQTT-SN and the link, which log themselves. Their
 * lines are not sent at all, when the link fails they would only fill the queue. On overflow the first lines
 * are kept (the first error is usually the cause), then a line tells how many were lost.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mqttsn_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Levels, also the offset of the object from 0xFFE0
#define NODE_LOG_DEBUG      0
#define NODE_LOG_INFO       1
#define NODE_LOG_WARN       2
#define NODE_LOG_ERROR      3
#define NODE_LOG_NONE       4           // Minimum level: nothing is sent

#define NODE_LOG_TEXT_MAX   (MQTTSN_MAX_MSG_LEN - MQTTSN_SIZEOF_PUBLISH)    // Longer lines are cut
#define NODE_LOG_DEPTH      16          // Lines waiting for the session

typedef struct {
    uint8_t level;
    uint8_t len;
    char    text[NODE_LOG_TEXT_MAX];    // "tag: text", not terminated
} node_log_line_t;

// Hook into the ESP-IDF log, lines from min_level up are queued; call first in app_main()
void node_log_init(int min_level);

// Next line for the gateway; thread-safe, called from the node task
bool node_log_take(node_log_line_t *line);

// A line of the PLC program (its log streams): queued as it is, whatever the minimum level, and printed to the
// console of a debug build; thread-safe
void node_log_add(int level, const char *text, size_t len);

// The queue itself, not thread-safe (host tests):
void node_log_reset(int min_level);
// A line as the ESP-IDF log prints it, "W (1234) tag: text\n"; returns true if queued
bool node_log_put(const char *line, size_t len);
// A line without a tag, any level; returns true if queued
bool node_log_put_text(int level, const char *text, size_t len);
bool node_log_get(node_log_line_t *line);

#ifdef __cplusplus
}
#endif
