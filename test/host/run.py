"""Build and run the host tests.

    python test/host/run.py [test ...]      tests: vm, io, clock, e2e, e2e_plc (default: all)

C compiler: $CC, default "gcc"; any gcc or clang, e.g. CC=clang or CC="zig cc".
"""
import os
import shlex
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMP = HERE.parents[1] / 'components'
BUILD = HERE / 'build'
EXE = '.exe' if os.name == 'nt' else ''

INCLUDE = [HERE / 'stubs', HERE] + [COMP / d for d in (
    'plc', 'plc/include', 'mqttsn', 'mqttsn/include', 'objdict', 'objdict/include',
    'serial_phy', 'serial_phy/include')]
FLAGS = ['-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']

PLC = [COMP / 'plc' / f for f in ('plc_vm.c', 'plc_api.c', 'plc_io.c')] + [COMP / 'objdict/od_clock.c']
BINARIES = {
    'test_vm': [HERE / 'test_vm.c', COMP / 'plc/plc_vm.c'],
    'test_io': [HERE / 'test_io.c', HERE / 'fake_hw.c'] + PLC,
    'test_clock': [HERE / 'test_clock.c', COMP / 'objdict/od_clock.c'],
    'host_node': [HERE / 'host_node.c', HERE / 'fake_hw.c', HERE / 'fake_store.c', COMP / 'plc/plc.c']
    + PLC + [COMP / f for f in ('mqttsn/mqttsn_client.c', 'mqttsn/mqttsn_msg.c', 'objdict/objdict.c',
                                'objdict/od_pack.c', 'objdict/od_storage.c', 'serial_phy/serial_frame.c')],
}
TESTS = {                               # name: (binary, command)
    'vm': ('test_vm', [str(BUILD / ('test_vm' + EXE))]),
    'io': ('test_io', [str(BUILD / ('test_io' + EXE))]),
    'clock': ('test_clock', [str(BUILD / ('test_clock' + EXE))]),
    'e2e': ('host_node', [sys.executable, str(HERE / 'e2e.py')]),
    'e2e_plc': ('host_node', [sys.executable, str(HERE / 'e2e_plc.py')]),
}


def build(name):
    cmd = shlex.split(os.environ.get('CC', 'gcc')) + FLAGS + [f'-I{d}' for d in INCLUDE]
    cmd += [str(src) for src in BINARIES[name]] + ['-o', str(BUILD / (name + EXE))]
    if os.name != 'nt':
        cmd.append('-lm')
    print(f'build {name}', flush=True)
    subprocess.run(cmd, check=True)


def main():
    names = sys.argv[1:] or list(TESTS)
    unknown = [n for n in names if n not in TESTS]
    if unknown:
        sys.exit(f'unknown test: {" ".join(unknown)}; tests: {" ".join(TESTS)}')

    BUILD.mkdir(exist_ok=True)
    for binary in sorted({TESTS[n][0] for n in names}):
        build(binary)

    failed = []
    for n in names:
        print(f'=== {n}', flush=True)
        if subprocess.run(TESTS[n][1], cwd=HERE).returncode != 0:
            failed.append(n)
    print(f'FAILED: {" ".join(failed)}' if failed else 'ALL TESTS PASSED')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
