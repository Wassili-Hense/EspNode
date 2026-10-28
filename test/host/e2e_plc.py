"""End-to-end PLC: upload like Enviriot DevicePLC.cs, merkers, error report, persistence."""
import struct
import sys
import time

import harness as hn
from harness import Harness, check, g

START_REQ, STOP_REQ, CRC_REQ, WRITE_REQ, SET_STACK_REQ = 1, 3, 5, 7, 11


crc16 = g.plc_crc16


class Plc:
    def __init__(self, h):
        self.h = h
        self.tid = next(t for t, n in h.gw.topics.items() if n == 'pa1')

    def responses(self, since):
        return [m[7:] for t, d, m in self.h.log
                if d == 'node' and t >= since and m[1] == g.PUBLISH and g.u16(m, 3) == self.tid]

    def cmd(self, data, wait_ms=2000):
        n = len(self.responses(0))
        self.h.gw.publish(self.tid, bytes(data))
        self.h.run(wait_ms, until=lambda: len(self.responses(0)) > n)
        r = self.responses(0)
        return r[n] if len(r) > n else None

    def upload(self, program, stack_bottom):
        """DevicePLC.cs: CRC check, stop, write 32-byte blocks, stack bottom, start."""
        r = self.cmd([CRC_REQ, 0, 0, len(program) & 0xFF, len(program) >> 8])
        if r == bytes([6, 0]) + crc16(program).to_bytes(2, 'little'):
            return 'same'
        assert self.cmd([STOP_REQ])[:2] == bytes([4, 0])
        for offs in range(0, len(program), 32):
            block = program[offs:offs + 32]
            req = bytes([WRITE_REQ, offs & 0xFF, offs >> 8]) + block + crc16(block).to_bytes(2, 'little')
            r = self.cmd(req)
            assert r == bytes([8, 0]), f'write {offs}: {r}'
        assert self.cmd([SET_STACK_REQ] + list(stack_bottom.to_bytes(4, 'little'))) == bytes([12, 0])
        return self.cmd([START_REQ])


# Programs: address 0 - init (RET), 4 - cycle
def program(*cycle):
    return bytes([0xFF, 0, 0, 0]) + bytes(cycle)


COUNTER = program(0x9B, 0, 0, 0x15, 0xBB, 0, 0, 0xFF)       # ram[0]++
DIV0 = program(0x3E, 0x38, 0x13, 0xFF)                       # 1 / 0
NODESTATUS = program(0xC6, 5, 0xBB, 0, 0, 0xFF)              # ram[0] = NodeStatus()
FLOATSTEP = program(0x9B, 1, 0, 0x3B, 0, 0, 0, 0x3F, 0x18, 0xBB, 1, 0, 0xFF)  # Mf1 += 0.5

t0 = time.time()
h = Harness('plc')

ok = h.run(120_000, until=lambda: h.status() == 'CONNECT')
check(ok and 'pa1' in h.gw.topics.values(), f'connected, control object registered: {h.gw.topics}')
plc = Plc(h)
h.run(500)
first = plc.responses(0)
check(first[:1] == [bytes([0, 0])], f'PLC state published after connect: {first[:1]}')

# Upload the counter
r = plc.upload(COUNTER, stack_bottom=1)
check(r == bytes([2, 0]), f'upload + start: {r}')
crc_ok = plc.cmd([CRC_REQ, 0, 0, len(COUNTER), 0])
check(crc_ok == bytes([6, 0]) + crc16(COUNTER).to_bytes(2, 'little'), 'CRC after upload matches')
check(plc.upload(COUNTER, stack_bottom=1) == 'same', 'second upload skipped (CRC equal)')

# Merker Md0 = ram[0]
mark = h.ms
h.gw.register(50, 'Md0')
acks = [m for m in h.node_msgs(mark) if m[1] == g.REGACK]
check(acks and acks[-1][6] == 0, 'REGISTER Md0 accepted')
h.run(1000)
vals = [int.from_bytes(m[7:], 'little', signed=True) for t, d, m in h.log
        if d == 'node' and t >= mark and m[1] == g.PUBLISH and g.u16(m, 3) == 50]
check(len(vals) > 3 and vals == sorted(vals), f'counter published, {len(vals)} values: {vals[:5]}...')

# Gateway writes the merker
mark = h.ms
h.gw.publish(50, (100000).to_bytes(4, 'little'))
h.run(500)
vals = [int.from_bytes(m[7:], 'little', signed=True) for t, d, m in h.log
        if d == 'node' and t >= mark and m[1] == g.PUBLISH and g.u16(m, 3) == 50]
check(vals and vals[0] >= 100000, f'merker written by gateway: {vals[:3]}')

# Wrong merker
mark = h.ms
h.gw.register(51, 'Md9999')
acks = [m for m in h.node_msgs(mark) if m[1] == g.REGACK]
check(acks and acks[-1][6] == 3, 'merker outside RW area rejected')

# Write while running is refused
r = plc.cmd([WRITE_REQ, 0, 0, 0xFF] + list(crc16(b'\xff').to_bytes(2, 'little')))
check(r == bytes([8, 0xFA]), f'write while running -> wrong state: {r}')

# Error report
r = plc.upload(DIV0, stack_bottom=1)
check(r == bytes([2, 0]), 'div0 program started')
mark = h.ms
h.run(500, until=lambda: any(x[0] == 4 for x in plc.responses(mark)))
rep = [x for x in plc.responses(mark) if x[0] == 4]
check(rep and len(rep[0]) == 18 and rep[0][1] == 0x86,
      f'stop report: {rep[0].hex(" ") if rep else None}')
if rep:
    pc = int.from_bytes(rep[0][14:18], 'little')
    check(pc == 7, f'report PC = {pc} (after DIV at 6)')

# API call from the program: Md0 = NodeStatus() = 1
r = plc.upload(NODESTATUS, stack_bottom=1)
check(r == bytes([2, 0]), 'NodeStatus program started')
mark = h.ms
h.run(500)
vals = [int.from_bytes(m[7:], 'little', signed=True) for t, d, m in h.log
        if d == 'node' and t >= mark and m[1] == g.PUBLISH and g.u16(m, 3) == 50]
check(vals[-1:] == [1], f'Md0 = NodeStatus(): {vals}')

# Float merker Mf1 (ram[1]) += 0.5 each cycle, 4 bytes IEEE-754 LE
r = plc.upload(FLOATSTEP, stack_bottom=2)
check(r == bytes([2, 0]), 'float program started')
mark = h.ms
h.gw.register(52, 'Mf1')
acks = [m for m in h.node_msgs(mark) if m[1] == g.REGACK]
check(acks and acks[-1][6] == 0, 'REGISTER Mf1 accepted')
h.run(500)


def floats(topic, since):
    return [struct.unpack('<f', m[7:])[0] for t, d, m in h.log
            if d == 'node' and t >= since and m[1] == g.PUBLISH and g.u16(m, 3) == topic and len(m) == 11]


vals = floats(52, mark)
check(len(vals) > 3 and all(v * 2 == int(v * 2) for v in vals) and vals == sorted(vals),
      f'Mf1 published as float: {vals[:4]}...')
mark = h.ms
h.gw.publish(52, struct.pack('<f', 1000.25))
h.run(300)
vals = floats(52, mark)
after = [v for v in vals if v >= 1000.25]   # the first one may be in flight before the write
check(after and all((v - 1000.25) * 2 == int((v - 1000.25) * 2) for v in after),
      f'Mf1 written by gateway: {vals[:3]}')

# Persistence: program, stack bottom and merker registrations survive reboot
h.cmd('reboot')
mark = h.ms
h.run(120_000, until=lambda: h.status() == 'CONNECT')
h.run(1000)
tid = next((t for t, n in h.gw.topics.items() if n == 'Mf1' and t != 52), None)
vals = floats(tid, mark) if tid else []
check(tid is not None and len(vals) > 3, f'after reboot: Mf1 re-registered as {tid}, values {vals[:3]}...')

# IO: the init code configures GPIO2 as output, the cycle copies Md2 (ram[2]) to it
def program_init(init, cycle):
    at = 4 + len(cycle)                                     # Address 0 jumps to init after the cycle
    return bytes([0xF3, at & 0xFF, at >> 8, 0xFF]) + bytes(cycle) + bytes(init)


def hw():
    return [ln[3:] for ln in h.cmd('hw')]


def level(pin):
    return int(h.cmd(f'level {pin}')[0].split()[1])


OUT2_INIT = [0x38, 0x3C, 5, 0x3C, 2, 0xC6, 31, 0xFF]           # ConfigPin(2, OUT, 0)
OUT2_CYCLE = [0x9B, 2, 0, 0xC1, 0, 0, 2, 0, 0xFF]              # OUT 2 = Md2
IO_A = program_init(OUT2_INIT, OUT2_CYCLE)
IO_B = program_init(OUT2_INIT, [0x00] + OUT2_CYCLE)            # Same pin setup, other code
IO_NONE = program_init([0xFF], [0xFF])
IO_BAD = program_init([0x38, 0x3C, 5, 0x3C, 6, 0xC6, 31, 0xFF], [0xFF])   # ConfigPin(6, OUT, 0): flash pin

plc.tid = max(t for t, n in h.gw.topics.items() if n == 'pa1')     # Registered again after reboot
hw()
r = plc.upload(IO_A, stack_bottom=3)
check(r == bytes([2, 0]), f'IO program started: {r}')
h.run(100)
calls = hw()
check('gpio_output 2 0 0' in calls, f'init configured GPIO2: {calls}')
h.gw.register(60, 'Md2')
h.gw.publish(60, (1).to_bytes(4, 'little'))
h.run(100)
check(level(2) == 1, 'Md2 = 1 -> GPIO2 high')
hw()

r = plc.upload(IO_B, stack_bottom=3)
h.run(100)
calls = hw()
check(r == bytes([2, 0]) and not any(c.endswith(' 2') or ' 2 ' in c for c in calls) and level(2) == 1,
      f'restart with the same setup keeps GPIO2 untouched: {calls}')

r = plc.upload(IO_NONE, stack_bottom=0)
h.run(100)
calls = hw()
check(r == bytes([2, 0]) and 'release 2' in calls, f'pin not set up by the new program released: {calls}')

r = plc.upload(IO_BAD, stack_bottom=0)
mark = h.ms
h.run(500, until=lambda: any(x[0] == 4 for x in plc.responses(mark)))
rep = [x for x in plc.responses(mark) if x[0] == 4]
check(rep and rep[0][1] == 0x8A, f'ConfigPin on a flash pin stops with 0x8A: {rep[0].hex(" ") if rep else None}')

# Clock: NowSeconds() and Today() follow the gateway time, set at connect
CLOCK_PRG = program(0xC6, 9, 0xBB, 3, 0, 0xC6, 10, 0xBB, 4, 0, 0xFF)    # Md3 = NowSeconds(), Md4 = Today()
r = plc.upload(CLOCK_PRG, stack_bottom=5)
check(r == bytes([2, 0]), 'clock program started')
h.gw.register(71, 'Md3')
h.gw.register(72, 'Md4')
h.run(2000)


def last_value(topic):
    vals = [int.from_bytes(m[7:], 'little', signed=True) for t, d, m in h.log
            if d == 'node' and m[1] == g.PUBLISH and g.u16(m, 3) == topic]
    return vals[-1] if vals else None


now = hn.CLOCK + hn.timedelta(milliseconds=h.ms)
secs = now.hour * 3600 + now.minute * 60 + now.second
today = (now.year % 100) << 24 | now.month << 16 | now.day << 8 | now.isoweekday()
check(last_value(71) is not None and abs(last_value(71) - secs) <= 1, f'NowSeconds() = {last_value(71)}, gateway {secs}')
check(last_value(72) == today, f'Today() = {last_value(72)}, gateway {today:#x}')

# Streams to the node log: lines of the init cycle, Info goes although the firmware level is Warning. A 0 byte
# sends the line at once, the others go after the cycle
def send(func, stream, *args):          # Last argument first, the stream on top; drop the result
    code = []
    for a in reversed(args):
        code += [0x3B] + list(struct.pack('<I', a))
    return code + [0x3D, stream & 0xFF, stream >> 8, 0xC6, func, 0x03]


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


LOG_INIT = (send(24, 0xFFE1, 1234) + send(23, 0xFFE2, ord('T')) + send(23, 0xFFE2, ord('='))
            + send(25, 0xFFE2, 3, f32(21.456)) + send(23, 0xFFE2, 0) + send(24, 0xFFE2, -7 & 0xFFFFFFFF) + [0xFF])
LOG_PRG = program_init(LOG_INIT, [0xFF])
mark = h.ms
r = plc.upload(LOG_PRG, stack_bottom=0)
h.run(500)
logs = [(g.u16(m, 3), m[2], m[7:]) for m in h.node_msgs(mark) if m[1] == g.PUBLISH and 0xFFE0 <= g.u16(m, 3) <= 0xFFE3]
check(r == bytes([2, 0]) and logs == [(0xFFE2, 0x01, b'T=21.46'), (0xFFE1, 0x01, b'1234'), (0xFFE2, 0x01, b'-7')],
      f'program lines in the node log, once: {logs}')

# Stop on an error: what the cycle wrote still goes
LOG_DIV0 = program(*(send(23, 0xFFE3, ord('E')) + [0x3E, 0x38, 0x13, 0xFF]))      # 'E', then 1 / 0
mark = h.ms
r = plc.upload(LOG_DIV0, stack_bottom=0)
h.run(500, until=lambda: any(len(x) == 18 for x in plc.responses(mark)))
h.run(100)
rep = [x for x in plc.responses(mark) if len(x) == 18]     # Not the STOP of the upload
logs = [(g.u16(m, 3), m[7:]) for m in h.node_msgs(mark) if m[1] == g.PUBLISH and 0xFFE0 <= g.u16(m, 3) <= 0xFFE3]
check(rep and rep[0][1] == 0x86 and logs == [(0xFFE3, b'E')], f'line before the stop on 0x86: {logs}')

h.cmd('quit')
print(f'{"FAILED" if hn.failed else "ALL PASSED"} in {time.time() - t0:.1f} s real time')
sys.exit(1 if hn.failed else 0)
