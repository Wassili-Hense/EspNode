"""End-to-end gateway: an X13 node on a serial link behind this node (PHY2), its messages in FORWARD.

The node behind is another host_node (E1S10), frames are passed between "phy2" of the gateway node and
"rx" of the node behind; gw_sim answers FORWARD as Enviriot, with a device per wireless node id.
"""
import subprocess
import sys
import time

import harness as hn
from harness import BUILD, NODE, Harness, check, g

NODE_ID = b'\x02\x01'                   # PHY2, the only node on a point-to-point link

t0 = time.time()


class Remote:
    """The node behind the gateway node."""

    def __init__(self):
        self.err = open(BUILD / 'node_stderr_e2e_gw_remote.log', 'w')
        self.p = subprocess.Popen([str(NODE)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=self.err, text=True, bufsize=1)

    def cmd(self, line):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()
        out = []
        while True:
            ln = self.p.stdout.readline()
            if not ln:
                return out
            ln = ln.strip()
            if ln == 'ok':
                return out
            out.append(ln)

    def status(self):
        return self.cmd('status')[0].split()[1]


class GwHarness(Harness):
    def __init__(self):
        super().__init__(log='e2e_gw')
        self.remote = Remote()
        self.to_remote = []             # (ms, message) gateway node -> node behind
        self.from_remote = []           # (ms, message) node behind -> gateway node
        self.dec2 = g.FrameDecoder()
        self.cmd('gw-phy 1')
        self.cmd('reboot')

    def process(self, lines):
        other = []
        for ln in lines:
            if ln.startswith('phy2 '):
                frame = bytes.fromhex(ln[5:])
                for m in self.dec2.feed(frame, now=0.0):
                    self.to_remote.append((self.ms, m))
                self.remote_lines(self.remote.cmd('rx ' + frame.hex()))
            else:
                other.append(ln)
        return super().process(other)

    def remote_lines(self, lines):
        for ln in lines:
            if ln.startswith('tx '):
                frame = bytes.fromhex(ln[3:])
                self.from_remote.append((self.ms, g.FrameDecoder().feed(frame, now=0.0)[0]))
                self.process(self.cmd('phy2rx ' + frame.hex()))

    def run(self, max_ms, until=None):
        end = self.ms + max_ms
        while self.ms < end:
            self.ms += hn.TICK_MS
            self.process(self.cmd('tick 1'))
            self.remote_lines(self.remote.cmd('tick 1'))
            if until and until():
                return True
        return until is None

    def child(self):
        return self.gw.children.get(NODE_ID)


def types(entries, since=0):
    return [g.MSG_NAMES.get(m[1], hex(m[1])) for t, m in entries if t >= since]


h = GwHarness()

# 1. The gateway node connects first; until then nothing reaches the node behind
ok = h.run(120_000, until=lambda: h.status() == 'CONNECT')
t_gw = h.ms
check(ok and h.gw.client_id == 'E1S10_10', f'gateway node connected as {h.gw.client_id!r} after {t_gw / 1000:.1f} s')
ok = h.run(180_000, until=lambda: h.remote.status() == 'CONNECT')
check(ok, f'node behind connected after {h.ms / 1000:.1f} s')
check(all(t >= t_gw for t, _ in h.to_remote), 'nothing for the node behind before the gateway node connected')
print('   to node behind:', ' '.join(types(h.to_remote)[:8]))
check('DHCPRESP' in types(h.to_remote) and 'GWINFO' in types(h.to_remote), 'DHCP through the gateway, GWINFO from it')
gwinfo = [m for _, m in h.to_remote if m[1] == g.GWINFO]
check(gwinfo[0] == bytes([3, g.GWINFO, 1]), f'GWINFO with the id of the own gateway: {gwinfo[0].hex(" ")}')
child = h.child()
check(child is not None and child.client_id == 'E1S10_21' and list(h.gw.children) == [NODE_ID],
      f'gw_sim sees the node behind as device {NODE_ID.hex()}: {child and child.client_id!r}')
check('pa1' in child.topics.values() and h.gw.client_id == 'E1S10_10', 'its session runs, the own one stays')

# 2. PUBLISH to the node behind and its answer
mark = h.ms
child.publish(0xFF00, b'far', predef=True)
h.run(500)
acks = [m for t, m in h.from_remote if t >= mark and m[1] == g.PUBACK and g.u16(m, 2) == 0xFF00]
check(len(acks) == 1 and acks[0][6] == 0, 'PUBLISH reaches the node behind, PUBACK comes back')

# 3. ADVERTISE of the gateway is passed on
mark = h.ms
h.to_node(g.msg(g.ADVERTISE, bytes([1]) + g.p16(900)))
check(types(h.to_remote, mark) == ['ADVERTISE'], f'ADVERTISE passed on: {types(h.to_remote, mark)}')

# 4. FORWARD for another PHY is dropped; a 50-byte message passes whole (no time passes: mark by count)
n = len(h.to_remote)
h.to_node(g.forward(b'\x03\x01', g.msg(g.PINGRESP)))
check(len(h.to_remote) == n, 'FORWARD for PHY3 dropped')
big = g.msg(g.PUBLISH, bytes([0x00]) + g.p16(0x1234) + g.p16(0) + bytes(range(43)))
h.to_node(g.forward(NODE_ID, big))
got = [m for _, m in h.to_remote[n:]]
check(len(big) == 50 and got == [big], f'50-byte message passes whole: {[len(m) for m in got]}')

# 5. Without the own session the node behind is not served
h.gw.disconnect()
mark = h.ms
out = h.cmd('phy2rx ' + g.encode_frame(g.msg(g.PINGREQ)).hex())
check(h.status() == 'DISCONNECTED' and not any(ln.startswith('tx ') for ln in out),
      'gateway node disconnected: PINGREQ of the node behind not forwarded')

h.remote.cmd('quit')
h.cmd('quit')
print(f'{"FAILED" if hn.failed else "ALL PASSED"} in {time.time() - t0:.1f} s real time')
sys.exit(1 if hn.failed else 0)
