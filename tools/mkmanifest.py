#!/usr/bin/env python3
"""Assemble the installer site: page, firmware and manifest.json for the page.

    python tools/mkmanifest.py build/default _site      # after idf.py -B build/default build
    python -m http.server -d _site                      # local check, Web Serial works on localhost

The build directory needs flasher_args.json, project_description.json and the files they name.
Partition addresses come from the built partition table, the one that is flashed.
"""
from __future__ import annotations

import argparse
import json
import shutil
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FW_DIR = 'fw'
ENTRY = struct.Struct('<2sBBII16sI')        # ESP-IDF partition table entry, 32 bytes
ENTRY_MAGIC = b'\xAA\x50'


def partitions(table: bytes) -> dict:
    parts = {}
    for pos in range(0, len(table) - ENTRY.size + 1, ENTRY.size):
        magic, ptype, subtype, offset, size, label, _ = ENTRY.unpack_from(table, pos)
        if magic != ENTRY_MAGIC:
            break                           # MD5 entry or end of the table
        parts[label.rstrip(b'\0').decode()] = {'type': ptype, 'subtype': subtype, 'offset': offset, 'size': size}
    return parts


def build_site(build: Path, site: Path, date: datetime) -> dict:
    args = json.loads((build / 'flasher_args.json').read_text(encoding='utf-8'))
    desc = json.loads((build / 'project_description.json').read_text(encoding='utf-8'))

    page = ROOT / 'site'
    if page.is_dir():
        shutil.copytree(page, site, dirs_exist_ok=True)
    (site / FW_DIR).mkdir(parents=True, exist_ok=True)

    files = []
    for offset, name in sorted(args['flash_files'].items(), key=lambda kv: int(kv[0], 0)):
        src = build / name
        dst = f'{FW_DIR}/{src.name}'
        shutil.copyfile(src, site / dst)
        files.append({'offset': int(offset, 0), 'path': dst, 'size': src.stat().st_size})

    manifest = {
        'project': desc['project_name'],
        'version': desc['project_version'],
        'date': date.strftime('%Y-%m-%dT%H:%M:%SZ'),
        'chip': args['extra_esptool_args']['chip'],
        'flash': args['flash_settings'],
        'files': files,
        'partitions': partitions((build / args['partition-table']['file']).read_bytes()),
    }
    for name in ('nvs', 'plc', 'cfg'):      # The page writes cfg and erases nvs and plc
        if name not in manifest['partitions']:
            raise ValueError(f'partition "{name}" missing in the partition table')
    (site / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return manifest


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('build', type=Path, help='build directory, e.g. build/default')
    ap.add_argument('site', type=Path, help='output directory')
    args = ap.parse_args()

    m = build_site(args.build, args.site, datetime.now(timezone.utc))
    print(f'{args.site}: {m["project"]} {m["version"]}, {len(m["files"])} files, '
          f'cfg at 0x{m["partitions"]["cfg"]["offset"]:X}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, KeyError, ValueError) as e:
        sys.exit(f'error: {e!r}')
