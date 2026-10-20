#!/usr/bin/env python3
"""Build a cfg partition image for EspNode (docs/cfg-format.md) until the installer page exists.

Interfaces go in the order given: the first is the client link to the gateway, the following are
gateways for X13 nodes (FORWARD).

    python tools/mkcfg.py --name kitchen --uart 1,17,16,-,38400            # UART1, no DE, SLIP
    python tools/mkcfg.py --wifi home,secret --uart 2,17,16,4,200000,rs485 -o wifi485.bin
    python tools/mkcfg.py --selftest                                        # example of the document
    python -m esptool --port COM3 write-flash 0x1a0000 cfg.bin             # write it to the node
"""
from __future__ import annotations

import argparse
import ipaddress
import struct
import sys
import time
import zlib
from pathlib import Path

PARTITION_OFFSET = 0x1A0000
PARTITION_SIZE = 0x1000
HDR_SIZE = 16
VERSION = 1

NODE_NAME, NODE_ADDR = 0x01, 0x02
IF_UART, IF_WIFI = 0x10, 0x20
WIFI_SSID, WIFI_PASSWORD, WIFI_IP, MQTTSN_GATEWAY = 0x21, 0x22, 0x23, 0x24
FRAMING = {'slip': 0, 'rs485': 1}
NO_PIN = 0xFF


def record(rtype: int, value: bytes = b'') -> bytes:
    if len(value) > 255:
        raise ValueError(f'record 0x{rtype:02X} too long')
    return bytes([rtype, len(value)]) + value


def build(records: bytes, generation: int) -> bytes:
    """Header + records, FF up to the end of the partition."""
    if len(records) > PARTITION_SIZE - HDR_SIZE:
        raise ValueError(f'records take {len(records)} bytes, max {PARTITION_SIZE - HDR_SIZE}')
    head = b'ECFG' + struct.pack('<BBHI', VERSION, 0, len(records), generation)
    crc = zlib.crc32(head + records)
    image = head + struct.pack('<I', crc) + records
    return image + b'\xFF' * (PARTITION_SIZE - len(image))


def uart(value: str) -> tuple:
    """port,tx,rx,de,baud[,slip|rs485]; de '-' - none"""
    parts = value.split(',')
    if len(parts) not in (5, 6):
        raise argparse.ArgumentTypeError('expected port,tx,rx,de,baud[,slip|rs485]')
    port, tx, rx = (int(p, 0) for p in parts[:3])
    de = NO_PIN if parts[3] in ('-', '-1', '') else int(parts[3], 0)
    baud = int(parts[4], 0)
    framing = parts[5].lower() if len(parts) == 6 else 'slip'
    if framing not in FRAMING:
        raise argparse.ArgumentTypeError(f'framing: {" or ".join(FRAMING)}')
    return ('uart', record(IF_UART, struct.pack('<BBBBIB', port, tx, rx, de, baud, FRAMING[framing])))


def wifi(value: str) -> tuple:
    """ssid[,password]"""
    ssid, _, password = value.partition(',')
    if not 1 <= len(ssid.encode()) <= 32 or len(password.encode()) > 64:
        raise argparse.ArgumentTypeError('SSID 1-32 bytes, password up to 64')
    return ('wifi', record(IF_WIFI) + record(WIFI_SSID, ssid.encode()) + record(WIFI_PASSWORD, password.encode()))


def static_ip(value: str) -> bytes:
    """ip/prefix,gateway, e.g. 192.168.1.10/24,192.168.1.1"""
    net, _, gw = value.partition(',')
    itf = ipaddress.IPv4Interface(net)
    return record(WIFI_IP, itf.ip.packed + itf.network.netmask.packed + ipaddress.IPv4Address(gw).packed)


def gateway(value: str) -> bytes:
    """ip[:port]"""
    ip, _, port = value.partition(':')
    return record(MQTTSN_GATEWAY, ipaddress.IPv4Address(ip).packed + struct.pack('<H', int(port or 1883)))


def selftest() -> None:
    # Example of docs/cfg-format.md
    image = build(record(NODE_NAME, b'kitchen') + record(NODE_ADDR, b'\xFF') + uart('1,17,16,-,38400')[1],
                  0x6708F000)
    expected = bytes.fromhex(
        '45 43 46 47 01 00 17 00 00 F0 08 67 4C 79 26 E2'
        '01 07 6B 69 74 63 68 65 6E 02 01 FF 10 09 01 11'
        '10 FF 00 96 00 00 00')
    assert image[:len(expected)] == expected, image[:len(expected)].hex(' ')
    assert len(image) == PARTITION_SIZE and set(image[len(expected):]) == {0xFF}
    assert zlib.crc32(b'123456789') == 0xCBF43926

    assert static_ip('192.168.1.10/24,192.168.1.1') == bytes([WIFI_IP, 12, 192, 168, 1, 10, 255, 255, 255, 0,
                                                              192, 168, 1, 1])
    assert gateway('192.168.1.2') == bytes([MQTTSN_GATEWAY, 6, 192, 168, 1, 2, 0x5B, 0x07])
    assert uart('2,17,16,4,200000,rs485')[1] == bytes([IF_UART, 9, 2, 17, 16, 4, 0x40, 0x0D, 0x03, 0x00, 1])
    print('selftest passed')


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--name', help='node name (ClientId), 1-23 bytes; default "<type>_<address>"')
    ap.add_argument('--addr', type=lambda v: int(v, 0), help='node address on serial / RS-485; default DHCP')
    ap.add_argument('--uart', dest='ifs', action='append', type=uart, metavar='PORT,TX,RX,DE,BAUD[,FRAMING]',
                    help='UART interface; DE "-" - none; framing slip (default) or rs485')
    ap.add_argument('--wifi', dest='ifs', action='append', type=wifi, metavar='SSID[,PASSWORD]',
                    help='WiFi interface')
    ap.add_argument('--ip', type=static_ip, metavar='IP/PREFIX,GATEWAY', help='WiFi static IP; default DHCP')
    ap.add_argument('--gateway', type=gateway, metavar='IP[:PORT]',
                    help='MQTT-SN gateway over WiFi; default broadcast SEARCHGW')
    ap.add_argument('--generation', type=lambda v: int(v, 0), help='default: now, seconds Unix')
    ap.add_argument('-o', '--output', type=Path, default=Path('cfg.bin'))
    ap.add_argument('--selftest', action='store_true', help='run self test and exit')
    args = ap.parse_args()

    if args.selftest:
        selftest()
        return
    if not args.ifs:
        ap.error('at least one interface (--uart or --wifi) is required')
    if (args.ip or args.gateway) and not any(kind == 'wifi' for kind, _ in args.ifs):
        ap.error('--ip and --gateway need --wifi')

    records = b''
    if args.name is not None:
        name = args.name.encode()
        if not 1 <= len(name) <= 23:
            ap.error('name: 1-23 bytes')
        records += record(NODE_NAME, name)
    if args.addr is not None:
        if not 1 <= args.addr <= 0xFF:
            ap.error('address: 1-255, 255 - DHCP')
        records += record(NODE_ADDR, bytes([args.addr]))
    records += b''.join(rec for _, rec in args.ifs)
    records += (args.ip or b'') + (args.gateway or b'')

    generation = args.generation if args.generation is not None else int(time.time())
    args.output.write_bytes(build(records, generation))
    print(f'{args.output}: generation {generation}, {len(records)} bytes of records')
    print(f'write: python -m esptool --port <PORT> write-flash 0x{PARTITION_OFFSET:x} {args.output}')


if __name__ == '__main__':
    try:
        main()
    except ValueError as e:
        sys.exit(f'error: {e}')
