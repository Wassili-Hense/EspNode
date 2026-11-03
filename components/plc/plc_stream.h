/*
 * PLC output streams: SendByte (= UartPutByte), PrintF. The functions append to the buffer of the stream, what
 * the cycle wrote goes out after it, as X13 sent the UART packet of the program (extser.c). Streams: UART ports
 * (the port number) and the node log (0xFFE0..0xFFE3, the objects the lines go to). See docs/plc-api.md.
 *
 * Not thread-safe: runs in the PLC task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "plc_vm.h"

#define PLC_STREAM_LOG          0xFFE0  // .. 0xFFE3: node log, Debug .. Error, the level is stream - 0xFFE0
#define PLC_STREAM_LOG_LEVELS   4
#define PLC_STREAM_UART_SIZE    256     // Bytes of a port waiting for its FIFO

// A line of the program for the node log (Debug 0 .. Error 3), not terminated
typedef void (*plc_stream_log_t)(int level, const char *text, size_t len);

// The next value of PrintF, in the order of the format; false - none (stack underflow)
typedef bool (*plc_stream_arg_t)(uint32_t *value);

void         plc_stream_init(plc_stream_log_t log);

// Drop what was not sent yet: program start and stop
void         plc_stream_reset(void);

// *ok: 1 - written, 0 - no room, nothing written: the output goes whole or not at all. A 0 byte to the log ends
// the line, to a UART it is data.
plc_status_t plc_stream_byte(uint32_t stream, uint32_t value, uint32_t *ok);
// fmt - len bytes, %c %d %x %X %f %% with width, 0/- and the precision of %f; n values, taken by arg() as the
// format goes: all of them, whatever *ok. A format the compiler would refuse, or not n conversions - PLC_ERR_PARM.
plc_status_t plc_stream_printf(uint32_t stream, const uint8_t *fmt, size_t len, uint32_t n, plc_stream_arg_t arg,
                               uint32_t *ok);

// After the cycle: log lines out, UART bytes into the FIFO as far as they fit, the rest stays for the next one
void         plc_stream_flush(void);
