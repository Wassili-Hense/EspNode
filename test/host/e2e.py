"""End-to-end MQTT-SN scenarios."""
import sys
import time

import harness as hn
from harness import Harness, check, g

t0 = time.time()

# 0. Enviriot behaviour: DHCP_ACK, then ADVERTISE (seen on real hardware), before/after node restart
for queue in ('before_tick', 'after_tick'):
    h = Harness()
    orig_handle = h.gw.handle
    def enviriot_handle(m, h=h, orig_handle=orig_handle, queue=queue):
        orig_handle(m)
        if m[1] == g.DHCPREQ:
            getattr(h, queue).append(g.msg(g.ADVERTISE, bytes([1]) + g.p16(900)))
    h.gw.handle = enviriot_handle
    ok = h.run(60_000, until=lambda: h.status() == 'CONNECT')
    connects = h.types().count('CONNECT')
    check(ok and connects == 1 and (queue == 'before_tick' or 'SEARCHGW' not in h.types()),
          f'ADVERTISE {queue}: {connects} CONNECT, {h.ms / 1000:.1f} s: {" ".join(h.types())}')
    h.cmd('quit')

h = Harness()

# 1. Fresh node: DHCP -> SEARCHGW -> CONNECT -> device info -> SUBSCRIBE -> CONNECT
ok = h.run(120_000, until=lambda: h.status() == 'CONNECT')
check(ok, f'connected after {h.ms / 1000:.1f} s simulated')
seq = h.types()
print('   node:', ' '.join(seq))
check(seq[:11] == ['DHCPREQ', 'SEARCHGW', 'CONNECT', 'REGISTER'] + ['PUBLISH'] * 5 + ['SUBSCRIBE', 'PUBACK'],
      'message sequence')
check(h.gw.client_id == 'E1S10_10', f'client id {h.gw.client_id!r}')
pubs = [m for m in h.node_msgs() if m[1] == g.PUBLISH]
check(pubs[0][2] == 0x31 and g.u16(pubs[0], 3) == 0xFF07 and len(pubs[0]) == 13 and
      pubs[0][10:13] == b'\x01\x81\x70',     # Day 1, Thursday | January, 70
      f'clock published first, not set yet: 1970-01-01: {pubs[0][7:].hex(" ")}')
check(pubs[1][2] == 0x31 and g.u16(pubs[1], 3) == 0xFFC0 and pubs[1][7:] == b'E1S10.400',
      'device type published QoS1|RETAIN|PREDEF')
check(g.u16(pubs[2], 3) == 0xFFC1 and pubs[2][7:] == b'\x10', 'PHY1 address published')
# host_node reserves GPIO1, 3, flash 6-11 and UART1; there are no GPIO20, 24, 28-31
check(pubs[3][2] == 0x31 and g.u16(pubs[3], 3) == 0xFFD8 and pubs[3][7:] == bytes([0x35, 0xF0, 0xEF, 0x0E, 0xFF]),
      f'free pins published after a gap in the ids: {pubs[3][7:].hex(" ")}')
check(pubs[4][2] == 0x31 and g.u16(pubs[4], 3) == 0xFFD9 and pubs[4][7:] == b'\x04',
      f'free UART ports: UART2: {pubs[4][7:].hex(" ")}')
clock_set = [m for m in h.node_msgs() if m[1] == g.PUBACK and g.u16(m, 2) == 0xFF07]
check(clock_set and clock_set[0][6] == 0, 'clock set by the gateway after the session')
dhcp = h.node_msgs()[0]
check(dhcp == bytes([6, g.DHCPREQ, 1]) + dhcp[3:5] + b'\x01', f'DHCPREQ format {dhcp.hex(" ")}')

# 2. Set node name from the gateway
mark = h.ms
h.gw.publish(0xFF00, b'kitchen', predef=True)
acks = [m for m in h.node_msgs(mark) if m[1] == g.PUBACK and g.u16(m, 2) == 0xFF00]
check(len(acks) == 1 and acks[0][6] == 0, 'name accepted (PUBACK rc 0)')

# Duplicate (DUP + same msg id) is acknowledged but not applied twice
h.gw.publish(0x1234, b'\x01')
acks = [m for m in h.node_msgs(mark) if m[1] == g.PUBACK]
check(acks[-1][6] == 2, 'publish to unknown topic -> invalid topic id')

# 3. REGISTER of an object without provider -> not supported
mark = h.ms
h.gw.register(5, 'Oz12')
acks = [m for m in h.node_msgs(mark) if m[1] == g.REGACK]
check(len(acks) == 1 and acks[0][6] == 3, 'REGISTER Oz12 -> not supported')
h.gw.register(6, 'Oz1x')
acks = [m for m in h.node_msgs(mark) if m[1] == g.REGACK]
check(acks[-1][6] == 3, 'REGISTER with bad name -> not supported')

# 4. Keep alive: PINGREQ within ~300 s, answered
mark = h.ms
h.run(310_000, until=lambda: 'PINGREQ' in h.types(mark))
pings = [t for t, d, m in h.log if d == 'node' and m[1] == g.PINGREQ and t >= mark]
check(len(pings) >= 1, f'PINGREQ after {(pings[0] - mark) / 1000 if pings else -1:.1f} s')
h.run(1000)
check(h.status() == 'CONNECT', 'still connected after PINGRESP')

# 5. Gateway goes silent -> connection lost -> searching
h.gw_online = False
mark = h.ms
ok = h.run(1_500_000, until=lambda: h.status() in ('SEARCHGW', 'DISCONNECTED'))
check(ok, f'connection loss detected after {(h.ms - mark) / 1000:.0f} s')
print('   node while silent:', ' '.join(h.types(mark)))

# 6. Gateway back -> reconnect with the stored name, no DHCP
h.gw_online = True
mark = h.ms
ok = h.run(300_000, until=lambda: h.status() == 'CONNECT')
check(ok, f'reconnected after {(h.ms - mark) / 1000:.1f} s')
check('DHCPREQ' not in h.types(mark), 'no DHCP, address kept')
check(h.gw.client_id == 'kitchen', f'client id after reconnect {h.gw.client_id!r}')

# 7. Reboot: settings survive
h.cmd('reboot')
h.gw.client_id = None
mark = h.ms
ok = h.run(120_000, until=lambda: h.status() == 'CONNECT')
check(ok and h.gw.client_id == 'kitchen' and 'DHCPREQ' not in h.types(mark),
      f'after reboot: client id {h.gw.client_id!r}, no DHCP')

# 8. DISCONNECT from gateway -> reconnect
mark = h.ms
h.gw.disconnect()
check(h.status() == 'DISCONNECTED', 'DISCONNECT handled')
ok = h.run(120_000, until=lambda: h.status() == 'CONNECT')
check(ok, f'reconnected after DISCONNECT in {(h.ms - mark) / 1000:.1f} s')

# 9. Empty name -> default client id again
h.gw.publish(0xFF00, b'', predef=True)
h.gw.disconnect()
h.run(120_000, until=lambda: h.status() == 'CONNECT')
check(h.gw.client_id == 'E1S10_10', f'empty name -> default client id {h.gw.client_id!r}')


# 10. cfg partition: a new generation replaces the name and address written over the network, the
#     objects stay; the PHY letters of the device type follow cfg and keep them too
def reboot():
    h.cmd('reboot')
    h.gw.client_id = None
    mark = h.ms
    return h.run(120_000, until=lambda: h.status() == 'CONNECT'), mark


def node_published(topic, since):
    pubs = [m for m in h.node_msgs(since) if m[1] == g.PUBLISH and g.u16(m, 3) == topic]
    return pubs[-1][7:] if pubs else None


def node_registered(since):
    return [m[6:].decode() for m in h.node_msgs(since) if m[1] == g.REGISTER]


mark = h.ms
h.gw.register(20, 'Md0')
acks = [m for m in h.node_msgs(mark) if m[1] == g.REGACK]
check(acks and acks[-1][6] == 0, 'REGISTER Md0 accepted')
h.gw.publish(0xFF00, b'kitchen', predef=True)

h.cmd('cfg 1000 20 E1S10.400 hall')
ok, mark = reboot()
check(ok and h.gw.client_id == 'hall' and 'DHCPREQ' not in h.types(mark) and
      node_published(0xFFC1, mark) == b'\x20',
      f'new cfg: client id {h.gw.client_id!r}, address {node_published(0xFFC1, mark)}')
check('Md0' in node_registered(mark), f'new cfg: objects kept {node_registered(mark)}')

h.gw.publish(0xFF00, b'kitchen', predef=True)
ok, mark = reboot()
check(ok and h.gw.client_id == 'kitchen', f'same cfg: name from the gateway kept {h.gw.client_id!r}')

h.cmd('cfg 1001 ff E1W10.400')
ok, mark = reboot()
check(ok and h.gw.client_id == 'E1W10_10' and 'DHCPREQ' in h.types(mark) and
      node_published(0xFFC0, mark) == b'E1W10.400',
      f'new template: client id {h.gw.client_id!r}, {" ".join(h.types(mark)[:3])}')
check('Md0' in node_registered(mark), f'new template: objects kept {node_registered(mark)}')

h.gw.publish(0xFF00, b'porch', predef=True)
h.cmd('cfg 0 ff E1S10.400')
ok, mark = reboot()
check(ok and h.gw.client_id == 'porch' and 'DHCPREQ' not in h.types(mark),
      f'no cfg: saved values kept, client id {h.gw.client_id!r}')

# Node log: lines of the start wait for the session, then PUBLISH QoS0 to 0xFFE0 + level; Info, MQTT-SN out
h.cmd('reboot')
for ln in ('W (12) espnode: cfg: not written, using defaults', 'I (13) espnode: cfg: generation 1',
           'W (14) mqttsn: tx PUBLISH failed', 'E (15) plc: program storage not available'):
    h.cmd('log ' + ln)
mark = h.ms
ok = h.run(120_000, until=lambda: h.status() == 'CONNECT')
h.run(1000)
logs = [(g.u16(m, 3), m[2], m[7:]) for m in h.node_msgs(mark) if m[1] == g.PUBLISH and 0xFFE0 <= g.u16(m, 3) <= 0xFFE3]
check(ok and logs == [(0xFFE2, 0x01, b'espnode: cfg: not written, using defaults'),
                      (0xFFE3, 0x01, b'plc: program storage not available')], f'node log after the session: {logs}')
msgs = h.node_msgs(mark)
first_log = min(i for i, m in enumerate(msgs) if m[1] == g.PUBLISH and g.u16(m, 3) >= 0xFFE0)
check(first_log > [m[1] for m in msgs].index(g.SUBSCRIBE), 'the log only in the session, after SUBSCRIBE')

h.cmd('quit')
print(f'{"FAILED" if hn.failed else "ALL PASSED"} in {time.time() - t0:.1f} s real time')
sys.exit(1 if hn.failed else 0)
