#!/usr/bin/env python3
"""Assemble dist/WoWBridge (MO2-style, Data-relative) from build outputs."""
import os
import shutil
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DIST = REPO / 'dist/WoWBridge'
FILES = {
    REPO / 'data/mounts.json': 'SKSE/Plugins/WoWBridge/mounts.json',  # build output -> Data-relative destination
    REPO / 'build/compass/WB_Compass.blp': 'SKSE/Plugins/WoWBridge/Icons/WB_Compass.blp',
    REPO / 'build/compass/manifest.json': 'SKSE/Plugins/WoWBridge/Icons/compass.json',
    REPO / 'build/skse/WoWBridge.dll': 'SKSE/Plugins/WoWBridge.dll',
    REPO / 'skse/WoWBridge.ini': 'SKSE/Plugins/WoWBridge.ini',
    REPO / 'build/esp/WoWBridge.esp': 'WoWBridge.esp',
    REPO / 'build/character.json': 'SKSE/Plugins/WoWBridge/character.json',   # data/build_catalog.py
    REPO / 'data/character-male.json': 'SKSE/Plugins/WoWBridge/character-male.json',
    REPO / 'data/progression.json': 'SKSE/Plugins/WoWBridge/progression.json',   # wb learnall rank metadata
    REPO / 'build/gear.json': 'SKSE/Plugins/WoWBridge/gear.json',   # data/build_esp.py
    REPO / 'data/skyrim_gear_looks.json': 'SKSE/Plugins/WoWBridge/skyrim_gear_looks.json',
}

ICON_SOURCE = REPO / 'build/skyrim_icons'   # tools/skyrim_icons.py (generated: game data + the WoW client)
ICON_DESTINATION = 'SKSE/Plugins/WoWBridge/Icons'
MINIMAP_SOURCE = REPO / 'build/skyrim_minimap'
MINIMAP_DESTINATION = 'SKSE/Plugins/WoWBridge/Minimap'


def pack_icons(destination):
    """Ship the reviewed BLPs and their lookup data; contact sheets stay in the repo."""
    manifest = ICON_SOURCE / 'manifest.json'
    if not manifest.is_file():   # not generated here: Skyrim's magic keeps the plugin's icons by kind
        print(f'no Skyrim icons ({manifest}): run data/skyrim_magic.py and tools/skyrim_icons.py to make them')
        return
    import json
    names = {row['icon'] + '.blp' for row in json.loads(manifest.read_text())['forms'].values()}
    names.update(('manifest.json', 'inventory.json'))
    for name in sorted(names):
        if Path(name).name != name:
            raise ValueError(f'invalid icon filename in manifest: {name}')
        source = ICON_SOURCE / name
        if not source.is_file():
            raise FileNotFoundError(f'missing generated icon asset: {source}')
    target = destination / ICON_DESTINATION
    target.mkdir(parents=True, exist_ok=True)
    for name in sorted(names):
        shutil.copyfile(ICON_SOURCE / name, target / name)
    print(f'packed {ICON_DESTINATION}: {len(names) - 2} icons and manifest/inventory')


def pack_minimap(destination):
    """Validate the entire atlas before copying. Stitched review PNGs stay local."""
    import hashlib
    import json
    manifest = MINIMAP_SOURCE / 'manifest.json'
    if not manifest.is_file():
        print(f'no static minimap ({manifest}): run tools/skyrim_minimap.py')
        return
    data = json.loads(manifest.read_text())
    if data['version'] != 1 or data['tile_pixels'] != 256 or data['tile_span_units'] != 16384 or data['units_per_yard'] != 64:
        raise ValueError('unsupported minimap manifest')
    expected_index = ['WOWBRIDGE_MINIMAP 1 64 16384']
    names = {'manifest.json', 'index.tsv'}
    for world, row in sorted(data['worldspaces'].items()):
        if overview := row.get('overview'):
            name = overview['file']
            if Path(name).name != name or name != f'WB_{world}_overview.blp' or name in names:
                raise ValueError(f'invalid/duplicate minimap overview filename: {name}')
            if hashlib.sha256((MINIMAP_SOURCE / name).read_bytes()).hexdigest() != overview['sha256']:
                raise ValueError(f'changed minimap asset: {name}')
            names.add(name)
        for tile in sorted(row['tiles'], key=lambda t: t['cell']):
            name = tile['file']
            x, y = tile['cell']
            if Path(name).name != name or name != f'WB_{world}_{x}_{y}.blp' or name in names:
                raise ValueError(f'invalid/duplicate minimap filename: {name}')
            raw = (MINIMAP_SOURCE / name).read_bytes()
            if hashlib.sha256(raw).hexdigest() != tile['sha256']:
                raise ValueError(f'changed minimap asset: {name}')
            names.add(name)
            expected_index.append(f'{world}\t{x}\t{y}\t{name}')
    if (MINIMAP_SOURCE / 'index.tsv').read_text() != '\n'.join(expected_index) + '\n':
        raise ValueError('minimap index does not match manifest')
    target = destination / MINIMAP_DESTINATION
    target.mkdir(parents=True, exist_ok=True)
    for name in sorted(names):   # linked, not copied: thousands of tiles, packaged on every build
        try:
            os.link(MINIMAP_SOURCE / name, target / name)
        except OSError:
            shutil.copyfile(MINIMAP_SOURCE / name, target / name)
    tile_count = sum(len(row['tiles']) for row in data['worldspaces'].values())
    print(f'packed {MINIMAP_DESTINATION}: {tile_count} terrain tiles, {len(names) - tile_count - 2} overviews and manifest/index')

if __name__ == '__main__':
    shutil.rmtree(DIST, ignore_errors=True)
    for src, rel in FILES.items():
        if src.exists():
            (DIST / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, DIST / rel)
            print('packed', rel)
    pack_icons(DIST)
    pack_minimap(DIST)
