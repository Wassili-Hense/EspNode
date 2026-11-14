"""End-to-end MQTT-SN over UDP, the WiFi of main.c: 4-byte addresses (IPv4), gateway search, name from the MAC."""
import sys
import time

import harness as hn
from harness import Harness, check, g

IP = bytes([192, 168, 1, 10])
IP2 = bytes([192, 168, 1, 11])
GW = bytes([192, 168, 1, 1])
OTHER = bytes([192, 168, 1, 99])
BROADCAST = b'\xff' * 4

t0 = time.time()


class UdpHarness(Harness):
    """The gateway hears what the node sends to its IP or to the broadcast and answers from GW."""

    def __init__(self, ip, gw=None):
        self.sent = []                  # (ms, dst, message) sent by the node
        super().__init__(log='e2e_udp')
        self.setup(ip, gw)

    def setup(self, ip, gw=None):
        """Reboot as a WiFi node with this IP; gw - the gateway of cfg, else searched."""
        self.cmd('cfg 0 ff E1W10.400')
        self.cmd(f'udp-phy {ip.hex()} A1B2C3' + (f' {gw.hex()}' if gw else ''))
        self.cmd('reboot')
        self.gw.client_id = None

    def process(self, lines):
        other = []
        for ln in lines:
            if ln.startswith('udp '):
                _, dst, data = ln.split()
                dst, m = bytes.fromhex(dst), bytes.fromhex(data)
                self.log.append((self.ms, 'node', m))
                self.sent.append((self.ms, dst, m))
                if self.gw_online and dst in (GW, BROADCAST):
                    self.gw.handle(m)
            else:
                other.append(ln)
        return other

    def to_node(self, m, src=GW):
        self.log.append((self.ms, 'gw', m))
        self.process(self.cmd(f'udprx {src.hex()} {m.hex()}'))

    def sent_since(self, mark):
        return [(d, m) for t, d, m in self.sent if t >= mark]


def published(sent, topic):
    return [m[7:] for _, m in sent if m[1] == g.PUBLISH and g.u16(m, 3) == topic]


# 1. Fresh WiFi node: SEARCHGW to the broadcast, then everything to the gateway that answered
h = UdpHarness(IP)
ok = h.run(60_000, until=lambda: h.status() == 'CONNECT')
check(ok, f'connected after {h.ms / 1000:.1f} s simulated')
sent = h.sent_since(0)
print('   node:', ' '.join(f'{g.MSG_NAMES.get(m[1], hex(m[1]))}>{".".join(map(str, d))}' for d, m in sent[:4]))
check(sent[0][0] == BROADCAST and sent[0][1][1] == g.SEARCHGW, 'SEARCHGW to the broadcast')
check(all(m[1] != g.DHCPREQ for _, m in sent), 'no X13 DHCP: the address is the IP')
check(all(d == GW for d, _ in sent[1:]), 'after GWINFO everything to the gateway')
check(h.gw.client_id == 'E1W10_A1B2C3', f'default name from the MAC: {h.gw.client_id!r}')
check(published(sent, 0xFFC0) == [b'E1W10.400'], f'device type {published(sent, 0xFFC0)}')
check(published(sent, 0xFFC1) == [IP], f'PHY1 address is the IP: {published(sent, 0xFFC1)}')
check(published(sent, 0xFFD9) == [b'\x07'], f'all UART ports free for the program: {published(sent, 0xFFD9)}')

# 2. A message from another host is ignored, the same from the gateway is taken
mark = h.ms
name = g.msg(g.PUBLISH, bytes([g.FL_QOS1 | g.FL_TOPICID_PREDEF]) + g.p16(0xFF00) + g.p16(77) + b'evil')
h.to_node(name, src=OTHER)
h.run(500)
name_acks = lambda: [m for _, m in h.sent_since(mark) if m[1] == g.PUBACK and g.u16(m, 2) == 0xFF00]
check(not name_acks(), 'PUBLISH from another host ignored')
h.to_node(name)
h.run(500)
check(len(name_acks()) == 1 and name_acks()[0][6] == 0, 'PUBLISH from the gateway accepted')
h.gw.publish(0xFF00, b'', predef=True)          # Back to the default name
h.run(100)

# 3. Gateway set in cfg: no search, CONNECT straight to it
h.setup(IP, GW)
mark = h.ms
ok = h.run(60_000, until=lambda: h.status() == 'CONNECT')
sent = h.sent_since(mark)
print('   node:', ' '.join(f'{g.MSG_NAMES.get(m[1], hex(m[1]))}>{".".join(map(str, d))}' for d, m in sent[:3]))
check(ok and sent[0] == (GW, sent[0][1]) and sent[0][1][1] == g.CONNECT and
      all(m[1] != g.SEARCHGW for _, m in sent), 'gateway of cfg: CONNECT without SEARCHGW')

# 4. Another IP after a reboot: PHY1 follows it, the name from the MAC stays
h.setup(IP2)
mark = h.ms
ok = h.run(60_000, until=lambda: h.status() == 'CONNECT')
sent = h.sent_since(mark)
check(ok and published(sent, 0xFFC1) == [IP2] and h.gw.client_id == 'E1W10_A1B2C3',
      f'new IP: PHY1 {published(sent, 0xFFC1)}, name {h.gw.client_id!r}')

# 5. Signal level 0xFFC8 (X13 objRSSI), QoS0: read at messages of the gateway; published after a ping, otherwise
#    (intensive exchange, no pings) only when it moved by 3 dB; the first one at once
def rssi_sent(mark):
    return [(t, m) for t, _, m in h.sent if t >= mark and m[1] == g.PUBLISH and g.u16(m, 3) == 0xFFC8]


def rssi_values(mark):
    return [int.from_bytes(m[7:], 'little', signed=True) for _, m in rssi_sent(mark)]


def pingresp(mark):
    return [t for t, d, m in h.log if d == 'gw' and m[1] == g.PINGRESP and t >= mark]


def from_gw():
    """A message of the gateway in the session: PUBLISH to 0xFF09, accepted and ignored; the node pings 2 s later"""
    h.gw.publish(0xFF09, b'', predef=True)


h.run(5_000)                    # The session settles, the ping after it
check(not rssi_sent(0), 'no level while the station reports none')
mark = h.ms
h.cmd('rssi -60')
h.run(5_000)
check(not rssi_sent(mark), 'no message of the gateway: not read')
from_gw()
h.run(500)
sent = rssi_sent(mark)
check(len(sent) == 1 and sent[0][1][2] == g.FL_TOPICID_PREDEF and sent[0][1][7:] == bytes([0xC4]),
      f'the first level QoS0 at once: {[(m[2], m[7:].hex()) for _, m in sent]}')
mark = h.ms
h.cmd('rssi -62')
from_gw()
h.run(1_000)
check(not rssi_sent(mark), f'moved by 2 dB: not published {rssi_values(mark)}')
h.run(2_000)
sent, pings = rssi_sent(mark), pingresp(mark)
check(rssi_values(mark) == [-62] and pings and sent[0][0] >= pings[0],
      f'after the ping anyway: {rssi_values(mark)}, PINGRESP at {[t - mark for t in pings]}, '
      f'published at {[t - mark for t, _ in sent]}')
mark = h.ms
h.cmd('rssi -70')
from_gw()
h.run(3_000)
check(rssi_values(mark) == [-70] and not pingresp(mark), f'moved by 8 dB: at once {rssi_values(mark)}')
mark = h.ms
h.cmd('rssi -71')
h.run(310_000, until=lambda: rssi_sent(mark))
sent, pings = rssi_sent(mark), pingresp(mark)
check(rssi_values(mark) == [-71] and pings and sent[0][0] >= pings[0] and sent[0][0] - mark > 250_000,
      f'idle: with the keep alive ping after {(sent[0][0] - mark) / 1000 if sent else -1:.0f} s {rssi_values(mark)}')
h.run(100)
mark = h.ms
h.cmd('rssi none')
from_gw()
h.run(3_000)
check(pingresp(mark) and not rssi_sent(mark), 'without a link nothing, also after the ping')

h.cmd('quit')
print(f'{"FAILED" if hn.failed else "ALL PASSED"} in {time.time() - t0:.1f} s real time')
sys.exit(1 if hn.failed else 0)
