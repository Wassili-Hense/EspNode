/*
 * PLC output streams: SendByte (= UartPutByte), PrintInt32, PrintFloat. The functions append to
 * the buffer of the stream, what the cycle wrote goes out after it, as X13 sent the UART packet of the
 * program (extser.c). Streams: UART ports (the port number) and the node log (0xFFE0..0xFFE3, the objects
 * the lines go to). See docs/plc-api.md.
 *
 * Not thread-safe: runs in the PLC task.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "plc_vm.h"

#define PLC_STREAM_LOG          0xFFE0  // .. 0xFFE3: node log, Debug .. Error, the level is stream - 0xFFE0
#define PLC_STREAM_LOG_LEVELS   4
#define PLC_STREAM_UART_SIZE    256     // Bytes of a port waiting for its FIFO

// A line of the program for the node log (Debug 0 .. Error 3), not terminated
typedef void (*plc_stream_log_t)(int level, const char *text, size_t len);

void         plc_stream_init(plc_stream_log_t log);

// Drop what was not sent yet: program start and stop
void         plc_stream_reset(void);

// *ok: 1 - written, 0 - no room, nothing written (a number is written whole or not at all).
// A 0 byte to the log ends the line.
plc_status_t plc_stream_byte(uint32_t stream, uint32_t value, uint32_t *ok);
plc_status_t plc_stream_int(uint32_t stream, uint32_t value, uint32_t *ok);
// format: decimal cba - a: 0 precision not set, else a - 1 digits after the point; b: minimum width, 0 - none;
// c: 0 pad with zeros, 1 with spaces on the left, 2 with spaces on the right
plc_status_t plc_stream_float(uint32_t stream, uint32_t format, uint32_t value, uint32_t *ok);

// After the cycle: log lines out, UART bytes into the FIFO as far as they fit, the rest stays for the next one
void         plc_stream_flush(void);
