"""Host harness: host_node (real C code, simulated time) <-> gw_sim.Gateway. Built by run.py."""
import os
import subprocess
import sys
from datetime import datetime, timedelta
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'tools'))
import gw_sim as g  # noqa: E402,F401

BUILD = HERE / 'build'
NODE = BUILD / ('host_node.exe' if os.name == 'nt' else 'host_node')
TICK_MS = 10
CLOCK = datetime(2026, 10, 15, 12, 0, 0)    # Gateway local time at simulated 0 ms


class Harness:
    def __init__(self, log='e2e'):
        self.err = open(BUILD / f'node_stderr_{log}.log', 'w')
        self.p = subprocess.Popen([str(NODE)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=self.err, text=True, bufsize=1)
        self.dec = g.FrameDecoder()
        self.ms = 0
        self.log = []                   # (ms, 'node'|'gw', msg)
        self.gw_online = True
        self.gw = g.Gateway(self.to_node, node_addr=0x10, gw_id=1, log=lambda s: None,
                            now=lambda: CLOCK + timedelta(milliseconds=self.ms))
        self.after_tick = []            # Messages to the node after the next tick
        self.before_tick = []           # Messages to the node before the next tick

    def cmd(self, line):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()
        out = []
        while True:
            ln = self.p.stdout.readline()
            if not ln:
                if line == 'quit':
                    return out
                raise EOFError(line)
            ln = ln.strip()
            if ln == 'ok':
                return out
            out.append(ln)

    def process(self, lines):
        other = []
        for ln in lines:
            if ln.startswith('tx '):
                for m in self.dec.feed(bytes.fromhex(ln[3:]), now=0.0):
                    self.log.append((self.ms, 'node', m))
                    if self.gw_online:
                        self.gw.handle(m)
            else:
                other.append(ln)
        return other

    def to_node(self, m):
        self.log.append((self.ms, 'gw', m))
        self.process(self.cmd('rx ' + g.encode_frame(m).hex()))

    def status(self):
        return self.cmd('status')[0].split()[1]

    def run(self, max_ms, until=None):
        end = self.ms + max_ms
        while self.ms < end:
            self.ms += TICK_MS
            pending, self.after_tick = self.after_tick, []
            for m in self.before_tick:
                self.to_node(m)
            self.before_tick = []
            self.process(self.cmd('tick 1'))
            for m in pending:
                self.to_node(m)
            if until and until():
                return True
        return until is None

    def node_msgs(self, since=0):
        return [m for t, d, m in self.log if d == 'node' and t >= since]

    def types(self, since=0, who='node'):
        return [g.MSG_NAMES.get(m[1], hex(m[1])) for t, d, m in self.log if d == who and t >= since]



failed = False


def check(cond, text):
    global failed
    print(('PASS ' if cond else 'FAIL ') + text)
    if not cond:
        failed = True
