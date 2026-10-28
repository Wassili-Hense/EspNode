// Host tests of the node log queue (components/node_log/node_log.c)
#include <stdio.h>
#include <string.h>

#include "node_log.h"

static int failed, passed;

#define CHECK(cond) do { \
    if (cond) { passed++; } else { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static node_log_line_t got;

static int put(const char *line)
{
    return node_log_put(line, strlen(line));
}

// Next line is this one
static int next(int level, const char *text)
{
    return node_log_get(&got) && (got.level == level) && (got.len == strlen(text)) &&
           (memcmp(got.text, text, got.len) == 0);
}

int main(void)
{
    char line[80];

    // Level, time and line end stripped; "tag: text" stays
    node_log_reset(NODE_LOG_WARN);
    CHECK(put("W (1234) cfg: not written, using defaults\n"));
    CHECK(put("E (5) plc: program storage not available\r\n"));
    CHECK(next(NODE_LOG_WARN, "cfg: not written, using defaults"));
    CHECK(next(NODE_LOG_ERROR, "plc: program storage not available"));
    CHECK(!node_log_get(&got));

    // Below the level, verbose, tags of MQTT-SN and the links, not a log line
    CHECK(!put("I (10) espnode: cfg: generation 1\n"));
    CHECK(!put("D (10) plc: x\n"));
    CHECK(!put("V (10) plc: x\n"));
    CHECK(!put("W (10) mqttsn: tx PUBLISH failed\n"));
    CHECK(!put("E (10) serial_phy: x\n"));
    CHECK(!put("W (10) wifi_phy: x\n"));
    CHECK(!put("W (10) mqttsn_gw: x\n"));
    CHECK(put("W (10) mqttsn_x: other tag\n"));
    CHECK(!put("plain text\n"));
    CHECK(!put("W (10) no tag\n"));
    CHECK(next(NODE_LOG_WARN, "mqttsn_x: other tag"));
    CHECK(!node_log_get(&got));

    // Info by setting; none
    node_log_reset(NODE_LOG_INFO);
    CHECK(put("I (10) espnode: cfg: generation 1\n"));
    CHECK(next(NODE_LOG_INFO, "espnode: cfg: generation 1"));
    node_log_reset(NODE_LOG_NONE);
    CHECK(!put("E (10) plc: x\n"));

    // Cut to what fits into PUBLISH
    node_log_reset(NODE_LOG_WARN);
    CHECK(put("W (1) plc: 0123456789012345678901234567890123456789012345678901234567890\n"));
    CHECK(node_log_get(&got) && (got.len == NODE_LOG_TEXT_MAX) && (NODE_LOG_TEXT_MAX == 43) &&
          (memcmp(got.text, "plc: 0123456789", 15) == 0));

    // Overflow: the first lines are kept, then how many were lost, then what came after
    node_log_reset(NODE_LOG_WARN);
    for (int i = 0; i < NODE_LOG_DEPTH + 5; i++) {
        snprintf(line, sizeof(line), "W (%d) plc: line %d\n", i, i);
        CHECK(put(line) == (i < NODE_LOG_DEPTH));
    }
    for (int i = 0; i < NODE_LOG_DEPTH; i++) {
        snprintf(line, sizeof(line), "plc: line %d", i);
        CHECK(next(NODE_LOG_WARN, line));
    }
    CHECK(next(NODE_LOG_WARN, "node_log: 5 lines dropped"));
    CHECK(!node_log_get(&got));

    // A new line after a loss comes after the report; with one free place the report takes it
    for (int i = 0; i < NODE_LOG_DEPTH + 2; i++) {
        put("W (1) plc: x\n");
    }
    CHECK(node_log_get(&got));
    CHECK(!put("E (2) plc: lost\n"));
    CHECK(node_log_get(&got) && node_log_get(&got));
    CHECK(put("E (3) plc: later\n"));
    for (int i = 3; i < NODE_LOG_DEPTH; i++) {
        CHECK(next(NODE_LOG_WARN, "plc: x"));
    }
    CHECK(next(NODE_LOG_WARN, "node_log: 2 lines dropped"));
    CHECK(next(NODE_LOG_WARN, "node_log: 1 lines dropped"));
    CHECK(next(NODE_LOG_ERROR, "plc: later"));
    CHECK(!node_log_get(&got));

    // Lines of the PLC program: any level whatever the minimum, as they are, in the same queue
    node_log_reset(NODE_LOG_ERROR);
    CHECK(node_log_put_text(NODE_LOG_DEBUG, "T=21.5", 6));
    CHECK(node_log_put_text(NODE_LOG_INFO, "W (1) x: y", 10));
    CHECK(!node_log_put_text(NODE_LOG_ERROR + 1, "x", 1));
    CHECK(!node_log_put_text(-1, "x", 1));
    CHECK(!node_log_put_text(NODE_LOG_INFO, "x", 0));
    memset(line, 'p', sizeof(line));
    CHECK(node_log_put_text(NODE_LOG_WARN, line, NODE_LOG_TEXT_MAX + 5));
    CHECK(next(NODE_LOG_DEBUG, "T=21.5"));
    CHECK(next(NODE_LOG_INFO, "W (1) x: y"));
    CHECK(node_log_get(&got) && (got.level == NODE_LOG_WARN) && (got.len == NODE_LOG_TEXT_MAX));
    for (int i = 0; i < NODE_LOG_DEPTH + 1; i++) {
        node_log_put_text(NODE_LOG_INFO, "x", 1);
    }
    CHECK(!node_log_put_text(NODE_LOG_INFO, "lost", 4));
    for (int i = 0; i < NODE_LOG_DEPTH; i++) {
        CHECK(next(NODE_LOG_INFO, "x"));
    }
    CHECK(next(NODE_LOG_WARN, "node_log: 2 lines dropped"));
    CHECK(!node_log_get(&got));

    printf("%d passed, %d failed\n", passed, failed);
    printf(failed ? "FAILED\n" : "ALL PASSED\n");
    return failed ? 1 : 0;
}
