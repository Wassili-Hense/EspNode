/*
 * X13 PLC: control object, merkers, PLC cycle.
 *
 * Based on X13.Home extplc, Copyright (c) 2011-2017 <comparator@gmx.de>,
 * BSD 3-Clause License. See LICENSE.X13 for details.
 */
#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "mqttsn_defs.h"
#include "plc.h"
#include "plc_api.h"
#include "plc_io.h"
#include "plc_store.h"
#include "plc_vm.h"

static const char *TAG = "plc";

#define PRG_SIZE            CONFIG_PLC_PROGRAM_SIZE
#define RAM_WORDS           CONFIG_PLC_RAM_WORDS
#define RW_WORDS            CONFIG_PLC_RW_WORDS     // Merker area, start of RAM

_Static_assert(PRG_SIZE <= 0x10000, "Program addresses are 16 bit");
_Static_assert((PRG_SIZE % 4096) == 0, "Program memory is stored in 4K flash sectors");
_Static_assert(RW_WORDS <= RAM_WORDS, "Merker area exceeds RAM");

// Control protocol, object "pa1" (X13: "pa0", the same protocol).
// Request: [cmd] [args], response: [cmd + 1] [status] [data]
enum {
    CMD_IDLE                = 0x00,
    CMD_START_REQ           = 0x01,
    CMD_START_RESP          = 0x02,
    CMD_STOP_REQ            = 0x03,
    CMD_STOP_RESP           = 0x04,     // Also sent when the program stops on an error
    CMD_CRC_REQ             = 0x05,     // addrL, addrH, lenL, lenH
    CMD_CRC_RESP            = 0x06,     // status, crcL, crcH
    CMD_WRITE_REQ           = 0x07,     // addrL, addrH, data[], crcL, crcH
    CMD_WRITE_RESP          = 0x08,
    CMD_SET_STACK_REQ       = 0x0B,     // stack bottom in words, 32 bit LE
    CMD_SET_STACK_RESP      = 0x0C,
};

static struct {
    bool            run;
    bool            busy;               // Exchange buffer holds a request or a response
    bool            answer;             // Response ready to publish
    bool            dirty;              // Program changed since saved
    bool            io_sweep;           // Release the pins of the previous run after the init cycle
    plc_status_t    error;              // Stop error to report
    plc_vm_regs_t   regs;               // VM registers at the error
    uint8_t         xlen;
    uint8_t         xbuf[MQTTSN_MSG_SIZE];
    uint8_t         prg[PRG_SIZE];
    uint32_t        ram[RAM_WORDS];
    uint32_t        old[RW_WORDS];      // Merker values published last
} s;

static uint16_t crc16(uint16_t crc, const uint8_t *buf, size_t len)
{
    while (len--) {
        uint8_t data = *buf++;
        for (int i = 0; i < 8; i++) {
            if (((crc & 0x8000) >> 8) ^ (data & 0x80)) {
                crc = (uint16_t)((crc << 1) ^ 0x8005);
            } else {
                crc = (uint16_t)(crc << 1);
            }
            data <<= 1;
        }
    }
    return crc;
}

static void put_u32(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)val;
    buf[1] = (uint8_t)(val >> 8);
    buf[2] = (uint8_t)(val >> 16);
    buf[3] = (uint8_t)(val >> 24);
}

static uint32_t get_u32(const uint8_t *buf)
{
    return buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

////////////////////////////////////////////////////////////////////////
// Control

static void reply(uint8_t cmd, uint8_t status)
{
    s.xbuf[0] = cmd;
    s.xbuf[1] = status;
    s.xlen = 2;
}

static uint8_t start(void)
{
    if (s.run) {
        return PLC_ERR_WRS;
    }

    uint8_t status = PLC_OK;
    if (s.dirty) {
        if (plc_store_save_program(s.prg, PRG_SIZE) == ESP_OK) {
            s.dirty = false;
        } else {
            status = PLC_ERR_STORAGE;   // Runs from RAM anyway
        }
    }

    // Pins set up again the same way keep running, outputs don't glitch
    plc_io_restart_begin();
    s.io_sweep = true;

    plc_vm_restart();
    s.run = true;
    ESP_LOGI(TAG, "started");
    return status;
}

static uint8_t write_block(const uint8_t *req, uint8_t len)
{
    if (s.run) {
        return PLC_ERR_WRS;
    }
    if (len < 6) {
        return PLC_ERR_FMT;
    }

    uint32_t addr = req[1] | ((uint32_t)req[2] << 8);
    uint32_t size = len - 5u;
    if (addr + size > PRG_SIZE) {
        return PLC_ERR_OFR;
    }

    uint16_t crc = (uint16_t)(req[len - 2] | (req[len - 1] << 8));
    if (crc16(0xFFFF, &req[3], size) != crc) {
        return PLC_ERR_CRC;
    }

    memcpy(&s.prg[addr], &req[3], size);
    s.dirty = true;
    return PLC_OK;
}

static uint8_t set_stack_bottom(const uint8_t *req, uint8_t len)
{
    if (len != 5) {
        return PLC_ERR_FMT;
    }
    if (s.run) {
        return PLC_ERR_WRS;
    }

    uint32_t val = get_u32(&req[1]);
    if (val >= RAM_WORDS - 2) {
        return PLC_ERR_OFR_SP;
    }
    if (val != plc_vm_stack_bottom()) {
        plc_vm_set_stack_bottom(val);
        if (plc_store_save_stack_bottom(val) != ESP_OK) {
            return PLC_ERR_STORAGE;
        }
    }
    return PLC_OK;
}

// Process the request in the exchange buffer, leave the response there
static void control(void)
{
    uint8_t *buf = s.xbuf;
    uint8_t len = s.xlen;

    switch ((len > 0) ? buf[0] : CMD_IDLE) {
    case CMD_START_REQ:
        reply(CMD_START_RESP, start());
        break;

    case CMD_STOP_REQ:
        if (s.run) {
            ESP_LOGI(TAG, "stopped");
        }
        s.run = false;
        reply(CMD_STOP_RESP, PLC_OK);
        break;

    case CMD_CRC_REQ: {
        if (len != 5) {
            reply(CMD_CRC_RESP, PLC_ERR_FMT);
            break;
        }
        uint32_t addr = buf[1] | ((uint32_t)buf[2] << 8);
        uint32_t size = buf[3] | ((uint32_t)buf[4] << 8);
        if (addr + size > PRG_SIZE) {
            reply(CMD_CRC_RESP, PLC_ERR_OFR);
            break;
        }
        uint16_t crc = crc16(0xFFFF, &s.prg[addr], size);
        reply(CMD_CRC_RESP, PLC_OK);
        buf[2] = (uint8_t)crc;
        buf[3] = (uint8_t)(crc >> 8);
        s.xlen = 4;
        break;
    }

    case CMD_WRITE_REQ:
        reply(CMD_WRITE_RESP, write_block(buf, len));
        break;

    case CMD_SET_STACK_REQ:
        reply(CMD_SET_STACK_RESP, set_stack_bottom(buf, len));
        break;

    default:                            // Echo the command with an error
        buf[1] = PLC_ERR_CMD;
        s.xlen = 2;
        break;
    }
}

// Control object "pa1": the gateway writes a request, the response is published
static uint8_t ctrl_read(const od_subidx_t *sidx, uint8_t *len, uint8_t *buf)
{
    (void)sidx;

    uint8_t n = s.answer ? s.xlen : 0;
    if (n > *len) {
        n = *len;
    }
    memcpy(buf, s.xbuf, n);
    *len = n;

    s.busy = false;
    s.answer = false;
    return MQTTSN_RC_ACCEPTED;
}

static uint8_t ctrl_write(const od_subidx_t *sidx, uint8_t len, const uint8_t *buf)
{
    (void)sidx;

    if (s.busy) {
        return MQTTSN_RC_REJ_CONG;
    }
    if (len > sizeof(s.xbuf)) {
        return MQTTSN_RC_REJ_NOT_SUPP;
    }

    memcpy(s.xbuf, buf, len);
    s.xlen = len;
    s.busy = true;
    s.answer = false;
    return MQTTSN_RC_ACCEPTED;
}

static bool ctrl_poll(const od_subidx_t *sidx)
{
    (void)sidx;

    if (s.answer) {
        return true;
    }
    if (s.busy) {
        control();
        s.answer = true;
        return true;
    }
    return false;
}

////////////////////////////////////////////////////////////////////////
// Merkers: "Mz<n>" bool, "Mb/MB<n>" 8 bit, "Mw/MW<n>" 16 bit, "Md<n>" 32 bit, "Mf<n>" float;
// n is the index in units of the type from the start of the PLC RAM

typedef struct {
    uint32_t word;
    uint32_t shift;
    uint32_t mask;          // Value mask before the shift
    uint8_t  size;          // Bytes in the payload
} merker_t;

static bool merker_locate(const od_subidx_t *sidx, merker_t *m)
{
    uint32_t base = sidx->base;

    switch (sidx->type) {
    case OD_TYPE_BOOL:
        *m = (merker_t){ base >> 5, base & 0x1F, 0x1, 1 };
        break;
    case OD_TYPE_INT8:
    case OD_TYPE_UINT8:
        *m = (merker_t){ base >> 2, (base & 0x03) * 8, 0xFF, 1 };
        break;
    case OD_TYPE_INT16:
    case OD_TYPE_UINT16:
        *m = (merker_t){ base >> 1, (base & 0x01) * 16, 0xFFFF, 2 };
        break;
    case OD_TYPE_INT32:
    case OD_TYPE_FLOAT:                 // IEEE-754 bits, 4 bytes LE
        *m = (merker_t){ base, 0, 0xFFFFFFFF, 4 };
        break;
    default:
        return false;
    }
    return m->word < RW_WORDS;
}

static void merker_store(uint32_t *mem, const merker_t *m, uint32_t val)
{
    mem[m->word] = (mem[m->word] & ~(m->mask << m->shift)) | ((val & m->mask) << m->shift);
}

static uint8_t merker_read(const od_subidx_t *sidx, uint8_t *len, uint8_t *buf)
{
    merker_t m;
    if (!merker_locate(sidx, &m) || (*len < m.size)) {
        return MQTTSN_RC_REJ_INV_ID;
    }

    uint32_t val = (s.ram[m.word] >> m.shift) & m.mask;
    merker_store(s.old, &m, val);

    for (uint8_t i = 0; i < m.size; i++) {
        buf[i] = (uint8_t)(val >> (8 * i));
    }
    *len = m.size;
    return MQTTSN_RC_ACCEPTED;
}

static uint8_t merker_write(const od_subidx_t *sidx, uint8_t len, const uint8_t *buf)
{
    merker_t m;
    if (!merker_locate(sidx, &m) || (len == 0)) {
        return MQTTSN_RC_REJ_NOT_SUPP;
    }

    uint32_t val = 0;
    if (sidx->type == OD_TYPE_BOOL) {
        val = (buf[0] != 0);
    } else {
        for (uint8_t i = 0; (i < m.size) && (i < len); i++) {
            val |= (uint32_t)buf[i] << (8 * i);
        }
    }

    merker_store(s.ram, &m, val);
    merker_store(s.old, &m, val);
    return MQTTSN_RC_ACCEPTED;
}

static bool merker_poll(const od_subidx_t *sidx)
{
    merker_t m;
    if (!merker_locate(sidx, &m)) {
        return false;
    }
    return ((s.ram[m.word] ^ s.old[m.word]) & (m.mask << m.shift)) != 0;
}

static bool merker_check(const od_subidx_t *sidx)
{
    merker_t m;
    return merker_locate(sidx, &m);
}

static uint8_t merker_attach(od_entry_t *entry)
{
    entry->read = merker_read;
    entry->write = merker_write;
    entry->poll = merker_poll;
    return MQTTSN_RC_ACCEPTED;
}

////////////////////////////////////////////////////////////////////////
// Provider

static void provider_init(void)
{
    // "pa1", not "pa0" as X13: the gateway tells EspNode programs (IO, TWI, float) from X13 ones
    const od_subidx_t ctrl = { .place = OD_PLACE_PLC_CTRL, .type = OD_TYPE_ARRAY, .base = 1 };

    od_entry_t *entry = od_add_object(&ctrl);
    if (entry == NULL) {
        ESP_LOGE(TAG, "no room for the control object");
        return;
    }
    entry->read = ctrl_read;
    entry->write = ctrl_write;
    entry->poll = ctrl_poll;
}

// PLC cycle, every od_poll()
static void provider_proc(void)
{
    if (s.run) {
        plc_status_t stat = plc_vm_cycle();
        if ((stat == PLC_OK) && s.io_sweep) {
            plc_io_restart_end();
            s.io_sweep = false;
        }
        if (stat != PLC_OK) {
            s.run = false;
            s.error = stat;
            plc_vm_regs(&s.regs);
            ESP_LOGW(TAG, "stopped, error 0x%02X: SP=%" PRIu32 " *SP=0x%08" PRIX32 " SFP=%" PRIu32
                     " PC=0x%04" PRIX32, (unsigned)stat, s.regs.sp, s.regs.top, s.regs.sfp,
                     s.regs.pc);
        }
    }

    // Report the error as soon as the exchange buffer is free
    if ((s.error != PLC_OK) && !s.busy) {
        uint8_t *buf = s.xbuf;
        buf[0] = CMD_STOP_RESP;
        buf[1] = s.error;
        put_u32(&buf[2], s.regs.sp);
        put_u32(&buf[6], s.regs.top);
        put_u32(&buf[10], s.regs.sfp);
        put_u32(&buf[14], s.regs.pc);
        s.xlen = 18;
        s.busy = true;
        s.answer = true;
        s.error = PLC_OK;
    }
}

// Session start: publish the PLC state (X13: plcStatPrep)
static void provider_session(void)
{
    if (!s.busy) {
        reply(s.run ? CMD_IDLE : CMD_STOP_RESP, s.error);
        s.busy = true;
        s.answer = true;
    }
}

// Device info for the compiler of Enviriot: what the program may use on this node (docs/plc-api.md).
// GPIO0..39: bit n of byte n / 8 - GPIOn.
#define FREE_PINS_LEN       5

static uint8_t read_free_pins(const od_subidx_t *sidx, uint8_t *len, uint8_t *buf)
{
    (void)sidx;
    if (*len < FREE_PINS_LEN) {
        return MQTTSN_RC_REJ_NOT_SUPP;
    }
    uint64_t pins = plc_io_free_pins();
    for (size_t i = 0; i < FREE_PINS_LEN; i++) {
        buf[i] = (uint8_t)(pins >> (8 * i));
    }
    *len = FREE_PINS_LEN;
    return MQTTSN_RC_ACCEPTED;
}

static uint8_t read_free_uarts(const od_subidx_t *sidx, uint8_t *len, uint8_t *buf)
{
    (void)sidx;
    if (*len < 1) {
        return MQTTSN_RC_REJ_NOT_SUPP;
    }
    buf[0] = plc_io_free_uarts();
    *len = 1;
    return MQTTSN_RC_ACCEPTED;
}

static const od_entry_t s_info[] = {
    { { OD_PLACE_PROGMEM, OD_TYPE_ARRAY, 0 }, OD_ID_FREE_PINS,  read_free_pins,  NULL, NULL },
    { { OD_PLACE_PROGMEM, OD_TYPE_UINT8, 0 }, OD_ID_FREE_UARTS, read_free_uarts, NULL, NULL },
};

static const od_provider_t s_provider = {
    .place      = OD_PLACE_MERKER,
    .check      = merker_check,
    .attach     = merker_attach,
    .detach     = NULL,
    .init       = provider_init,
    .proc       = provider_proc,
    .on_session = provider_session,
    .info       = s_info,
    .n_info     = sizeof(s_info) / sizeof(s_info[0]),
};

esp_err_t plc_init(const plc_config_t *cfg)
{
    memset(&s, 0, sizeof(s));

    const plc_io_config_t io_cfg = {
        .reserved_pins  = cfg->reserved_pins,
        .reserved_uarts = cfg->reserved_uarts,
        .adc2           = cfg->adc2,
    };
    plc_io_init(&io_cfg);

    uint32_t stack_bottom = 0;
    if (plc_store_load(s.prg, PRG_SIZE, &stack_bottom) != ESP_OK) {
        ESP_LOGE(TAG, "program storage not available, programs are lost on reset");
    }
    if (stack_bottom >= RAM_WORDS - 2) {
        stack_bottom = 0;
    }

    const plc_vm_config_t vm_cfg = {
        .prg        = s.prg,
        .prg_size   = PRG_SIZE,
        .ram        = s.ram,
        .ram_words  = RAM_WORDS,
        .api        = plc_api_call,
        .io_in      = plc_api_in,
        .io_out     = plc_api_out,
    };
    plc_vm_init(&vm_cfg);
    plc_vm_set_stack_bottom(stack_bottom);

    s.run = true;
    ESP_LOGI(TAG, "program %u bytes, RAM %u words, merkers %u words, stack bottom %" PRIu32,
             (unsigned)PRG_SIZE, (unsigned)RAM_WORDS, (unsigned)RW_WORDS, stack_bottom);
    return ESP_OK;
}

const od_provider_t *plc_provider(void)
{
    return &s_provider;
}
