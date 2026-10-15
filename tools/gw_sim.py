#!/usr/bin/env python3
"""MQTT-SN serial gateway simulator for EspNode (X13 serial framing).

Answers a node like the X13 gateway does: DHCP, SEARCHGW, CONNECT, REGISTER, PUBLISH,
SUBSCRIBE, PINGREQ, and lets you send commands to the node from the console.

    python tools/gw_sim.py COM5             # talk to a node, needs pyserial
    python tools/gw_sim.py --selftest       # check the framing and the gateway logic
"""
from __future__ import annotations

import argparse
import queue
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import Callable

# Framing: [Length] [message, 0xC0/0xDB escaped as 0xDB, b ^ 0x20] [0xC0]
END = 0xC0
ESC = 0xDB
ESC_XOR = 0x20
MAX_MSG_LEN = 50            # MQTTSN_MAX_MSG_LEN
RX_TIMEOUT = 0.1            # Max pause between bytes of one frame, s

# Message types
ADVERTISE, SEARCHGW, GWINFO = 0x00, 0x01, 0x02
CONNECT, CONNACK = 0x04, 0x05
REGISTER, REGACK = 0x0A, 0x0B
PUBLISH, PUBACK = 0x0C, 0x0D
SUBSCRIBE, SUBACK = 0x12, 0x13
PINGREQ, PINGRESP = 0x16, 0x17
DISCONNECT = 0x18
DHCPREQ, DHCPRESP = 0x43, 0x44

MSG_NAMES = {
    ADVERTISE: 'ADVERTISE', SEARCHGW: 'SEARCHGW', GWINFO: 'GWINFO', CONNECT: 'CONNECT',
    CONNACK: 'CONNACK', REGISTER: 'REGISTER', REGACK: 'REGACK', PUBLISH: 'PUBLISH',
    PUBACK: 'PUBACK', SUBSCRIBE: 'SUBSCRIBE', SUBACK: 'SUBACK', PINGREQ: 'PINGREQ',
    PINGRESP: 'PINGRESP', DISCONNECT: 'DISCONNECT', DHCPREQ: 'DHCPREQ', DHCPRESP: 'DHCPRESP',
}

FL_DUP, FL_QOS1, FL_RETAIN = 0x80, 0x20, 0x10
FL_QOS_MASK, FL_TOPICID_MASK = 0x60, 0x03
FL_TOPICID_NORM, FL_TOPICID_PREDEF = 0x00, 0x01

RC_NAMES = {0: 'accepted', 1: 'congestion', 2: 'invalid topic id', 3: 'not supported'}

# PLC control object "pa1" (X13: "pa0"): request [cmd] [args], response [cmd + 1] [status] [data]
PLC_TOPIC = 'pa1'
PLC_CMDS = {0: 'IDLE', 1: 'START_REQ', 2: 'START_RESP', 3: 'STOP_REQ', 4: 'STOP_RESP',
            5: 'CRC_REQ', 6: 'CRC_RESP', 7: 'WRITE_REQ', 8: 'WRITE_RESP',
            11: 'SET_STACK_REQ', 12: 'SET_STACK_RESP'}

PLC_BLOCK = 32              # Program bytes per WRITE_REQ, as Enviriot DevicePLC
PLC_TIMEOUT = 2.0           # Wait for a PLC response, s


def plc_crc16(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC of the PLC control protocol: X13 plc_calc_crc, polynomial 0x8005, MSB first."""
    for b in data:
        for _ in range(8):
            if ((crc & 0x8000) >> 8) ^ (b & 0x80):
                crc = ((crc << 1) ^ 0x8005) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
            b = (b << 1) & 0xFF
    return crc


# Predefined objects of the node, decoded as strings
PREDEF_STRINGS = {0xFF00: 'NodeName', 0xFFC0: 'DeviceType'}
PREDEF_NAMES = {0xFF07: 'RTC', 0xFF10: 'NodeId', 0xFF14: 'GateId', 0xFFC1: 'PHY1addr', **PREDEF_STRINGS}

# Node clock, X13 RTC object: sec, min, hour, day, weekday << 5 | month, year % 100, BCD
RTC_TOPIC = 0xFF07
RTC_MAX_DIFF = 2.0          # s, Enviriot MsDevice: larger difference -> set the node clock


def rtc_encode(t: datetime) -> bytes:
    def bcd(v: int) -> int:
        return ((v // 10) << 4) | (v % 10)
    return bytes([bcd(t.second), bcd(t.minute), bcd(t.hour), bcd(t.day),
                  (t.isoweekday() << 5) | bcd(t.month), bcd(t.year % 100)])


def rtc_decode(data: bytes, century: int) -> datetime | None:
    def dec(b: int) -> int:
        return (b >> 4) * 10 + (b & 0x0F)
    try:
        hour = dec(data[2] & 0x3F) + (12 if data[2] & 0x40 else 0)
        return datetime(century + dec(data[5]), dec(data[4] & 0x1F), dec(data[3] & 0x3F),
                        hour, dec(data[1] & 0x7F), dec(data[0] & 0x7F))
    except (ValueError, IndexError):
        return None


def encode_frame(msg: bytes) -> bytes:
    if not 2 <= len(msg) <= MAX_MSG_LEN:
        raise ValueError(f'message length {len(msg)} out of range')
    out = bytearray([len(msg)])
    for b in msg:
        if b in (END, ESC):
            out += bytes([ESC, b ^ ESC_XOR])
        else:
            out.append(b)
    out.append(END)
    return bytes(out)


class FrameDecoder:
    """Mirror of serial_frame_decode() in components/serial_phy/serial_frame.c."""

    def __init__(self) -> None:
        self.reset()
        self.last_rx = 0.0

    def reset(self) -> None:
        self.length = 0
        self.buf = bytearray()
        self.count = 0
        self.esc = False

    def feed(self, data: bytes, now: float | None = None) -> list[bytes]:
        now = time.monotonic() if now is None else now
        if self.length and now - self.last_rx > RX_TIMEOUT:
            self.reset()
        self.last_rx = now

        frames = []
        for b in data:
            if self.length == 0:
                if 2 <= b <= MAX_MSG_LEN:
                    self.length = b
                    self.buf = bytearray()
                    self.count = 0
                    self.esc = False
                continue
            if b == END:
                if self.count == self.length:
                    frames.append(bytes(self.buf))
                self.length = 0
                continue
            if b == ESC:
                self.esc = True
                continue
            if self.esc:
                self.esc = False
                b ^= ESC_XOR
            if self.count < self.length:
                self.buf.append(b)
            self.count += 1
        return frames


def u16(data: bytes, offs: int) -> int:
    return (data[offs] << 8) | data[offs + 1]


def p16(val: int) -> bytes:
    return bytes([(val >> 8) & 0xFF, val & 0xFF])


def msg(msg_type: int, body: bytes = b'') -> bytes:
    return bytes([2 + len(body), msg_type]) + body


def describe_value(topic_id: int, flags: int, data: bytes) -> str:
    predef = (flags & FL_TOPICID_MASK) == FL_TOPICID_PREDEF
    if predef and topic_id in PREDEF_STRINGS:
        return repr(data.decode('utf-8', errors='replace'))
    text = data.hex(' ') if data else '<empty>'
    if 0 < len(data) <= 8:
        text += f' = {int.from_bytes(data, "little", signed=True)}'
    return text


def topic_label(topic_id: int, flags: int) -> str:
    if (flags & FL_TOPICID_MASK) == FL_TOPICID_PREDEF:
        return f'0x{topic_id:04X} ({PREDEF_NAMES.get(topic_id, "predefined")})'
    return str(topic_id)


class Gateway:
    """Gateway side of the protocol; send() gets complete MQTT-SN messages."""

    def __init__(self, send: Callable[[bytes], None], node_addr: int = 0x10, gw_id: int = 1,
                 log: Callable[[str], None] = print, now: Callable[[], datetime] = datetime.now) -> None:
        self.send_raw = send
        self.now = now                          # Local time the node clock is set to
        self.node_addr = node_addr
        self.gw_id = gw_id
        self.log = log
        self.topics: dict[int, str] = {}        # Topic id -> name registered by the node
        self.next_topic_id = 1
        self.msg_id = 0
        self.client_id: str | None = None
        self.subscribed = False
        self.plc_rx: queue.Queue[bytes] = queue.Queue()     # PLC responses, filled by handle()
        self.rtc_set = False                    # Set the node clock when its session is done

    def _send(self, m: bytes) -> None:
        self.log(f'  -> {MSG_NAMES.get(m[1], hex(m[1]))} {m.hex(" ")}')
        self.send_raw(m)

    def _new_msg_id(self) -> int:
        self.msg_id = self.msg_id % 0xFFFE + 1
        return self.msg_id

    def handle(self, m: bytes) -> None:
        if len(m) < 2 or m[0] != len(m):
            self.log(f'<-  malformed {m.hex(" ")}')
            return
        t = m[1]
        self.log(f'<-  {MSG_NAMES.get(t, hex(t))} {m.hex(" ")}')

        if t == DHCPREQ:
            hlen = m[5:]
            addr = b''.join(self.node_addr.to_bytes(h, 'little') for h in hlen)
            self.log(f'    DHCP radius {m[2]}, assign 0x{self.node_addr:02X}')
            self._send(msg(DHCPRESP, bytes([self.gw_id]) + m[3:5] + addr))
        elif t == SEARCHGW:
            self.log(f'    radius {m[2]}')
            self._send(msg(GWINFO, bytes([self.gw_id])))
        elif t == CONNECT:
            self.client_id = m[6:].decode('utf-8', errors='replace')
            self.subscribed = False
            self.log(f'    client "{self.client_id}", keep alive {u16(m, 4)} s, flags 0x{m[2]:02X}')
            self._send(msg(CONNACK, b'\x00'))
        elif t == REGISTER:
            topic_id, msg_id, name = u16(m, 2), u16(m, 4), m[6:].decode('ascii', errors='replace')
            if topic_id == 0:
                topic_id = self.next_topic_id
                self.next_topic_id += 1
            self.topics[topic_id] = name
            self.log(f'    "{name}" -> topic {topic_id}')
            self._send(msg(REGACK, p16(topic_id) + p16(msg_id) + b'\x00'))
        elif t == PUBLISH:
            flags, topic_id, msg_id, data = m[2], u16(m, 3), u16(m, 5), m[7:]
            dup = ' DUP' if flags & FL_DUP else ''
            retain = ' RETAIN' if flags & FL_RETAIN else ''
            name = self.topics.get(topic_id, '')
            self.log(f'    {topic_label(topic_id, flags)} {name} QoS{(flags & FL_QOS_MASK) >> 5}'
                     f'{retain}{dup}: {describe_value(topic_id, flags, data)}')
            if name == PLC_TOPIC and len(data) >= 2:
                self.log(f'    PLC {PLC_CMDS.get(data[0], hex(data[0]))}, status 0x{data[1]:02X}')
                self.plc_rx.put(bytes(data))
            if (flags & FL_QOS_MASK) == FL_QOS1:
                self._send(msg(PUBACK, p16(topic_id) + p16(msg_id) + b'\x00'))
            if (flags & FL_TOPICID_MASK) == FL_TOPICID_PREDEF and topic_id == RTC_TOPIC and len(data) == 6:
                self._check_rtc(data)
        elif t == SUBSCRIBE:
            flags, msg_id, topic = m[2], u16(m, 3), m[5:].decode('utf-8', errors='replace')
            self.subscribed = True
            self.log(f'    "{topic}"')
            self._send(msg(SUBACK, bytes([flags & FL_QOS_MASK]) + p16(0) + p16(msg_id) + b'\x00'))
            if self.rtc_set:                    # The node takes PUBLISH from now on
                self.rtc_set = False
                now = self.now()
                self.log(f'    set node clock to {now:%Y-%m-%d %H:%M:%S}')
                self.publish(RTC_TOPIC, rtc_encode(now), predef=True)
        elif t == PINGREQ:
            self._send(msg(PINGRESP))
        elif t in (REGACK, PUBACK) and len(m) >= 7:
            self.log(f'    topic {u16(m, 2)}: {RC_NAMES.get(m[6], hex(m[6]))}')

    def _check_rtc(self, data: bytes) -> None:
        """As Enviriot MsDevice: the node publishes its clock at connect, a clock more than 2 s off is set
        later, from MsDevice.Tick (Enviriot also sets it every hour)."""
        now = self.now()
        node = rtc_decode(data, now.year // 100 * 100)
        self.log(f'    node clock {node}')
        if node is None or abs((node - now).total_seconds()) > RTC_MAX_DIFF:
            self.rtc_set = True

    # Commands to the node

    def publish(self, topic_id: int, data: bytes, predef: bool = False) -> None:
        flags = FL_QOS1 | (FL_TOPICID_PREDEF if predef else FL_TOPICID_NORM)
        self._send(msg(PUBLISH, bytes([flags]) + p16(topic_id) + p16(self._new_msg_id()) + data))

    def register(self, topic_id: int, name: str) -> None:
        self.topics[topic_id] = name
        self._send(msg(REGISTER, p16(topic_id) + p16(self._new_msg_id()) + name.encode('ascii')))

    def disconnect(self) -> None:
        self._send(msg(DISCONNECT))

    def plc(self, data: bytes) -> None:
        tids = [t for t, n in self.topics.items() if n == PLC_TOPIC]
        if not tids:
            raise ValueError(f'node has not registered "{PLC_TOPIC}" yet')
        self.publish(tids[-1], data)            # The node registers it again after a reconnect

    def plc_request(self, data: bytes, timeout: float = PLC_TIMEOUT) -> bytes | None:
        """Send a PLC control request and wait for its response [cmd + 1] [status] ..."""
        while not self.plc_rx.empty():
            self.plc_rx.get_nowait()
        self.plc(data)
        deadline = time.monotonic() + timeout
        while (left := deadline - time.monotonic()) > 0:
            try:
                resp = self.plc_rx.get(timeout=left)
            except queue.Empty:
                break
            if resp[0] == data[0] + 1:
                return resp
        return None

    def plc_upload(self, program: bytes, stack_bottom: int) -> str:
        """Upload like Enviriot DevicePLC: CRC check, STOP, WRITE blocks, SET_STACK, START."""
        def request(data: bytes, check: bool = True) -> bytes:
            name = PLC_CMDS[data[0]]
            resp = self.plc_request(data)
            if resp is None:
                raise ValueError(f'no response to {name}')
            if check and resp[1] != 0:
                raise ValueError(f'{name}: status 0x{resp[1]:02X}')
            return resp

        size = len(program)
        if not 0 < size <= 0xFFFF:
            raise ValueError(f'program size {size}')
        resp = request(bytes([5, 0, 0]) + size.to_bytes(2, 'little'))
        if resp[2:4] == plc_crc16(program).to_bytes(2, 'little'):
            return 'program unchanged, not uploaded'
        request(b'\x03', check=False)           # A pending error report has the same command
        for offs in range(0, size, PLC_BLOCK):
            block = program[offs:offs + PLC_BLOCK]
            request(bytes([7]) + offs.to_bytes(2, 'little') + block + plc_crc16(block).to_bytes(2, 'little'))
        request(bytes([11]) + stack_bottom.to_bytes(4, 'little'))
        request(b'\x01')
        return f'{size} bytes uploaded, stack bottom {stack_bottom}, started'


HELP = """commands:
  name <text>                  set node name (PUBLISH to 0xFF00)
  pub <topic> <hex> [predef]   publish raw data, topic is decimal or 0x..
  reg <topic> <name>           register object, e.g. "reg 50 Md0" (PLC merker)
  plc start|stop               start/stop the PLC program
  plc crc <addr> <len>         CRC of the program memory
  plc raw <hex>                raw PLC control request
  plc upload <file> [stack]    upload a program image and start it, stack - stack bottom in
                               words (data below it is protected from the stack), default 0
  disconnect                   send DISCONNECT
  quit"""


def run_command(gw: Gateway, line: str) -> bool:
    parts = line.split()
    if not parts:
        return True
    cmd, args = parts[0].lower(), parts[1:]
    try:
        if cmd == 'quit':
            return False
        if cmd == 'name':
            gw.publish(0xFF00, ' '.join(args).encode('utf-8'), predef=True)
        elif cmd == 'pub' and len(args) >= 2:
            gw.publish(int(args[0], 0), bytes.fromhex(args[1]), predef='predef' in args[2:])
        elif cmd == 'reg' and len(args) == 2:
            gw.register(int(args[0], 0), args[1])
        elif cmd == 'plc' and args[:1] == ['start']:
            gw.plc(b'\x01')
        elif cmd == 'plc' and args[:1] == ['stop']:
            gw.plc(b'\x03')
        elif cmd == 'plc' and args[:1] == ['crc'] and len(args) == 3:
            gw.plc(bytes([5]) + int(args[1], 0).to_bytes(2, 'little') + int(args[2], 0).to_bytes(2, 'little'))
        elif cmd == 'plc' and args[:1] == ['raw'] and len(args) == 2:
            gw.plc(bytes.fromhex(args[1]))
        elif cmd == 'plc' and args[:1] == ['upload'] and len(args) in (2, 3):
            stack = int(args[2], 0) if len(args) == 3 else 0
            print(gw.plc_upload(Path(args[1]).read_bytes(), stack))
        elif cmd == 'disconnect':
            gw.disconnect()
        else:
            print(HELP)
    except (ValueError, OSError) as err:
        print(f'error: {err}')
    return True


def serve(port: str, baud: int, node_addr: int, gw_id: int) -> None:
    try:
        import serial  # type: ignore
    except ImportError:
        sys.exit('pyserial is required: pip install pyserial')

    ser = serial.Serial(port, baud, timeout=0.05)
    lock = threading.Lock()

    def log(text: str) -> None:
        with lock:
            print(f'{time.strftime("%H:%M:%S")} {text}', flush=True)

    gw = Gateway(lambda m: ser.write(encode_frame(m)), node_addr, gw_id, log)
    dec = FrameDecoder()

    def reader() -> None:
        while True:
            data = ser.read(256)
            if data:
                for frame in dec.feed(data):
                    gw.handle(frame)

    threading.Thread(target=reader, daemon=True).start()
    log(f'gateway {gw_id} on {port} @ {baud}, DHCP address 0x{node_addr:02X}. "help" for commands')

    for line in sys.stdin:
        if not run_command(gw, line):
            break
    ser.close()


def selftest() -> None:
    # Framing round trip with escapes
    payload = bytes([7, PUBLISH, 0x21, 0xC0, 0xDB, 0x00, 0x01])
    frame = encode_frame(payload)
    assert frame == bytes([7, 7, PUBLISH, 0x21, 0xDB, 0xE0, 0xDB, 0xFB, 0x00, 0x01, END]), frame.hex()
    dec = FrameDecoder()
    assert dec.feed(frame, now=0.0) == [payload]

    # Noise before a frame, byte by byte delivery
    dec = FrameDecoder()
    out = []
    for i, b in enumerate(bytes([0xFF, 0x00, END]) + frame):
        out += dec.feed(bytes([b]), now=i * 0.001)
    assert out == [payload], out

    # Length mismatch and truncated frames are dropped, the next frame is received
    dec = FrameDecoder()
    assert dec.feed(bytes([5, 1, 2, END]) + frame, now=0.0) == [payload]
    assert dec.feed(bytes([5, 1, 2, 3, 4, 5, 6, END]) + frame, now=0.01) == [payload]

    # Inter-byte timeout drops a partial frame
    dec = FrameDecoder()
    assert dec.feed(frame[:4], now=0.0) == []
    assert dec.feed(frame[4:], now=0.5) == []
    assert dec.feed(frame, now=0.51) == [payload]

    # Gateway answers
    sent: list[bytes] = []
    gw = Gateway(sent.append, node_addr=0x10, gw_id=1, log=lambda _: None)
    gw.handle(bytes([6, DHCPREQ, 1, 0x12, 0x34, 1]))
    assert sent[-1] == bytes([6, DHCPRESP, 1, 0x12, 0x34, 0x10]), sent[-1].hex()
    gw.handle(msg(SEARCHGW, b'\x01'))
    assert sent[-1] == bytes([3, GWINFO, 1])
    gw.handle(msg(CONNECT, bytes([0x04, 0x01]) + p16(300) + b'E1S10_10'))
    assert sent[-1] == bytes([3, CONNACK, 0]) and gw.client_id == 'E1S10_10'
    gw.handle(msg(REGISTER, p16(0) + p16(7) + b'Oz12'))
    assert sent[-1] == msg(REGACK, p16(1) + p16(7) + b'\x00')
    gw.handle(msg(PUBLISH, bytes([0x31]) + p16(0xFFC0) + p16(8) + b'E1S10.400'))
    assert sent[-1] == msg(PUBACK, p16(0xFFC0) + p16(8) + b'\x00')
    gw.handle(msg(SUBSCRIBE, bytes([0x20]) + p16(9) + b'#'))
    assert sent[-1] == msg(SUBACK, bytes([0x20]) + p16(0) + p16(9) + b'\x00') and gw.subscribed
    gw.handle(msg(PINGREQ))
    assert sent[-1] == bytes([2, PINGRESP])

    # PLC control object registered by the node
    gw.handle(msg(REGISTER, p16(0) + p16(10) + b'pa1'))
    tid = u16(sent[-1], 2)
    run_command(gw, 'plc start')
    assert sent[-1][1] == PUBLISH and u16(sent[-1], 3) == tid and sent[-1][7:] == b'\x01'
    run_command(gw, 'plc crc 0 0x100')
    assert sent[-1][7:] == bytes([5, 0, 0, 0, 1])

    # Upload to a node that answers every PLC request with OK and keeps what was written
    mem = bytearray(256)
    reqs: list[int] = []

    def plc_node(m: bytes) -> None:
        sent.append(m)
        if m[1] != PUBLISH or u16(m, 3) != tid:
            return
        req = m[7:]
        reqs.append(req[0])
        resp = bytes([req[0] + 1, 0])
        if req[0] == 5:
            resp += plc_crc16(bytes(mem[:req[3] | req[4] << 8])).to_bytes(2, 'little')
        elif req[0] == 7:
            addr = req[1] | req[2] << 8
            mem[addr:addr + len(req) - 5] = req[3:-2]
        gw.handle(msg(PUBLISH, bytes([0]) + p16(tid) + p16(0) + resp))

    gw.send_raw = plc_node
    prg = bytes(range(40))
    assert gw.plc_upload(prg, 4).startswith('40 bytes') and mem[:40] == prg
    assert reqs == [5, 3, 7, 7, 11, 1], reqs
    assert gw.plc_upload(prg, 4).startswith('program unchanged')

    # Node clock: set when more than 2 s off
    clock = datetime(2026, 10, 15, 12, 34, 56)
    gw.now = lambda: clock
    assert rtc_encode(clock) == bytes([0x56, 0x34, 0x12, 0x15, (4 << 5) | 0x10, 0x26])
    sent.clear()
    gw.handle(msg(PUBLISH, bytes([0x30 | FL_TOPICID_PREDEF]) + p16(RTC_TOPIC) + p16(11)
                  + bytes([0x00, 0x00, 0x00, 0x01, (4 << 5) | 0x01, 0x70])))
    assert [m[1] for m in sent] == [PUBACK]
    gw.handle(msg(SUBSCRIBE, bytes([0x20]) + p16(12) + b'#'))
    assert sent[-1] == msg(PUBLISH, bytes([FL_QOS1 | FL_TOPICID_PREDEF]) + p16(RTC_TOPIC) + p16(gw.msg_id)
                           + rtc_encode(clock)), sent[-1].hex()
    sent.clear()
    gw.handle(msg(PUBLISH, bytes([0x30 | FL_TOPICID_PREDEF]) + p16(RTC_TOPIC) + p16(13) + rtc_encode(clock)))
    gw.handle(msg(SUBSCRIBE, bytes([0x20]) + p16(14) + b'#'))
    assert [m[1] for m in sent] == [PUBACK, SUBACK]

    print('selftest passed')


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port', nargs='?', help='serial port, e.g. COM5 or /dev/ttyUSB0')
    ap.add_argument('--baud', type=int, default=38400)
    ap.add_argument('--addr', type=lambda v: int(v, 0), default=0x10, help='address given by DHCP')
    ap.add_argument('--gw-id', type=lambda v: int(v, 0), default=1)
    ap.add_argument('--selftest', action='store_true', help='run self test and exit')
    args = ap.parse_args()

    if args.selftest:
        selftest()
    elif args.port:
        serve(args.port, args.baud, args.addr, args.gw_id)
    else:
        ap.error('port is required')


if __name__ == '__main__':
    main()
