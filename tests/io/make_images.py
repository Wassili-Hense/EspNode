"""Program images for checking the PLC IO on a board, without the Enviriot compiler.

    python tests/io/make_images.py              # writes tests/io/build/*.bin
    python tools/gw_sim.py COM4                 # then: plc upload tests/io/build/blink.bin 4

Scenarios, jumpers and expected values: tests/io/README.md. Numbers: docs/plc-api.md.
"""
from pathlib import Path

# Opcodes
DUP, DROP = 0x02, 0x03
LDI_0, LDI_U1, LDI_U2, LDI_S4, LDI_1 = 0x38, 0x3C, 0x3D, 0x3B, 0x3E
AND, ADD, DIV = 0x09, 0x10, 0x13
CEQ, CGT = 0x20, 0x22
LDM, STM = 0x9B, 0xBB                   # LDM_S4_C16 / STM_S4_C16: 32-bit word, 16-bit address
IN, OUT, API = 0xC0, 0xC1, 0xC6
JZ, JNZ, JMP, RET = 0xF1, 0xF2, 0xF3, 0xFF

# API functions
MILLIS, SECONDS = 6, 7
CONFIG_PIN1, CONFIG_PIN2 = 30, 31
TWI_CONTROL, TWI_STATUS = 1, 2        # Bus 0, as X13
UART_AVAIL, UART_GET, UART_PUT = 21, 22, 23
TWI_RDY, TWI_FAILED = 0x10 << 8, 0xE0 << 8     # Status bits: access << 8

# Pin modes
IN_PU, OUT_ = 2, 5
ANALOG, DAC, PWM, CNT_RISE = 9, 10, 11, 13
TWI0_SDA, TWI0_SCL, UART2_TX, UART2_RX = 16, 17, 23, 24

STACK_BOTTOM = 4                        # Md0..Md3 are the data of every program


class Asm:
    """Code at a known address; jumps by label."""

    def __init__(self):
        self.code = []
        self.labels = {}
        self.fixups = []

    def op(self, *b):
        self.code += b
        return self

    def ldi(self, v):
        if v == 0:
            return self.op(LDI_0)
        if v == 1:
            return self.op(LDI_1)
        if 0 <= v <= 0xFF:
            return self.op(LDI_U1, v)
        if 0 <= v <= 0xFFFF:
            return self.op(LDI_U2, v & 0xFF, v >> 8)
        v &= 0xFFFFFFFF
        return self.op(LDI_S4, *v.to_bytes(4, 'little'))

    def ld(self, word):                 # push Md<word>
        return self.op(LDM, word & 0xFF, word >> 8)

    def st(self, word):                 # Md<word> = pop
        return self.op(STM, word & 0xFF, word >> 8)

    def io(self, opcode, pin):          # [place] [type] [base L] [base H], place and type 0
        return self.op(opcode, 0, 0, pin, 0)

    def call(self, func, *args):        # The compiler pushes the last argument first
        for a in reversed(args):
            self.ldi(a)
        return self.op(API, func)

    def label(self, name):
        self.labels[name] = len(self.code)
        return self

    def jump(self, opcode, name):
        self.fixups.append((len(self.code) + 1, name))
        return self.op(opcode, 0, 0)

    def assemble(self, base):
        out = bytearray(self.code)
        for at, name in self.fixups:
            out[at:at + 2] = (base + self.labels[name]).to_bytes(2, 'little')
        return bytes(out)


def image(init, cycle):
    """Address 0 jumps to init, the cycle starts at 4, init follows the cycle."""
    cycle_code = cycle.op(RET).assemble(4)
    init_at = 4 + len(cycle_code)
    return bytes([JMP, init_at & 0xFF, init_at >> 8, RET]) + cycle_code + init.op(RET).assemble(init_at)


def blink(period_ms):
    """GPIO2 (devkit LED) toggles every period_ms; Md0 = its state."""
    init = Asm().call(CONFIG_PIN1, 2, OUT_)
    cycle = Asm().call(MILLIS).ldi(period_ms).op(DIV).ldi(1).op(AND).io(OUT, 2).io(IN, 2).st(0)
    return image(init, cycle)


def pwm_count():
    """PWM 100 Hz 50 % on GPIO4, counter of rising edges on GPIO5 (jumper 4-5), input with pull-up
    on GPIO18. Md0 = count, Md1 = GPIO18, Md2 written by the gateway -> sets the count."""
    init = Asm().call(CONFIG_PIN2, 4, PWM, 100).call(CONFIG_PIN2, 5, CNT_RISE, 1000) \
        .call(CONFIG_PIN2, 18, IN_PU, 20).ldi(0xFFFFFFFF).st(2)
    cycle = Asm().ldi(32768).io(OUT, 4) \
        .ld(2).ldi(0xFFFFFFFF).op(CEQ).jump(JNZ, 'read') \
        .ld(2).io(OUT, 5).ldi(0xFFFFFFFF).st(2) \
        .label('read').io(IN, 5).st(0).io(IN, 18).st(1)
    return image(init, cycle)


def adc_dac():
    """DAC GPIO25 = Md1 (0..255, written by the gateway), Md0 = GPIO34 in mV, 12 dB (jumper 25-34)."""
    init = Asm().call(CONFIG_PIN1, 25, DAC).call(CONFIG_PIN2, 34, ANALOG, 3)
    cycle = Asm().ld(1).io(OUT, 25).io(IN, 34).st(0)
    return image(init, cycle)


def twi_scan():
    """Probes the addresses 0x08..0x77 one by one, bus 0 on SDA 21 / SCL 22, 100 kHz, without waiting:
    TwiControl(address) starts a probe, the next cycles poll TwiStatus().
    Md0 = last address found, Md2 = devices found by the last full pass, Md1 = address, Md3 = count."""
    init = Asm().call(CONFIG_PIN1, 21, TWI0_SDA).call(CONFIG_PIN2, 22, TWI0_SCL, 100).ldi(8).st(1)
    cycle = Asm().op(API, TWI_STATUS).op(DUP).jump(JNZ, 'check') \
        .op(DROP).ld(1).op(API, TWI_CONTROL).jump(JMP, 'end') \
        .label('check').op(DUP).ldi(TWI_RDY).op(AND).jump(JZ, 'failed') \
        .op(DROP).ld(1).st(0).ld(3).ldi(1).op(ADD).st(3).jump(JMP, 'next') \
        .label('failed').ldi(TWI_FAILED).op(AND).jump(JZ, 'end') \
        .label('next').ld(1).ldi(1).op(ADD).st(1) \
        .ld(1).ldi(0x77).op(CGT).jump(JZ, 'end') \
        .ld(3).st(2).ldi(0).st(3).ldi(8).st(1) \
        .label('end')
    return image(init, cycle)


def uart_loop():
    """UART2 9600 8N1, TX GPIO4 -> RX GPIO5 (jumper). Sends getSeconds() & 0xFF once a second with
    UartPutByte, reads it back when UartBytesToRead reports it (a cycle after it arrived).
    Md0 = last byte received, Md1 = result of UartPutByte, Md2 = bytes received, Md3 = second of the
    last send."""
    init = Asm().call(CONFIG_PIN2, 4, UART2_TX, 9600).call(CONFIG_PIN2, 5, UART2_RX, 0)
    cycle = Asm().call(SECONDS).ld(3).op(CEQ).jump(JNZ, 'rx') \
        .call(SECONDS).st(3).ld(3).ldi(0xFF).op(AND).ldi(2).op(API, UART_PUT).st(1) \
        .label('rx').call(UART_AVAIL, 2).jump(JZ, 'end') \
        .call(UART_GET, 2).st(0).ld(2).ldi(1).op(ADD).st(2) \
        .label('end')
    return image(init, cycle)


def err_flash():
    """ConfigPin1(6, OUT): flash pin -> stop 0x8A."""
    return image(Asm().call(CONFIG_PIN1, 6, OUT_), Asm())


def err_pwm9():
    """Nine PWM pins at one frequency, eight channels -> stop 0x89."""
    init = Asm()
    for pin in (2, 4, 5, 13, 14, 15, 18, 19, 21):
        init.call(CONFIG_PIN2, pin, PWM, 1000)
    return image(init, Asm())


IMAGES = {
    'blink': blink(500),
    'blink_fast': blink(100),
    'pwm_count': pwm_count(),
    'adc_dac': adc_dac(),
    'twi_scan': twi_scan(),
    'uart_loop': uart_loop(),
    'err_flash': err_flash(),
    'err_pwm9': err_pwm9(),
}


def main():
    out = Path(__file__).resolve().parent / 'build'
    out.mkdir(exist_ok=True)
    for name, data in IMAGES.items():
        (out / f'{name}.bin').write_bytes(data)
        print(f'{name}.bin {len(data)} bytes')
    print(f'stack bottom for all: {STACK_BOTTOM}')


if __name__ == '__main__':
    main()
