#!/usr/bin/env python3
"""Audit the exported equipment's existing vanilla ground NIF collision, without writing game data."""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from data import esm
from data.build_esp import MASTER, REPO, armor, weapon
from data.skyrim_magic import Bsa


def block_types(raw):
    """Read only the SSE NIF header/type table; no geometry or Havok payload is modified."""
    at = raw.index(b'\n') + 1
    version, endian, user, count, bethesda = struct.unpack_from('<IBIII', raw, at)
    if (version, endian, user, bethesda) != (0x14020007, 1, 12, 100):
        raise ValueError('expected Skyrim SE NIF 20.2.0.7, user 12, BS 100')
    at += 17
    for _ in range(3):  # author, process script, export script (sized byte strings)
        at += 1 + raw[at]
    number = struct.unpack_from('<H', raw, at)[0]
    at += 2
    types = []
    for _ in range(number):
        size = struct.unpack_from('<I', raw, at)[0]
        at += 4
        types.append(raw[at:at + size].decode('ascii'))
        at += size
    indices = struct.unpack_from('<' + 'H' * count, raw, at)
    return [types[i] for i in indices]


def audit(catalog, master):
    templates = {r.edid: r for rows in esm.read(master, {'ARMO', 'WEAP', 'KYWD', 'MISC'}).values() for r in rows}
    archives = [Bsa(p) for p in sorted(master.parent.glob('Skyrim - Meshes*.bsa'))]
    meshes = {}
    for entry, item in catalog['items'].items():
        record = weapon(item, templates, 0x800) if item['class'] == 2 else armor(item, templates, 0x800)
        tags = ('MODL',) if record.type == 'WEAP' else ('MOD2', 'MOD4')
        paths = {record.get(tag).rstrip(b'\0').decode('cp1252').replace('\\', '/').lower()
                 for tag in tags if record.get(tag)}
        if not paths:
            raise ValueError(f'item {entry}: no ground model')
        for path in sorted(paths):
            if path not in meshes:
                key = 'meshes/' + path
                archive = next((b for b in archives if key in b.files), None)
                if archive is None:
                    raise FileNotFoundError(key)
                types = block_types(archive.read(key))
                collision = [t for t in types if t.startswith('bhk')]
                if 'bhkCollisionObject' not in collision or not any(t in collision for t in ('bhkRigidBody', 'bhkRigidBodyT')):
                    raise ValueError(f'{path}: no native rigid-body collision')
                meshes[path] = {'archive': archive.path.name, 'collision_blocks': collision, 'entries': []}
            meshes[path]['entries'].append(int(entry))
    for row in meshes.values():
        row['entries'].sort()
    return {'items': len(catalog['items']), 'meshes': meshes, 'all_have_rigid_body_collision': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog', type=Path, default=REPO / 'build/catalog.json')
    parser.add_argument('--master', type=Path, default=MASTER)
    args = parser.parse_args()
    print(json.dumps(audit(json.loads(args.catalog.read_text()), args.master), indent=2, sort_keys=True))


if __name__ == '__main__':
    main()
