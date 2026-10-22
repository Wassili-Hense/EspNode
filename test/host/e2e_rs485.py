"""End-to-end RS-485: a gateway node E1SM10 (serial to gw_sim, an RS-485 bus as PHY2) and two nodes E1M10
on the bus.

All three are host_node processes. A frame one of them puts on the bus reaches the others, each takes the
frames to its address and to 0 (rs485_frame). gw_sim answers FORWARD as Enviriot: the addresses of the
nodes behind from the device of the gateway node, a device per wireless node id [PHY, bus address].
"""
import sys
import time

import harness as hn
from harness import Harness, check, g

GW_ADDR = 0x10                          # Of the gateway node from gw_sim DHCP, also its bus address
UNDEF = 0xFF

t0 = time.time()


def parse(frame):
    """X13 RS-485 frame -> (dst, src, message)."""
    assert frame[:2] == b'\xf0\x1c' and frame[2] == len(frame) - 4, frame.hex()
    assert sum(frame[2:-1]) & 0xFF == frame[-1], f'CRC: {frame.hex()}'
    return frame[3], frame[4], bytes(frame[5:-1])


class BusHarness(Harness):
    def __init__(self):
        super().__init__(log='e2e_rs485')
        self.nodes = [hn.Node(f'e2e_rs485_node{i}') for i in (1, 2)]
        for i, n in enumerate(self.nodes, 1):
            for c in ('rs485-phy 1', 'cfg 1 ff E1M10.400', f'seed {1000 * i}', 'reboot'):
                n.cmd(c)
        self.bus = []                   # (ms, who, dst, src, message): who - 0 the gateway node, 1.. the nodes
        for c in ('gw-phy 2', 'cfg 1 ff E1SM10.400', 'reboot'):
            self.cmd(c)

    def put(self, who, lines):
        """The frames of participant who reach all the others; returns the other lines."""
        other = []
        for ln in lines:
            if not ln.startswith('bus '):
                other.append(ln)
                continue
            frame = bytes.fromhex(ln[4:])
            self.bus.append((self.ms, who) + parse(frame))
            outs = [(i, n.cmd('busrx ' + frame.hex())) for i, n in enumerate(self.nodes, 1) if i != who]
            if who != 0:
                outs.append((0, self.cmd('busrx ' + frame.hex())))
            for i, out in outs:
                if i == 0:
                    self.process(out)
                else:
                    self.put(i, out)
        return other

    def process(self, lines):
        return super().process(self.put(0, lines))

    def run(self, max_ms, until=None):
        end = self.ms + max_ms
        while self.ms < end:
            self.ms += hn.TICK_MS
            self.process(self.cmd('tick 1'))
            for i, n in enumerate(self.nodes, 1):
                self.put(i, n.cmd('tick 1'))
            if until and until():
                return True
        return until is None

    def frames(self, msg_type, since=0, who=None):
        return [(w, dst, src, m) for _, w, dst, src, m in self.bus[since:]
                if m[1] == msg_type and (who is None or (w == who if who != 'node' else w > 0))]


h = BusHarness()

# 1. The gateway node gets its address and connects; it is silent on the bus until then
ok = h.run(120_000, until=lambda: h.status() == 'CONNECT')
t_gw = h.ms
check(ok and h.gw.client_id == 'E1SM10_10', f'gateway node connected as {h.gw.client_id!r} after {t_gw / 1000:.1f} s')

# 2. Both nodes on the bus get addresses through it and connect
ok = h.run(300_000, until=lambda: all(n.status() == 'CONNECT' for n in h.nodes))
check(ok, f'both nodes on the bus connected after {h.ms / 1000:.1f} s')
check(all(t >= t_gw for t, w, *_ in h.bus if w == 0), 'nothing from the gateway node on the bus before it connected')
names = sorted(c.client_id for c in h.gw.children.values())
check(names == ['E1M10_21', 'E1M10_22'], f'different addresses from DHCP: {names}')
check(sorted(h.gw.children) == [b'\x02\x21', b'\x02\x22'],
      f'gw_sim sees devices {[k.hex() for k in h.gw.children]}')
dhcp = h.frames(g.DHCPRESP, who=0)
check(dhcp and all(dst == UNDEF and src == GW_ADDR for _, dst, src, _ in dhcp),
      f'DHCPRESP to the nodes without an address (0xFF): {len(dhcp)}')
gwinfo = h.frames(g.GWINFO, who=0)
check(gwinfo and all(dst == 0 and src == GW_ADDR and m == bytes([3, g.GWINFO, 1]) for _, dst, src, m in gwinfo),
      f'GWINFO to all, from the bus address of the gateway node 0x{GW_ADDR:02X}: {len(gwinfo)}')
connect = {w: (dst, src) for w, dst, src, _ in h.frames(g.CONNECT, who='node')}
check(sorted(connect.values()) == [(GW_ADDR, 0x21), (GW_ADDR, 0x22)],
      f'CONNECT to the gateway node from the own addresses: {connect}')

# 3. PUBLISH to one node: the other one does not take it
addr = {w: src for w, (_, src) in connect.items()}
mark = len(h.bus)
h.gw.children[b'\x02\x21'].publish(0xFF00, b'bus', predef=True)
h.run(500)
pub = [(w, dst, src) for w, dst, src, _ in h.frames(g.PUBLISH, mark, who=0)]
acks = [(w, dst, src) for w, dst, src, m in h.frames(g.PUBACK, mark) if g.u16(m, 2) == 0xFF00]
node21 = [w for w, a in addr.items() if a == 0x21]
check(pub == [(0, 0x21, GW_ADDR)] and acks == [(node21[0], GW_ADDR, 0x21)],
      f'PUBLISH to node 0x21 only, its PUBACK comes back: {pub} {acks}')

# 4. ADVERTISE of the gateway reaches every node on the bus
mark = len(h.bus)
h.to_node(g.msg(g.ADVERTISE, bytes([1]) + g.p16(900)))
adv = [(w, dst) for w, dst, _, _ in h.frames(g.ADVERTISE, mark)]
check(adv == [(0, 0)], f'ADVERTISE passed on to all (DST 0): {adv}')

for n in h.nodes:
    n.cmd('quit')
h.cmd('quit')
print(f'{"FAILED" if hn.failed else "ALL PASSED"} in {time.time() - t0:.1f} s real time')
sys.exit(1 if hn.failed else 0)
