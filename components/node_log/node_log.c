/*
 * Log of the node for the gateway: the queue.
 */
#include <stdio.h>
#include <string.h>

#include "node_log.h"

// MQTT-SN and the links: when the link fails their lines only fill the queue, and sending them could recurse
static const char *const s_quiet_tags[] = { "mqttsn", "mqttsn_gw", "serial_phy", "wifi_phy" };

static struct {
    int             min_level;
    uint8_t         head;               // Oldest line
    uint8_t         count;
    uint32_t        dropped;            // Lost since the last report
    node_log_line_t q[NODE_LOG_DEPTH];
} s;

static int level_of(char c)
{
    switch (c) {
    case 'E': return NODE_LOG_ERROR;
    case 'W': return NODE_LOG_WARN;
    case 'I': return NODE_LOG_INFO;
    case 'D': return NODE_LOG_DEBUG;
    default:  return -1;                // Verbose and anything else
    }
}

static bool quiet(const char *tag, size_t len)
{
    for (size_t i = 0; i < sizeof(s_quiet_tags) / sizeof(s_quiet_tags[0]); i++) {
        if ((strlen(s_quiet_tags[i]) == len) && (memcmp(s_quiet_tags[i], tag, len) == 0)) {
            return true;
        }
    }
    return false;
}

static void push(int level, const char *text, size_t len)
{
    node_log_line_t *line = &s.q[(s.head + s.count) % NODE_LOG_DEPTH];
    if (len > NODE_LOG_TEXT_MAX) {
        len = NODE_LOG_TEXT_MAX;
    }
    line->level = (uint8_t)level;
    line->len = (uint8_t)len;
    memcpy(line->text, text, len);
    s.count++;
}

// How many lines were lost, before the next one
static void push_dropped(void)
{
    char text[NODE_LOG_TEXT_MAX + 1];
    int n = snprintf(text, sizeof(text), "node_log: %u lines dropped", (unsigned)s.dropped);
    push(NODE_LOG_WARN, text, (size_t)n);
    s.dropped = 0;
}

static bool queue(int level, const char *text, size_t len)
{
    if ((s.dropped > 0) && (s.count < NODE_LOG_DEPTH)) {
        push_dropped();
    }
    if (s.count >= NODE_LOG_DEPTH) {
        s.dropped++;                    // The first lines are kept
        return false;
    }
    push(level, text, len);
    return true;
}

void node_log_reset(int min_level)
{
    memset(&s, 0, sizeof(s));
    s.min_level = min_level;
}

bool node_log_put(const char *line, size_t len)
{
    // "W (1234) tag: text": level, time, tag
    if ((len < 4) || (line[1] != ' ') || (line[2] != '(')) {
        return false;
    }
    int level = level_of(line[0]);
    if (level < s.min_level) {
        return false;
    }
    const char *end = line + len;
    const char *tag = memchr(line, ')', len);
    if ((tag == NULL) || (end - tag < 2) || (tag[1] != ' ')) {
        return false;
    }
    tag += 2;
    const char *colon = tag;
    while ((colon + 1 < end) && !((colon[0] == ':') && (colon[1] == ' '))) {
        colon++;
    }
    if ((colon + 1 >= end) || quiet(tag, (size_t)(colon - tag))) {
        return false;
    }
    while ((end > tag) && ((end[-1] == '\n') || (end[-1] == '\r'))) {
        end--;
    }
    return queue(level, tag, (size_t)(end - tag));
}

bool node_log_put_text(int level, const char *text, size_t len)
{
    if ((level < NODE_LOG_DEBUG) || (level > NODE_LOG_ERROR) || (len == 0)) {
        return false;
    }
    return queue(level, text, len);
}

bool node_log_get(node_log_line_t *line)
{
    if ((s.count == 0) && (s.dropped > 0)) {
        push_dropped();
    }
    if (s.count == 0) {
        return false;
    }
    *line = s.q[s.head];
    s.head = (uint8_t)((s.head + 1) % NODE_LOG_DEPTH);
    s.count--;
    return true;
}
