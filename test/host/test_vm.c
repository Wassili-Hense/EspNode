// Unit tests of plc_vm.c with hand-assembled bytecode
#include <stdio.h>
#include <string.h>

#include "plc_vm.h"

enum {
    NOP = 0x00, DUP = 0x02, DROP = 0x03, SWAP = 0x05, OVER = 0x06, ROT = 0x07,
    NOT = 0x08, AND = 0x09, OR = 0x0A, XOR = 0x0B, LSL = 0x0C, LSR = 0x0D, ASR = 0x0E,
    ADD = 0x10, SUB = 0x11, MUL = 0x12, DIV = 0x13, MOD = 0x14, INC = 0x15, DEC = 0x16, NEG = 0x17,
    CEQ = 0x20, CGT = 0x22, CLT = 0x24, CZE = 0x26,
    LDI_0 = 0x38, LDI_S1 = 0x39, LDI_S2 = 0x3A, LDI_S4 = 0x3B, LDI_U1 = 0x3C, LDI_U2 = 0x3D, LDI_1 = 0x3E, LDI_M1 = 0x3F,
    LD_P0 = 0x40, LD_L0 = 0x50, ST_P0 = 0x60, ST_L0 = 0x70,
    LDI_MIN = 0x87,
    LDM_B1_C16 = 0x98, LDM_S1_C16 = 0x99, LDM_S2_C16 = 0x9A, LDM_S4_C16 = 0x9B, LDM_U1_C16 = 0x9C, LDM_U2_C16 = 0x9D,
    LDM_S4_CS8 = 0x8B,
    STM_B1_C16 = 0xB8, STM_S1_C16 = 0xB9, STM_S2_C16 = 0xBA, STM_S4_C16 = 0xBB, STM_S4_CS8 = 0xAB,
    IN = 0xC0, OUT = 0xC1, API = 0xC6, LPM_S1 = 0xC9, LPM_U1 = 0xCC, LPM_U2 = 0xCD,
    JZ = 0xF1, JNZ = 0xF2, JMP = 0xF3, CALL = 0xF7, CHECK_IDX = 0xFD, TEST_EQ = 0xFE, RET = 0xFF,
};

#define W 64
static uint8_t prg[256];
static uint32_t ram[W];
static int failed;
static uint32_t api_args[4];
static int api_calls;
static int io_on;                   // IN/OUT handlers installed by load()
static uint32_t out_operand, out_value;

// Base 8 fails, others return the operand
static plc_status_t io_in(uint32_t operand, uint32_t *value)
{
    if ((operand >> 16) == 8) {
        return PLC_ERR_PARM;
    }
    *value = operand;
    return PLC_RUN;
}

static plc_status_t io_out(uint32_t operand, uint32_t value)
{
    out_operand = operand;
    out_value = value;
    return PLC_RUN;
}

static plc_status_t api(uint8_t func)
{
    api_calls++;
    if (func == 99) {               // (a, b) -> a * 10 + b
        api_args[0] = plc_vm_pop();
        api_args[1] = plc_vm_pop();
        plc_vm_push(api_args[0] * 10 + api_args[1]);
        return PLC_RUN;
    }
    if (func == 98) {
        return PLC_ERR_PARM;
    }
    return PLC_ERR_UNK_API;
}

// Load main code at 4 (address 0: RET, init does nothing)
static void load(const uint8_t *code, size_t len)
{
    memset(prg, 0xFF, sizeof(prg));
    memcpy(&prg[4], code, len);
    memset(ram, 0, sizeof(ram));
    plc_vm_config_t cfg = { prg, sizeof(prg), ram, W, api, io_on ? io_in : NULL, io_on ? io_out : NULL };
    plc_vm_init(&cfg);
    plc_vm_cycle();                 // first cycle: address 0
}

static void expect(const char *name, plc_status_t st, plc_status_t exp_st, uint32_t got, uint32_t exp)
{
    if (st != exp_st || got != exp) {
        printf("FAIL %s: status 0x%02X (exp 0x%02X), value 0x%08X (exp 0x%08X)\n", name, st, exp_st, got, exp);
        failed++;
    } else {
        printf("PASS %s\n", name);
    }
}

#define RUN(name, exp_st, idx, exp, ...) do { \
    const uint8_t code_[] = { __VA_ARGS__ }; load(code_, sizeof(code_)); \
    plc_status_t st_ = plc_vm_cycle(); expect(name, st_, exp_st, ram[idx], exp); } while (0)

// Store result to ram[0]
#define TO0 STM_S4_C16, 0, 0, RET

enum {
    FADD = 0x18, FSUB, FMUL, FDIV, FNEG, FABS, FMIN, FMAX,
    FCEQ = 0x28, FCNE, FCGT, FCGE, FCLT, FCLE, FCZE, FISFINITE,
    S32TOF = 0x30, U32TOF, FTOS32, FTOU32, FROUND, FTRUNC, FISNAN, FISINF,
};

// Load a 32-bit pattern (float bits)
#define F4(b) LDI_S4, (uint8_t)(b), (uint8_t)((b) >> 8), (uint8_t)((b) >> 16), (uint8_t)((b) >> 24)
#define F_1_5   0x3FC00000u
#define F_2_5   0x40200000u
#define F_M1_5  0xBFC00000u
#define F_M2_5  0xC0200000u
#define F_1     0x3F800000u
#define F_0     0x00000000u
#define F_M0    0x80000000u
#define F_NAN   0x7FC00000u
#define F_INF   0x7F800000u
#define F_MINF  0xFF800000u
#define F_3E9   0x4F32D05Eu     // 3000000000
#define F_1E10  0x501502F9u     // 10000000000
#define F_HALFM 0x3EFFFFFFu     // 0.49999997
#define F_M0_4  0xBECCCCCDu     // -0.4

int main(void)
{
    RUN("sub", PLC_OK, 0, 2, LDI_U1, 7, LDI_U1, 5, SUB, TO0);
    RUN("neg const", PLC_OK, 0, (uint32_t)-3, LDI_S1, 0xFD, TO0);
    RUN("s2 const", PLC_OK, 0, (uint32_t)-1000, LDI_S2, 0x18, 0xFC, TO0);
    RUN("s4 const", PLC_OK, 0, 0x12345678, LDI_S4, 0x78, 0x56, 0x34, 0x12, TO0);
    RUN("mul wrap", PLC_OK, 0, 0x00000000, LDI_S4, 0, 0, 1, 0, LDI_S4, 0, 0, 1, 0, MUL, TO0);
    RUN("div", PLC_OK, 0, (uint32_t)-3, LDI_S1, 0xF9, LDI_U1, 2, DIV, TO0);           // -7/2 = -3
    RUN("mod", PLC_OK, 0, (uint32_t)-1, LDI_S1, 0xF9, LDI_U1, 2, MOD, TO0);           // -7%2 = -1
    RUN("div 0", PLC_ERR_DIV0, 0, 0, LDI_1, LDI_0, DIV, TO0);
    RUN("mod 0", PLC_ERR_DIV0, 0, 0, LDI_1, LDI_0, MOD, TO0);
    RUN("min / -1", PLC_OK, 0, 0x80000000, LDI_MIN, LDI_M1, DIV, TO0);
    RUN("lsl 40", PLC_OK, 0, 0, LDI_1, LSL, 40, TO0);
    RUN("lsl 31", PLC_OK, 0, 0x80000000, LDI_1, LSL, 31, TO0);
    RUN("asr 40", PLC_OK, 0, 0xFFFFFFFF, LDI_MIN, ASR, 40, TO0);
    RUN("lsr 31", PLC_OK, 0, 1, LDI_MIN, LSR, 31, TO0);
    RUN("cgt signed", PLC_OK, 0, 1, LDI_1, LDI_M1, CGT, TO0);                           // 1 > -1
    RUN("cze", PLC_OK, 0, 1, LDI_0, CZE, TO0);
    RUN("swap", PLC_OK, 0, 2, LDI_U1, 1, LDI_U1, 2, SWAP, DROP, TO0);                   // (1 2 -- 2 1), drop 1
    RUN("rot", PLC_OK, 0, 1, LDI_U1, 1, LDI_U1, 2, LDI_U1, 3, ROT, TO0);                // (1 2 3 -- 2 3 1)
    RUN("over", PLC_OK, 0, 1, LDI_U1, 1, LDI_U1, 2, OVER, TO0);
    // Bit/byte/half-word memory, little-endian inside the words
    RUN("bool store", PLC_OK, 1, 0x2, LDI_1, STM_B1_C16, 33, 0, RET);
    RUN("u8 store", PLC_OK, 1, 0x00AB0000, LDI_U1, 0xAB, STM_S1_C16, 6, 0, RET);
    RUN("s8 load", PLC_OK, 0, (uint32_t)-2, LDI_U1, 0xFE, STM_S1_C16, 5, 0, LDM_S1_C16, 5, 0, TO0);
    RUN("u16 store", PLC_OK, 1, 0xBEEF0000, LDI_U2, 0xEF, 0xBE, STM_S2_C16, 3, 0, RET);
    RUN("s16 load", PLC_OK, 0, (uint32_t)-2, LDI_U2, 0xFE, 0xFF, STM_S2_C16, 3, 0, LDM_S2_C16, 3, 0, TO0);
    RUN("u16 load", PLC_OK, 0, 0xFFFE, LDI_U2, 0xFE, 0xFF, STM_S2_C16, 3, 0, LDM_U2_C16, 3, 0, TO0);
    RUN("bool load", PLC_OK, 0, 1, LDI_1, STM_B1_C16, 70, 0, LDM_B1_C16, 70, 0, TO0);
    RUN("indexed", PLC_OK, 3, 9, LDI_U1, 9, LDI_U1, 1, STM_S4_CS8, 2, RET);             // ram[1 + 2] = 9
    RUN("ram range", PLC_ERR_OFR_RAM, 0, 0, LDI_1, STM_S4_C16, 0, 1, RET);              // addr 256 >= 64
    // Calls: f(5) increments its parameter, result returned in place
    RUN("call", PLC_OK, 0, 6, LDI_U1, 5, CALL, 13, 0, TO0, NOP, LD_P0, INC, ST_P0, RET);
    // Loop: ram[0] = 10 iterations
    RUN("loop", PLC_OK, 0, 10, LDI_U1, 10, /*6*/ DUP, JZ, 21, 0, LDM_S4_C16, 0, 0, INC, STM_S4_C16, 0, 0, DEC, JMP, 6, 0, /*21*/ DROP, RET);
    RUN("watchdog", PLC_ERR_WD, 0, 0, JMP, 4, 0);
    RUN("pc range", PLC_ERR_OFR_PC, 0, 0, JMP, 0x00, 0x01);                            // 256 >= program size
    RUN("stack underflow", PLC_ERR_OFR_SP, 0, 0, DROP, RET);
    RUN("unknown op", PLC_ERR_UNK_COP, 0, 0, 0x01);
    RUN("IN without handler", PLC_ERR_UNK_COP, 0, 0, IN, 0, 0, 0, 0);
    RUN("OUT without handler", PLC_ERR_UNK_COP, 0, 0, LDI_1, OUT, 0, 0, 0, 0, RET);
    // IN/OUT: operand [place][type][base L][base H] = place | type << 8 | base << 16
    io_on = 1;
    RUN("IN operand", PLC_OK, 0, 0x0107BBAA, IN, 0xAA, 0xBB, 0x07, 0x01, TO0);
    RUN("IN error", PLC_ERR_PARM, 0, 0, IN, 0, 0, 8, 0, TO0);
    RUN("OUT", PLC_OK, 0, 0, LDI_U1, 42, OUT, 1, 2, 3, 0, RET);
    expect("OUT operand", PLC_OK, PLC_OK, out_operand, 0x00030201);
    expect("OUT value", PLC_OK, PLC_OK, out_value, 42);
    RUN("OUT underflow", PLC_ERR_OFR_SP, 0, 0, OUT, 0, 0, 2, 0, RET);
    {
        const uint8_t code[] = { IN, 0, 0, 1, 0, RET };
        load(code, sizeof(code));
        plc_vm_set_stack_bottom(W);
        expect("IN overflow", plc_vm_cycle(), PLC_ERR_OFR_SP, 0, 0);
    }
    io_on = 0;
    RUN("check_idx ok", PLC_OK, 0, 3, LDI_U1, 3, CHECK_IDX, 4, 0, TO0);
    RUN("check_idx", PLC_ERR_OFR_IDX, 0, 0, LDI_U1, 4, CHECK_IDX, 4, 0, TO0);
    RUN("test_eq", PLC_ERR_TEST, 0, 0, LDI_U1, 4, TEST_EQ, 5, 0, 0, 0, RET);
    // LPM: table at 32 in the program
    {
        const uint8_t code[] = { LDI_U1, 2, LPM_U1, 32, 0, TO0 };
        load(code, sizeof(code));
        prg[32] = 0x11; prg[33] = 0x22; prg[34] = 0x83;
        expect("lpm u1", plc_vm_cycle(), PLC_OK, ram[0], 0x83);
        const uint8_t code2[] = { LDI_U1, 2, LPM_S1, 32, 0, TO0 };
        load(code2, sizeof(code2));
        prg[34] = 0x83;
        expect("lpm s1", plc_vm_cycle(), PLC_OK, ram[0], 0xFFFFFF83);
    }
    // API: arguments are popped in their order, the compiler pushes the last one first
    api_calls = 0;
    RUN("api order", PLC_OK, 0, 12, LDI_U1, 2, LDI_U1, 1, API, 99, TO0);
    RUN("api error", PLC_ERR_PARM, 0, 0, API, 98, RET);
    RUN("api unknown", PLC_ERR_UNK_API, 0, 0, API, 7, RET);
    // First cycle from 0 (init), next ones from 4
    {
        memset(prg, 0xFF, sizeof(prg));
        const uint8_t init[] = { JMP, 16, 0 };              // 0: init at 16
        const uint8_t main_[] = { LDM_S4_C16, 0, 0, INC, STM_S4_C16, 0, 0, RET };
        const uint8_t init_code[] = { LDI_U1, 100, STM_S4_C16, 1, 0, RET };
        memcpy(&prg[0], init, sizeof(init));
        memcpy(&prg[4], main_, sizeof(main_));
        memcpy(&prg[16], init_code, sizeof(init_code));
        memset(ram, 0, sizeof(ram));
        plc_vm_config_t cfg = { prg, sizeof(prg), ram, W, api, NULL, NULL };
        plc_vm_init(&cfg);
        plc_vm_cycle();
        plc_vm_cycle();
        plc_vm_cycle();
        expect("init once", PLC_OK, PLC_OK, ram[1], 100);
        expect("cycles from 4", PLC_OK, PLC_OK, ram[0], 2);
        plc_vm_restart();
        plc_vm_cycle();
        expect("restart runs init", PLC_OK, PLC_OK, ram[0], 2);
    }
    // Float
    RUN("fadd", PLC_OK, 0, 0x40800000, F4(F_1_5), F4(F_2_5), FADD, TO0);              // 4.0
    RUN("fsub order", PLC_OK, 0, 0xBF800000, F4(F_1_5), F4(F_2_5), FSUB, TO0);        // 1.5 - 2.5 = -1
    RUN("fmul", PLC_OK, 0, 0x40700000, F4(F_1_5), F4(F_2_5), FMUL, TO0);              // 3.75
    RUN("fdiv", PLC_OK, 0, 0x3F19999A, F4(F_1_5), F4(F_2_5), FDIV, TO0);              // 0.6
    RUN("fdiv 0", PLC_OK, 0, F_INF, F4(F_1), LDI_0, FDIV, TO0);
    RUN("fdiv 0/0 nan", PLC_OK, 0, 1, LDI_0, LDI_0, FDIV, FISNAN, TO0);
    RUN("fneg", PLC_OK, 0, F_M1_5, F4(F_1_5), FNEG, TO0);
    RUN("fneg 0", PLC_OK, 0, F_M0, LDI_0, FNEG, TO0);
    RUN("fabs", PLC_OK, 0, F_1_5, F4(F_M1_5), FABS, TO0);
    RUN("fmax", PLC_OK, 0, F_2_5, F4(F_1_5), F4(F_2_5), FMAX, TO0);
    RUN("fmin", PLC_OK, 0, F_1_5, F4(F_2_5), F4(F_1_5), FMIN, TO0);
    RUN("fmin nan", PLC_OK, 0, 1, F4(F_1_5), F4(F_NAN), FMIN, FISNAN, TO0);
    RUN("fmax nan", PLC_OK, 0, 1, F4(F_NAN), F4(F_1_5), FMAX, FISNAN, TO0);
    RUN("fmin -0 +0", PLC_OK, 0, F_M0, F4(F_0), F4(F_M0), FMIN, TO0);
    RUN("fmax -0 +0", PLC_OK, 0, F_0, F4(F_M0), F4(F_0), FMAX, TO0);
    RUN("fcgt order", PLC_OK, 0, 1, F4(F_2_5), F4(F_1_5), FCGT, TO0);                 // 2.5 > 1.5
    RUN("fclt", PLC_OK, 0, 0, F4(F_2_5), F4(F_1_5), FCLT, TO0);
    RUN("fcge eq", PLC_OK, 0, 1, F4(F_1_5), F4(F_1_5), FCGE, TO0);
    RUN("fcle neg", PLC_OK, 0, 1, F4(F_M2_5), F4(F_1_5), FCLE, TO0);
    RUN("fceq nan", PLC_OK, 0, 0, F4(F_NAN), F4(F_NAN), FCEQ, TO0);
    RUN("fcne nan", PLC_OK, 0, 1, F4(F_NAN), F4(F_NAN), FCNE, TO0);
    RUN("fceq -0 +0", PLC_OK, 0, 1, F4(F_M0), LDI_0, FCEQ, TO0);
    RUN("fcze -0", PLC_OK, 0, 1, F4(F_M0), FCZE, TO0);
    RUN("fcze nan", PLC_OK, 0, 0, F4(F_NAN), FCZE, TO0);
    RUN("fisfinite inf", PLC_OK, 0, 0, F4(F_INF), FISFINITE, TO0);
    RUN("fisfinite", PLC_OK, 0, 1, F4(F_1_5), FISFINITE, TO0);
    RUN("fisinf -inf", PLC_OK, 0, 1, F4(F_MINF), FISINF, TO0);
    RUN("fisnan", PLC_OK, 0, 0, F4(F_INF), FISNAN, TO0);
    RUN("s32tof", PLC_OK, 0, 0xC0400000, LDI_S1, 0xFD, S32TOF, TO0);                 // -3.0
    RUN("u32tof", PLC_OK, 0, 0x4F800000, LDI_M1, U32TOF, TO0);                       // 4294967296.0
    RUN("ftos32 -1.5", PLC_OK, 0, 0xFFFFFFFF, F4(F_M1_5), FTOS32, TO0);
    RUN("ftou32 -1.5", PLC_OK, 0, 0xFFFFFFFF, F4(F_M1_5), FTOU32, TO0);               // (-1.5) >>> 0
    RUN("ftos32 3e9", PLC_OK, 0, 0xB2D05E00, F4(F_3E9), FTOS32, TO0);                 // -1294967296
    RUN("ftos32 1e10", PLC_OK, 0, 0x540BE400, F4(F_1E10), FTOS32, TO0);               // mod 2^32
    RUN("ftos32 nan", PLC_OK, 0, 0, F4(F_NAN), FTOS32, TO0);
    RUN("ftos32 inf", PLC_OK, 0, 0, F4(F_INF), FTOS32, TO0);
    RUN("ftos32 0.5", PLC_OK, 0, 0, F4(0x3F000000u), FTOS32, TO0);
    RUN("ftos32 2.5", PLC_OK, 0, 2, F4(F_2_5), FTOS32, TO0);
    RUN("fround 2.5", PLC_OK, 0, 0x40400000, F4(F_2_5), FROUND, TO0);                 // 3
    RUN("fround -2.5", PLC_OK, 0, 0xC0000000, F4(F_M2_5), FROUND, TO0);               // -2
    RUN("fround 0.49999997", PLC_OK, 0, F_0, F4(F_HALFM), FROUND, TO0);
    RUN("fround -0.4", PLC_OK, 0, F_M0, F4(F_M0_4), FROUND, TO0);                     // -0
    RUN("fround inf", PLC_OK, 0, F_INF, F4(F_INF), FROUND, TO0);
    RUN("ftrunc -1.5", PLC_OK, 0, 0xBF800000, F4(F_M1_5), FTRUNC, TO0);
    // Stack bottom: data below it is protected from pushes
    {
        const uint8_t code[] = { LDI_1, LDI_1, LDI_1, RET };
        load(code, sizeof(code));
        plc_vm_set_stack_bottom(W - 2);
        expect("stack bottom", plc_vm_cycle(), PLC_ERR_OFR_SP, 0, 0);
        plc_vm_regs_t r;
        plc_vm_regs(&r);
        expect("regs sp", PLC_OK, PLC_OK, r.sp, W - 2);
        expect("regs pc", PLC_OK, PLC_OK, r.pc, 7);
    }

    printf(failed ? "%d FAILED\n" : "ALL PASSED\n", failed);
    return failed ? 1 : 0;
}
