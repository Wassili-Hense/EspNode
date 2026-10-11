/*
 * X13 PLC virtual machine.
 *
 * Bytecode compatible with X13 extplc_vm. Differences, all in cases where the STM32 result was
 * a side effect of the ARM instruction set and would crash or differ on Xtensa:
 *  - '%' by 0 stops with PLC_ERR_DIV0 like '/' (ARM: returned the dividend);
 *  - shifts by 32 and more give 0 (sign for ASR) as on ARM;
 *  - INT32_MIN / -1 gives INT32_MIN as on ARM;
 *  - CALL at SP = 0 stops with PLC_ERR_OFR_SP (X13 wrote outside of RAM);
 *  - IN/OUT opcodes (X13 IO objects) stop with PLC_ERR_UNK_COP.
 *
 * Float operations (Enviriot extension, 0x18-0x1F, 0x28-0x37): 32-bit IEEE-754 in a stack cell,
 * operand order as for integers, results follow JavaScript, the source language:
 *  - FDIV by 0 gives +-Infinity / NaN, no error;
 *  - FMIN/FMAX (Math.min/max): NaN if an operand is NaN, -0 < +0;
 *  - FROUND (Math.round): halves round towards +Infinity;
 *  - FTOS32/FTOU32 (x | 0, x >>> 0): truncate, modulo 2^32, NaN and Infinity give 0.
 *
 * Based on X13.Home extplc_vm, Copyright (c) 2011-2017 <comparator@gmx.de>,
 * BSD 3-Clause License. See LICENSE.X13 for details.
 */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "plc_vm.h"

#define WATCHDOG            0xFFFF      // Max operations per cycle
#define CYCLE_ENTRY         4           // Start address of every cycle but the first

static struct {
    const uint8_t  *prg;
    uint32_t        prg_size;
    uint32_t       *ram;
    uint32_t        ram_words;
    plc_vm_api_t    api;

    uint32_t        stack_bot;          // Lowest stack address, data below
    uint32_t        sp;                 // Stack pointer, grows down
    uint32_t        sfp;                // Stack frame pointer
    uint32_t        pc;
    plc_status_t    stat;
    bool            first_start;
} s;

////////////////////////////////////////////////////////////////////////
// Memory access

// Read n bytes of program memory, little-endian
static uint32_t prg_read(uint32_t addr, uint32_t n)
{
    if ((addr >= s.prg_size) || (n > s.prg_size - addr)) {
        s.stat = PLC_ERR_OFR_PC;
        return 0;
    }

    uint32_t val = 0;
    for (uint32_t i = n; i > 0; i--) {
        val = (val << 8) | s.prg[addr + i - 1];
    }
    return val;
}

// Read an operand at PC
static uint32_t lpm(uint32_t n)
{
    uint32_t val = prg_read(s.pc, n);
    s.pc += n;
    return val;
}

static uint32_t lpm_u8(void)  { return lpm(1); }
static uint32_t lpm_u16(void) { return lpm(2); }
static uint32_t lpm_u32(void) { return lpm(4); }

// RAM is an array of 32-bit words; bool, 8 and 16 bit addresses select parts of a word,
// lowest bits first (little-endian layout as on STM32).
static uint32_t get_bool(uint32_t addr)
{
    if (addr < s.ram_words * 32) {
        return (s.ram[addr >> 5] >> (addr & 0x1F)) & 1;
    }
    s.stat = PLC_ERR_OFR_RAM;
    return 0;
}

static uint32_t get_u8(uint32_t addr)
{
    if (addr < s.ram_words * 4) {
        return (s.ram[addr >> 2] >> ((addr & 0x03) * 8)) & 0xFF;
    }
    s.stat = PLC_ERR_OFR_RAM;
    return 0;
}

static uint32_t get_s8(uint32_t addr)
{
    uint32_t val = get_u8(addr);
    return (val & 0x80) ? (val | 0xFFFFFF00) : val;
}

static uint32_t get_u16(uint32_t addr)
{
    if (addr < s.ram_words * 2) {
        return (s.ram[addr >> 1] >> ((addr & 0x01) * 16)) & 0xFFFF;
    }
    s.stat = PLC_ERR_OFR_RAM;
    return 0;
}

static uint32_t get_s16(uint32_t addr)
{
    uint32_t val = get_u16(addr);
    return (val & 0x8000) ? (val | 0xFFFF0000) : val;
}

static uint32_t get_u32(uint32_t addr)
{
    if (addr < s.ram_words) {
        return s.ram[addr];
    }
    s.stat = PLC_ERR_OFR_RAM;
    return 0;
}

static void set_bool(uint32_t addr, bool val)
{
    if (addr < s.ram_words * 32) {
        uint32_t mask = 1UL << (addr & 0x1F);
        if (val) {
            s.ram[addr >> 5] |= mask;
        } else {
            s.ram[addr >> 5] &= ~mask;
        }
    } else {
        s.stat = PLC_ERR_OFR_RAM;
    }
}

static void set_u8(uint32_t addr, uint32_t val)
{
    if (addr < s.ram_words * 4) {
        uint32_t shift = (addr & 0x03) * 8;
        s.ram[addr >> 2] = (s.ram[addr >> 2] & ~(0xFFUL << shift)) | ((val & 0xFF) << shift);
    } else {
        s.stat = PLC_ERR_OFR_RAM;
    }
}

static void set_u16(uint32_t addr, uint32_t val)
{
    if (addr < s.ram_words * 2) {
        uint32_t shift = (addr & 0x01) * 16;
        s.ram[addr >> 1] = (s.ram[addr >> 1] & ~(0xFFFFUL << shift)) | ((val & 0xFFFF) << shift);
    } else {
        s.stat = PLC_ERR_OFR_RAM;
    }
}

static void set_u32(uint32_t addr, uint32_t val)
{
    if (addr < s.ram_words) {
        s.ram[addr] = val;
    } else {
        s.stat = PLC_ERR_OFR_RAM;
    }
}

uint32_t plc_vm_pop(void)
{
    if (s.sp < s.sfp) {
        return s.ram[s.sp++];
    }
    s.stat = PLC_ERR_OFR_SP;
    return 0;
}

void plc_vm_push(uint32_t val)
{
    if (s.sp > s.stack_bot) {
        s.ram[--s.sp] = val;
    } else {
        s.stat = PLC_ERR_OFR_SP;
    }
}

#define pop     plc_vm_pop
#define push    plc_vm_push

// Stack checks: one operand / two operands / room for a result
#define HAS1    (s.sp < s.sfp)
#define HAS2    ((s.sp + 1) < s.sfp)
#define ROOM    (s.sp > s.stack_bot)

////////////////////////////////////////////////////////////////////////
// Operations

typedef void (*op_t)(void);

// 0x00 - 0x07, stack manipulation
static void op_nop(void) {}
static void op_dup(void)  { if (HAS1 && ROOM) { uint32_t v = s.ram[s.sp]; s.ram[--s.sp] = v; } else { s.stat = PLC_ERR_OFR_SP; } }    // a -- a a
static void op_drop(void) { if (HAS1) { s.sp++; } else { s.stat = PLC_ERR_OFR_SP; } }
static void op_nip(void)  { if (HAS2) { uint32_t v = s.ram[s.sp++]; s.ram[s.sp] = v; } else { s.stat = PLC_ERR_OFR_SP; } }           // a b -- b
static void op_swap(void) { if (HAS2) { uint32_t v = s.ram[s.sp]; s.ram[s.sp] = s.ram[s.sp + 1]; s.ram[s.sp + 1] = v; } else { s.stat = PLC_ERR_OFR_SP; } }
static void op_over(void) { uint32_t c1 = pop(); uint32_t c2 = pop(); push(c2); push(c1); push(c2); }                        // a b -- a b a
static void op_rot(void)  { uint32_t c1 = pop(); uint32_t c2 = pop(); uint32_t c3 = pop(); push(c2); push(c1); push(c3); }  // a b c -- b c a

// Unary operation on the top of the stack
#define UNARY(name, expr) \
    static void name(void) { if (HAS1) { uint32_t v = s.ram[s.sp]; s.ram[s.sp] = (expr); } else { s.stat = PLC_ERR_OFR_SP; } }

// Binary operation, v2 - second, v1 - top; computed on uint32 to wrap around like ARM
#define BINARY(name, expr) \
    static void name(void) { if (HAS2) { uint32_t v1 = s.ram[s.sp++]; uint32_t v2 = s.ram[s.sp]; s.ram[s.sp] = (expr); } else { s.stat = PLC_ERR_OFR_SP; } }

// Shift by an immediate count; ARM semantics for counts >= 32
#define SHIFT(name, expr) \
    static void name(void) { if (HAS1) { uint32_t v = s.ram[s.sp]; uint32_t n = lpm_u8(); s.ram[s.sp] = (expr); } else { s.stat = PLC_ERR_OFR_SP; } }

// 0x08 - 0x0F, bitwise
UNARY(op_not, ~v)
BINARY(op_and, v1 & v2)
BINARY(op_or,  v1 | v2)
BINARY(op_xor, v1 ^ v2)
SHIFT(op_lsl, (n < 32) ? (v << n) : 0)
SHIFT(op_lsr, (n < 32) ? (v >> n) : 0)
SHIFT(op_asr, (n < 32) ? (uint32_t)((int32_t)v >> n) : (((int32_t)v < 0) ? 0xFFFFFFFF : 0))

// 0x10 - 0x17, arithmetic
BINARY(op_add, v2 + v1)
BINARY(op_sub, v2 - v1)
BINARY(op_mul, v2 * v1)

static void op_div(void)
{
    if (!HAS2) {
        s.stat = PLC_ERR_OFR_SP;
        return;
    }
    int32_t v1 = (int32_t)s.ram[s.sp++];
    int32_t v2 = (int32_t)s.ram[s.sp];
    if (v1 == 0) {
        s.stat = PLC_ERR_DIV0;
    } else if (v1 == -1) {
        s.ram[s.sp] = 0U - (uint32_t)v2;        // INT32_MIN / -1 = INT32_MIN
    } else {
        s.ram[s.sp] = (uint32_t)(v2 / v1);
    }
}

static void op_mod(void)
{
    if (!HAS2) {
        s.stat = PLC_ERR_OFR_SP;
        return;
    }
    int32_t v1 = (int32_t)s.ram[s.sp++];
    int32_t v2 = (int32_t)s.ram[s.sp];
    if (v1 == 0) {
        s.stat = PLC_ERR_DIV0;
    } else if (v1 == -1) {
        s.ram[s.sp] = 0;
    } else {
        s.ram[s.sp] = (uint32_t)(v2 % v1);
    }
}

UNARY(op_inc, v + 1)
UNARY(op_dec, v - 1)
UNARY(op_neg, 0U - v)

// 0x20 - 0x27, compare (signed)
BINARY(op_ceq, v2 == v1)
BINARY(op_cne, v2 != v1)
BINARY(op_cgt, (int32_t)v2 >  (int32_t)v1)
BINARY(op_cge, (int32_t)v2 >= (int32_t)v1)
BINARY(op_clt, (int32_t)v2 <  (int32_t)v1)
BINARY(op_cle, (int32_t)v2 <= (int32_t)v1)
UNARY(op_cze, v == 0)

// Float operations, values are IEEE-754 bits in the stack cells
static float to_float(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static uint32_t to_bits(float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

// Math.min / Math.max
static float js_min(float a, float b)
{
    if (isnan(a) || isnan(b)) {
        return NAN;
    }
    if (a == b) {
        return signbit(a) ? a : b;      // -0 < +0
    }
    return (a < b) ? a : b;
}

static float js_max(float a, float b)
{
    if (isnan(a) || isnan(b)) {
        return NAN;
    }
    if (a == b) {
        return signbit(a) ? b : a;
    }
    return (a > b) ? a : b;
}

// Math.round: halves towards +Infinity, the sign of zero is kept
static float js_round(float a)
{
    float r = floorf(a);
    if (a - r >= 0.5f) {                // Exact for |a| < 2^23, larger floats are integers
        r += 1.0f;
    }
    if ((r == 0.0f) && signbit(a)) {
        r = -0.0f;
    }
    return r;
}

// ToInt32 / ToUint32 (x | 0, x >>> 0): truncate, modulo 2^32, NaN and Infinity give 0.
// Both give the same 32 bits.
static uint32_t js_to_int32(float f)
{
    uint32_t bits = to_bits(f);
    int exp = (int)((bits >> 23) & 0xFF) - 127;
    if ((exp < 0) || (exp == 128)) {
        return 0;                       // |f| < 1, NaN, Infinity
    }

    uint32_t mant = (bits & 0x7FFFFF) | 0x800000;
    uint32_t mag;
    if (exp <= 23) {
        mag = mant >> (23 - exp);
    } else if (exp < 23 + 32) {
        mag = mant << (exp - 23);       // Bits above 2^31 drop out: modulo 2^32
    } else {
        mag = 0;
    }
    return (bits & 0x80000000) ? (0U - mag) : mag;
}

// Unary and binary float operations, a - second, b - top of the stack
#define FUNARY(name, expr) \
    static void name(void) { if (HAS1) { float a = to_float(s.ram[s.sp]); s.ram[s.sp] = (expr); } else { s.stat = PLC_ERR_OFR_SP; } }
#define FBINARY(name, expr) \
    static void name(void) { if (HAS2) { float b = to_float(s.ram[s.sp++]); float a = to_float(s.ram[s.sp]); s.ram[s.sp] = (expr); } else { s.stat = PLC_ERR_OFR_SP; } }

// 0x18 - 0x1F, float arithmetic
FBINARY(op_fadd, to_bits(a + b))
FBINARY(op_fsub, to_bits(a - b))
FBINARY(op_fmul, to_bits(a * b))
FBINARY(op_fdiv, to_bits(a / b))
FUNARY(op_fneg, to_bits(-a))
FUNARY(op_fabs, to_bits(fabsf(a)))
FBINARY(op_fmin, to_bits(js_min(a, b)))
FBINARY(op_fmax, to_bits(js_max(a, b)))

// 0x28 - 0x2F, float compare, 0/1; any comparison with NaN is false, but '!='
FBINARY(op_fceq, a == b)
FBINARY(op_fcne, a != b)
FBINARY(op_fcgt, a > b)
FBINARY(op_fcge, a >= b)
FBINARY(op_fclt, a < b)
FBINARY(op_fcle, a <= b)
FUNARY(op_fcze, a == 0.0f)
FUNARY(op_fisfinite, isfinite(a) ? 1 : 0)

// 0x30 - 0x37, conversions
UNARY(op_s32tof, to_bits((float)(int32_t)v))
UNARY(op_u32tof, to_bits((float)v))
UNARY(op_ftos32, js_to_int32(to_float(v)))
UNARY(op_ftou32, js_to_int32(to_float(v)))
FUNARY(op_fround, to_bits(js_round(a)))
FUNARY(op_ftrunc, to_bits(truncf(a)))
FUNARY(op_fisnan, isnan(a) ? 1 : 0)
FUNARY(op_fisinf, isinf(a) ? 1 : 0)

// 0x38 - 0x3F, 0x87, load constant
#define LDI(name, expr) \
    static void name(void) { if (ROOM) { uint32_t v = (expr); s.ram[--s.sp] = v; } else { s.stat = PLC_ERR_OFR_SP; } }

LDI(op_ldi_0,   0)
LDI(op_ldi_s1,  lpm_u8() | 0xFFFFFF00)      // Negative constants only, X13 compiler
LDI(op_ldi_s2,  lpm_u16() | 0xFFFF0000)
LDI(op_ldi_s4,  lpm_u32())
LDI(op_ldi_u1,  lpm_u8())
LDI(op_ldi_u2,  lpm_u16())
LDI(op_ldi_1,   1)
LDI(op_ldi_m1,  0xFFFFFFFF)
LDI(op_ldi_min, 0x80000000)

// 0x40 - 0x7F, load/store relative to SFP: parameters above, locals below
#define LD_SFP(name, expr) \
    static void name(void) { if (ROOM) { uint32_t addr = (expr); if (addr < s.ram_words) { s.ram[--s.sp] = s.ram[addr]; } else { s.stat = PLC_ERR_OFR_RAM; } } else { s.stat = PLC_ERR_OFR_SP; } }
#define ST_SFP(name, expr) \
    static void name(void) { if (HAS1) { uint32_t addr = (expr); if (addr < s.ram_words) { s.ram[addr] = s.ram[s.sp++]; } else { s.stat = PLC_ERR_OFR_RAM; } } else { s.stat = PLC_ERR_OFR_SP; } }

LD_SFP(op_ld_p0, s.sfp + 0x02)  LD_SFP(op_ld_p1, s.sfp + 0x03)  LD_SFP(op_ld_p2, s.sfp + 0x04)  LD_SFP(op_ld_p3, s.sfp + 0x05)
LD_SFP(op_ld_p4, s.sfp + 0x06)  LD_SFP(op_ld_p5, s.sfp + 0x07)  LD_SFP(op_ld_p6, s.sfp + 0x08)  LD_SFP(op_ld_p7, s.sfp + 0x09)
LD_SFP(op_ld_p8, s.sfp + 0x0A)  LD_SFP(op_ld_p9, s.sfp + 0x0B)  LD_SFP(op_ld_pA, s.sfp + 0x0C)  LD_SFP(op_ld_pB, s.sfp + 0x0D)
LD_SFP(op_ld_pC, s.sfp + 0x0E)  LD_SFP(op_ld_pD, s.sfp + 0x0F)  LD_SFP(op_ld_pE, s.sfp + 0x10)  LD_SFP(op_ld_pF, s.sfp + 0x11)
LD_SFP(op_ld_l0, s.sfp - 0x01)  LD_SFP(op_ld_l1, s.sfp - 0x02)  LD_SFP(op_ld_l2, s.sfp - 0x03)  LD_SFP(op_ld_l3, s.sfp - 0x04)
LD_SFP(op_ld_l4, s.sfp - 0x05)  LD_SFP(op_ld_l5, s.sfp - 0x06)  LD_SFP(op_ld_l6, s.sfp - 0x07)  LD_SFP(op_ld_l7, s.sfp - 0x08)
LD_SFP(op_ld_l8, s.sfp - 0x09)  LD_SFP(op_ld_l9, s.sfp - 0x0A)  LD_SFP(op_ld_lA, s.sfp - 0x0B)  LD_SFP(op_ld_lB, s.sfp - 0x0C)
LD_SFP(op_ld_lC, s.sfp - 0x0D)  LD_SFP(op_ld_lD, s.sfp - 0x0E)  LD_SFP(op_ld_lE, s.sfp - 0x0F)  LD_SFP(op_ld_lF, s.sfp - 0x10)

ST_SFP(op_st_p0, s.sfp + 0x02)  ST_SFP(op_st_p1, s.sfp + 0x03)  ST_SFP(op_st_p2, s.sfp + 0x04)  ST_SFP(op_st_p3, s.sfp + 0x05)
ST_SFP(op_st_p4, s.sfp + 0x06)  ST_SFP(op_st_p5, s.sfp + 0x07)  ST_SFP(op_st_p6, s.sfp + 0x08)  ST_SFP(op_st_p7, s.sfp + 0x09)
ST_SFP(op_st_p8, s.sfp + 0x0A)  ST_SFP(op_st_p9, s.sfp + 0x0B)  ST_SFP(op_st_pA, s.sfp + 0x0C)  ST_SFP(op_st_pB, s.sfp + 0x0D)
ST_SFP(op_st_pC, s.sfp + 0x0E)  ST_SFP(op_st_pD, s.sfp + 0x0F)  ST_SFP(op_st_pE, s.sfp + 0x10)  ST_SFP(op_st_pF, s.sfp + 0x11)
ST_SFP(op_st_l0, s.sfp - 0x01)  ST_SFP(op_st_l1, s.sfp - 0x02)  ST_SFP(op_st_l2, s.sfp - 0x03)  ST_SFP(op_st_l3, s.sfp - 0x04)
ST_SFP(op_st_l4, s.sfp - 0x05)  ST_SFP(op_st_l5, s.sfp - 0x06)  ST_SFP(op_st_l6, s.sfp - 0x07)  ST_SFP(op_st_l7, s.sfp - 0x08)
ST_SFP(op_st_l8, s.sfp - 0x09)  ST_SFP(op_st_l9, s.sfp - 0x0A)  ST_SFP(op_st_lA, s.sfp - 0x0B)  ST_SFP(op_st_lB, s.sfp - 0x0C)
ST_SFP(op_st_lC, s.sfp - 0x0D)  ST_SFP(op_st_lD, s.sfp - 0x0E)  ST_SFP(op_st_lE, s.sfp - 0x0F)  ST_SFP(op_st_lF, s.sfp - 0x10)

// 0x80 - 0x9F, load from memory. Address: stack (_s), stack * size + 8/16-bit offset (_cs8,
// _cs16), 16-bit constant (_c16). The size multiplier converts a 32-bit word index.
#define LDM(name, get, addr) \
    static void name(void) { uint32_t a = (addr); push(get(a)); }

LDM(op_ldm_b1_s, get_bool, pop())
LDM(op_ldm_s1_s, get_s8,   pop())
LDM(op_ldm_s2_s, get_s16,  pop())
LDM(op_ldm_s4_s, get_u32,  pop())
LDM(op_ldm_u1_s, get_u8,   pop())
LDM(op_ldm_u2_s, get_u16,  pop())
LDM(op_ldm_b1_cs8, get_bool, pop() * 32 + lpm_u8())
LDM(op_ldm_s1_cs8, get_s8,   pop() * 4  + lpm_u8())
LDM(op_ldm_s2_cs8, get_s16,  pop() * 2  + lpm_u8())
LDM(op_ldm_s4_cs8, get_u32,  pop()      + lpm_u8())
LDM(op_ldm_u1_cs8, get_u8,   pop() * 4  + lpm_u8())
LDM(op_ldm_u2_cs8, get_u16,  pop() * 2  + lpm_u8())
LDM(op_ldm_b1_cs16, get_bool, pop() * 32 + lpm_u16())
LDM(op_ldm_s1_cs16, get_s8,   pop() * 4  + lpm_u16())
LDM(op_ldm_s2_cs16, get_s16,  pop() * 2  + lpm_u16())
LDM(op_ldm_s4_cs16, get_u32,  pop()      + lpm_u16())
LDM(op_ldm_u1_cs16, get_u8,   pop() * 4  + lpm_u16())
LDM(op_ldm_u2_cs16, get_u16,  pop() * 2  + lpm_u16())
LDM(op_ldm_b1_c16, get_bool, lpm_u16())
LDM(op_ldm_s1_c16, get_s8,   lpm_u16())
LDM(op_ldm_s2_c16, get_s16,  lpm_u16())
LDM(op_ldm_s4_c16, get_u32,  lpm_u16())
LDM(op_ldm_u1_c16, get_u8,   lpm_u16())
LDM(op_ldm_u2_c16, get_u16,  lpm_u16())

// 0xA0 - 0xBF, store to memory: address first, then the value
static void put_bool(uint32_t addr, uint32_t val) { set_bool(addr, val != 0); }

#define STM(name, set, addr) \
    static void name(void) { uint32_t a = (addr); uint32_t v = pop(); set(a, v); }

STM(op_stm_b1_s, put_bool, pop())
STM(op_stm_s1_s, set_u8,   pop())
STM(op_stm_s2_s, set_u16,  pop())
STM(op_stm_s4_s, set_u32,  pop())
STM(op_stm_b1_cs8, put_bool, pop() * 32 + lpm_u8())
STM(op_stm_s1_cs8, set_u8,   pop() * 4  + lpm_u8())
STM(op_stm_s2_cs8, set_u16,  pop() * 2  + lpm_u8())
STM(op_stm_s4_cs8, set_u32,  pop()      + lpm_u8())
STM(op_stm_b1_cs16, put_bool, pop() * 32 + lpm_u16())
STM(op_stm_s1_cs16, set_u8,   pop() * 4  + lpm_u16())
STM(op_stm_s2_cs16, set_u16,  pop() * 2  + lpm_u16())
STM(op_stm_s4_cs16, set_u32,  pop()      + lpm_u16())
STM(op_stm_b1_c16, put_bool, lpm_u16())
STM(op_stm_s1_c16, set_u8,   lpm_u16())
STM(op_stm_s2_c16, set_u16,  lpm_u16())
STM(op_stm_s4_c16, set_u32,  lpm_u16())

// 0xC0 - 0xCF
// X13 IO objects are not supported, IO is done by API functions
static void op_in_out(void) { s.stat = PLC_ERR_UNK_COP; }

static void op_api(void)
{
    uint8_t func = (uint8_t)lpm_u8();
    if (s.stat != PLC_RUN) {
        return;
    }

    plc_status_t stat = (s.api != NULL) ? s.api(func) : PLC_ERR_UNK_API;
    if ((stat != PLC_RUN) && (s.stat == PLC_RUN)) {
        s.stat = stat;
    }
}

// Load from program memory: index from the stack + 16-bit base
static void op_lpm_s1(void) { uint32_t a = pop() + lpm_u16();     uint32_t v = prg_read(a, 1); push((v & 0x80) ? (v | 0xFFFFFF00) : v); }
static void op_lpm_u1(void) { uint32_t a = pop() + lpm_u16();     push(prg_read(a, 1)); }
static void op_lpm_s2(void) { uint32_t a = 2 * pop() + lpm_u16(); uint32_t v = prg_read(a, 2); push((v & 0x8000) ? (v | 0xFFFF0000) : v); }
static void op_lpm_u2(void) { uint32_t a = 2 * pop() + lpm_u16(); push(prg_read(a, 2)); }
static void op_lpm_s4(void) { uint32_t a = 4 * pop() + lpm_u16(); push(prg_read(a, 4)); }

// 0xF0 - 0xFF, control flow
static void op_sjmp(void) { if (HAS1) { s.pc = s.ram[s.sp++]; } else { s.stat = PLC_ERR_OFR_SP; } }

static void op_jz(void)
{
    if (!HAS1) {
        s.stat = PLC_ERR_OFR_SP;
    } else if (s.ram[s.sp++] == 0) {
        s.pc = lpm_u16();
    } else {
        s.pc += 2;
    }
}

static void op_jnz(void)
{
    if (!HAS1) {
        s.stat = PLC_ERR_OFR_SP;
    } else if (s.ram[s.sp++] != 0) {
        s.pc = lpm_u16();
    } else {
        s.pc += 2;
    }
}

static void op_jmp(void) { s.pc = lpm_u16(); }

// Call address from the stack: ( addr -- ret_pc sfp )
static void op_scall(void)
{
    if (HAS1 && ROOM) {
        uint32_t addr = s.ram[s.sp];
        s.ram[s.sp] = s.pc;
        s.ram[--s.sp] = s.sfp;
        s.sfp = s.sp;
        s.pc = addr;
    } else {
        s.stat = PLC_ERR_OFR_SP;
    }
}

static void op_call(void)
{
    if (s.sp > s.stack_bot + 1) {       // X13: (sp - 1) > stack_bot, wrapped around at sp == 0
        uint32_t addr = lpm_u16();
        s.ram[--s.sp] = s.pc;
        s.ram[--s.sp] = s.sfp;
        s.sfp = s.sp;
        s.pc = addr;
    } else {
        s.stat = PLC_ERR_OFR_SP;
    }
}

static void op_check_idx(void)
{
    if (!HAS1) {
        s.stat = PLC_ERR_OFR_SP;
        return;
    }
    int32_t idx = (int32_t)s.ram[s.sp];
    if ((idx < 0) || ((uint32_t)idx >= lpm_u16())) {
        s.stat = PLC_ERR_OFR_IDX;
    }
}

static void op_test_eq(void)
{
    if (!HAS1) {
        s.stat = PLC_ERR_OFR_SP;
    } else if (s.ram[s.sp] != lpm_u32()) {
        s.stat = PLC_ERR_TEST;
    } else {
        s.sp++;
    }
}

static void op_ret(void)
{
    if (s.sfp < s.ram_words - 1) {
        s.sp = s.sfp;
        s.sfp = s.ram[s.sp++];
        s.pc = s.ram[s.sp++];
    } else if (s.sfp == s.ram_words) {
        s.stat = PLC_OK;                // Return from the cycle
    } else {
        s.stat = PLC_ERR_OFR_SFP;
    }
}

static void op_unk(void) { s.stat = PLC_ERR_UNK_COP; }

#define UNK op_unk

static const op_t s_ops[256] = {
    // 0x00
    op_nop,         UNK,            op_dup,         op_drop,        op_nip,         op_swap,        op_over,        op_rot,
    op_not,         op_and,         op_or,          op_xor,         op_lsl,         op_lsr,         op_asr,         UNK,
    op_add,         op_sub,         op_mul,         op_div,         op_mod,         op_inc,         op_dec,         op_neg,
    op_fadd,        op_fsub,        op_fmul,        op_fdiv,        op_fneg,        op_fabs,        op_fmin,        op_fmax,
    // 0x20
    op_ceq,         op_cne,         op_cgt,         op_cge,         op_clt,         op_cle,         op_cze,         UNK,
    op_fceq,        op_fcne,        op_fcgt,        op_fcge,        op_fclt,        op_fcle,        op_fcze,        op_fisfinite,
    op_s32tof,      op_u32tof,      op_ftos32,      op_ftou32,      op_fround,      op_ftrunc,      op_fisnan,      op_fisinf,
    op_ldi_0,       op_ldi_s1,      op_ldi_s2,      op_ldi_s4,      op_ldi_u1,      op_ldi_u2,      op_ldi_1,       op_ldi_m1,
    // 0x40
    op_ld_p0,       op_ld_p1,       op_ld_p2,       op_ld_p3,       op_ld_p4,       op_ld_p5,       op_ld_p6,       op_ld_p7,
    op_ld_p8,       op_ld_p9,       op_ld_pA,       op_ld_pB,       op_ld_pC,       op_ld_pD,       op_ld_pE,       op_ld_pF,
    op_ld_l0,       op_ld_l1,       op_ld_l2,       op_ld_l3,       op_ld_l4,       op_ld_l5,       op_ld_l6,       op_ld_l7,
    op_ld_l8,       op_ld_l9,       op_ld_lA,       op_ld_lB,       op_ld_lC,       op_ld_lD,       op_ld_lE,       op_ld_lF,
    // 0x60
    op_st_p0,       op_st_p1,       op_st_p2,       op_st_p3,       op_st_p4,       op_st_p5,       op_st_p6,       op_st_p7,
    op_st_p8,       op_st_p9,       op_st_pA,       op_st_pB,       op_st_pC,       op_st_pD,       op_st_pE,       op_st_pF,
    op_st_l0,       op_st_l1,       op_st_l2,       op_st_l3,       op_st_l4,       op_st_l5,       op_st_l6,       op_st_l7,
    op_st_l8,       op_st_l9,       op_st_lA,       op_st_lB,       op_st_lC,       op_st_lD,       op_st_lE,       op_st_lF,
    // 0x80
    op_ldm_b1_s,    op_ldm_s1_s,    op_ldm_s2_s,    op_ldm_s4_s,    op_ldm_u1_s,    op_ldm_u2_s,    UNK,            op_ldi_min,
    op_ldm_b1_cs8,  op_ldm_s1_cs8,  op_ldm_s2_cs8,  op_ldm_s4_cs8,  op_ldm_u1_cs8,  op_ldm_u2_cs8,  UNK,            UNK,
    op_ldm_b1_cs16, op_ldm_s1_cs16, op_ldm_s2_cs16, op_ldm_s4_cs16, op_ldm_u1_cs16, op_ldm_u2_cs16, UNK,            UNK,
    op_ldm_b1_c16,  op_ldm_s1_c16,  op_ldm_s2_c16,  op_ldm_s4_c16,  op_ldm_u1_c16,  op_ldm_u2_c16,  UNK,            UNK,
    // 0xA0
    op_stm_b1_s,    op_stm_s1_s,    op_stm_s2_s,    op_stm_s4_s,    UNK,            UNK,            UNK,            UNK,
    op_stm_b1_cs8,  op_stm_s1_cs8,  op_stm_s2_cs8,  op_stm_s4_cs8,  UNK,            UNK,            UNK,            UNK,
    op_stm_b1_cs16, op_stm_s1_cs16, op_stm_s2_cs16, op_stm_s4_cs16, UNK,            UNK,            UNK,            UNK,
    op_stm_b1_c16,  op_stm_s1_c16,  op_stm_s2_c16,  op_stm_s4_c16,  UNK,            UNK,            UNK,            UNK,
    // 0xC0
    op_in_out,      op_in_out,      UNK,            UNK,            UNK,            UNK,            op_api,         UNK,
    UNK,            op_lpm_s1,      op_lpm_s2,      op_lpm_s4,      op_lpm_u1,      op_lpm_u2,      UNK,            UNK,
    UNK,            UNK,            UNK,            UNK,            UNK,            UNK,            UNK,            UNK,
    UNK,            UNK,            UNK,            UNK,            UNK,            UNK,            UNK,            UNK,
    // 0xE0
    UNK,            UNK,            UNK,            UNK,            UNK,            UNK,            UNK,            UNK,
    UNK,            UNK,            UNK,            UNK,            UNK,            UNK,            UNK,            UNK,
    op_sjmp,        op_jz,          op_jnz,         op_jmp,         op_scall,       UNK,            UNK,            op_call,
    UNK,            UNK,            UNK,            UNK,            UNK,            op_check_idx,   op_test_eq,     op_ret,
};

////////////////////////////////////////////////////////////////////////
// API

void plc_vm_init(const plc_vm_config_t *cfg)
{
    s.prg = cfg->prg;
    s.prg_size = cfg->prg_size;
    s.ram = cfg->ram;
    s.ram_words = cfg->ram_words;
    s.api = cfg->api;
    s.stack_bot = 0;
    s.sp = s.ram_words;
    s.sfp = s.ram_words;
    s.pc = 0;
    s.stat = PLC_OK;
    s.first_start = true;
}

void plc_vm_restart(void)
{
    s.first_start = true;
}

plc_status_t plc_vm_cycle(void)
{
    s.sfp = s.ram_words;
    s.sp = s.ram_words;
    s.pc = s.first_start ? 0 : CYCLE_ENTRY;
    s.first_start = false;
    s.stat = PLC_RUN;

    uint32_t wd = WATCHDOG;
    while (s.stat == PLC_RUN) {
        if (--wd == 0) {
            s.stat = PLC_ERR_WD;
            break;
        }
        if (s.pc >= s.prg_size) {
            s.stat = PLC_ERR_OFR_PC;
            break;
        }
        s_ops[s.prg[s.pc++]]();
    }
    return s.stat;
}

uint32_t plc_vm_stack_bottom(void)
{
    return s.stack_bot;
}

void plc_vm_set_stack_bottom(uint32_t words)
{
    s.stack_bot = words;
}

void plc_vm_regs(plc_vm_regs_t *regs)
{
    regs->sp = s.sp;
    regs->top = ((s.sp > 0) && (s.sp < s.ram_words)) ? s.ram[s.sp - 1] : 0xFFFFFFFF;
    regs->sfp = s.sfp;
    regs->pc = s.pc;
}
