#!/usr/bin/env python3
"""Build a north-up minimap atlas from Skyrim SE's level-4 terrain diffuse DDSs.

Uses the repository's lazy BSA v105/LZ4 reader and Pillow's BC1/BC3 decoder.
No game, client archives, network, or native renderer is needed at generation time.
"""
import argparse
from functools import lru_cache
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import sys

from PIL import Image, ImageChops

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from data import esm
from data.skyrim_magic import Bsa, plugin_header

ROOT = Path(__file__).resolve().parents[1]
DATA = Path(__file__).resolve().parents[2] / 'steamapps/common/Skyrim Special Edition/Data'
PATTERN = re.compile(r'textures/terrain/([a-z0-9_]+)/\1\.4\.(-?\d+)\.(-?\d+)\.dds$')
CELL_UNITS, LOD_CELLS, UNITS_PER_YARD = 4096, 4, 64
TILE_UNITS = CELL_UNITS * LOD_CELLS
TILE_PIXELS = 256
PIXELS_PER_CELL = TILE_PIXELS // LOD_CELLS


def worldspace_records(data, plugins=None):
    """Winning WRLD records, resolving WNAM through each plugin's own MAST table.

    Explicit plugins are lowest priority first; their masters load before them.
    Automatic discovery follows the shipped masters, then other ESM/ESL files.
    Like archive discovery, this is not a mod manager's active load order.
    """
    available = {p.name.casefold(): p for p in data.iterdir()
                 if p.suffix.casefold() in ('.esm', '.esl', '.esp')}
    base = ['skyrim.esm', 'update.esm', 'dawnguard.esm', 'hearthfires.esm', 'dragonborn.esm']
    requested = plugins if plugins is not None else [available[n] for n in base if n in available] + [
        p for n, p in sorted(available.items()) if n not in base and p.suffix.casefold() in ('.esm', '.esl')]
    records, loaded, loading = {}, set(), set()

    def load(path):
        name = path.name.casefold()
        if name in loaded:
            return
        if name in loading:
            raise ValueError(f'cyclic plugin masters: {path.name}')
        loading.add(name)
        _, masters = plugin_header(path)
        for master in masters:
            if master.casefold() not in available:
                raise FileNotFoundError(f'missing master {master} for {path.name}')
            load(available[master.casefold()])
        files = [m.casefold() for m in masters] + [name]

        def resolve(form):
            index = form >> 24
            if index >= len(files):
                raise ValueError(f'invalid WRLD form {form:08X} in {path.name}')
            return files[index], form & 0xffffff

        for record in esm.read(path, {'WRLD'})['WRLD']:
            if record.flags & 0x1000:  # ignored override
                continue
            key = resolve(record.form)
            if record.flags & 0x20:  # deleted
                records.pop(key, None)
                continue
            parent = record.get('WNAM')
            form = struct.unpack('<I', parent)[0] if parent else 0
            records[key] = {'edid': record.edid.lower(), 'parent_key': resolve(form) if form else None,
                            'source_plugin': path.name}
        loading.remove(name)
        loaded.add(name)

    for path in requested:
        load(Path(path))
    worlds = {}
    for key, record in records.items():
        if not record['edid']:
            raise ValueError(f'WRLD has no editor ID: {key}')
        parent = record['parent_key']
        if parent is not None and parent not in records:
            raise ValueError(f'unresolved WRLD parent: {key} -> {parent}')
        name = record['edid']
        if name in worlds:
            raise ValueError(f'duplicate WRLD editor ID: {name}')
        worlds[name] = {'parent': records[parent]['edid'] if parent else None,
                        'source_plugin': record['source_plugin']}
    for name in worlds:
        seen, current = set(), name
        while current is not None:
            if current in seen:
                raise ValueError(f'cyclic WRLD parents: {name}')
            seen.add(current)
            current = worlds[current]['parent']
    return worlds


def empty_lod_mask(image):
    """Black LOD padding, including BC1's near-black quantization fringe.

    Test the source, before the display curve. All channels must be <= 8;
    coloured/dark terrain and Blackreach's green source texture are preserved.
    """
    rgb = image.convert('RGB')
    red, green, blue = rgb.split()
    maximum = ImageChops.lighter(ImageChops.lighter(red, green), blue)
    # A few dark terrain texels alone are not evidence of empty LOD. Every
    # shipped padded tile has exact black; Blackreach has none.
    mask = (maximum.point([255 if value <= 8 else 0 for value in range(256)])
            if maximum.histogram()[0] else Image.new('L', image.size))
    # Flat fill: MarkarthWorld pads unbuilt land with one exact colour, (75, 73, 61), over up to 13% of a tile. Natural
    # terrain in these uncompressed sources never repeats one colour over 5% of a tile, so such a colour is padding; a
    # wholly flat tile (Blackreach's green) is that world's own texture.
    colours = rgb.getcolors(maxcolors=256 * 256)
    if colours:
        count, colour = max(colours)
        area = image.size[0] * image.size[1]
        if colour != (0, 0, 0) and 0.05 * area < count < 0.95 * area:
            r, g, b = (channel.point([255 if value == c else 0 for value in range(256)]) for channel, c in zip((red, green, blue), colour))
            mask = ImageChops.lighter(mask, ImageChops.multiply(ImageChops.multiply(r, g), b))
    return mask


class TerrainTiles:
    """Composite native texels in world space, before colour grading and mips."""
    def __init__(self, sources, records):
        self.sources, self.records = sources, records
        self.origins = {}
        for world, x, y in sources:
            self.origins.setdefault(world, (x, y))

    def parent(self, world):
        return self.records.get(world, {}).get('parent')

    @lru_cache(maxsize=128)
    def raw(self, world, x, y):
        archive, name = self.sources[world, x, y]
        source = archive.read(name)
        image = Image.open(io.BytesIO(source)).convert('RGB')
        if image.size != (TILE_PIXELS, TILE_PIXELS):
            raise ValueError(f'expected 256x256 terrain tile: {name}, got {image.size}')
        return image, hashlib.sha256(source).hexdigest()

    def region(self, world, x, y):
        """A four-cell region on the requesting grid; coverage excludes atlas holes."""
        image = Image.new('RGB', (TILE_PIXELS, TILE_PIXELS))
        coverage = Image.new('L', image.size)
        if world is None:
            return image, coverage
        parent = self.parent(world)
        if parent is not None:
            image, coverage = self.region(parent, x, y)
        origin = self.origins.get(world)
        if origin is None:
            return image, coverage
        start_x = origin[0] + (x - origin[0]) // LOD_CELLS * LOD_CELLS
        start_y = origin[1] + (y - origin[1]) // LOD_CELLS * LOD_CELLS
        for tx in range(start_x, x + LOD_CELLS, LOD_CELLS):
            for ty in range(start_y, y + LOD_CELLS, LOD_CELLS):
                if (world, tx, ty) not in self.sources:
                    continue
                tile, _ = self.composite(world, tx, ty)
                # East-right, north-up. Cell offsets are exact multiples of 64
                # texels: no resizing or clamping at the parent's tile edges.
                at = ((tx - x) * PIXELS_PER_CELL, (y - ty) * PIXELS_PER_CELL)
                image.paste(tile, at)
                coverage.paste(255, (at[0], at[1], at[0] + TILE_PIXELS, at[1] + TILE_PIXELS))
        return image, coverage

    @lru_cache(maxsize=128)
    def composite(self, world, x, y):
        image, _ = self.raw(world, x, y)
        parent = self.parent(world)
        empty = empty_lod_mask(image)
        count = empty.histogram()[255]
        filled = 0
        if parent is not None and count:
            under, coverage = self.region(parent, x, y)
            mask = ImageChops.multiply(empty, coverage)
            filled = mask.histogram()[255]
            image = Image.composite(under, image, mask)
        return image, {'empty_pixels': count, 'parent_pixels': filled,
                       'uncovered_empty_pixels': count - filled}


def terrain_look(image):
    """A fixed curve shared by every tile: slightly brighter, clearer natural terrain.

    No per-tile auto contrast (which would introduce seams) and no invented roads.
    """
    curve = [max(0, min(255, round((v - 96) * 1.08 + 104))) for v in range(256)]
    return image.convert('RGB').point(curve * 3)


def dxt1(image):
    """The image's DXT1 (BC1) blocks, as the client's own minimap textures are compressed."""
    out = io.BytesIO()
    image.convert('RGB').save(out, 'DDS', pixel_format='DXT1')
    return out.getvalue()[128:]


def blp2(image):
    """BLP2 DXT1, 256x256, with nine mip levels: an eighth of BGRA's size (the atlas is ~300 MB, not 2.3 GB)."""
    if image.size != (256, 256):
        raise ValueError(f'expected 256x256 terrain tile, got {image.size}')
    levels = [dxt1(image)]
    for size in (128, 64, 32, 16, 8, 4, 2, 1):
        levels.append(dxt1(image.resize((size, size), Image.Resampling.LANCZOS)))
    offsets, sizes, at = [], [], 148 + 1024
    for mip in levels:
        offsets.append(at)
        sizes.append(len(mip))
        at += len(mip)
    return (struct.pack('<4sI4BII', b'BLP2', 1, 2, 0, 0, 1, 256, 256) +
            struct.pack('<16I', *(offsets + [0] * (16 - len(offsets)))) +
            struct.pack('<16I', *(sizes + [0] * (16 - len(sizes)))) + bytes(1024) + b''.join(levels))


def overview_blp2(image):
    """BGRA BLP2 with alpha and mip levels; absent atlas tiles stay transparent."""
    width, height = image.size
    levels = []
    w, h = width, height
    while True:
        levels.append(image.resize((w, h), Image.Resampling.LANCZOS).convert('RGBA').tobytes('raw', 'BGRA'))
        if w == h == 1:
            break
        w, h = max(1, w // 2), max(1, h // 2)
    offsets, sizes, at = [], [], 1172
    for mip in levels:
        offsets.append(at)
        sizes.append(len(mip))
        at += len(mip)
    return (struct.pack('<4sI4BII', b'BLP2', 1, 3, 8, 0, 1, width, height) +
            struct.pack('<16I', *(offsets + [0] * (16 - len(offsets)))) +
            struct.pack('<16I', *(sizes + [0] * (16 - len(sizes)))) + bytes(1024) + b''.join(levels))


def build_overviews(output, manifest=None):
    """Derive fast continent images from verified atlas BLPs, without game inputs.

    Each overview has the same bounds as its index's tiles. This also upgrades
    an existing generated atlas without re-exporting Skyrim's archives.
    """
    standalone = manifest is None
    if standalone:
        manifest = json.loads((output / 'manifest.json').read_text())
    for world, row in sorted(manifest['worldspaces'].items()):
        low_x, low_y, high_x, high_y = row['bounds_yards']
        width, height = int((high_x - low_x) / 8), int((high_y - low_y) / 8)
        image = Image.new('RGBA', (width, height))
        for tile in row['tiles']:
            raw = (output / tile['file']).read_bytes()
            if hashlib.sha256(raw).hexdigest() != tile['sha256']:
                raise ValueError(f"changed minimap asset: {tile['file']}")
            with Image.open(io.BytesIO(raw)) as source:
                if source.size != (256, 256):
                    raise ValueError(f"invalid minimap tile dimensions: {tile['file']}")
                # Paste actual tiles only. Sparse holes remain transparent to
                # the parent's overview under them, just as in the local view.
                x, y = tile['grid']
                image.paste(source.convert('RGBA').resize((32, 32), Image.Resampling.LANCZOS),
                            (x * 32, height - (y + 1) * 32))
        ratio = min(1, 1024 / max(width, height))
        image = image.resize((max(1, int(width * ratio)), max(1, int(height * ratio))), Image.Resampling.LANCZOS)
        raw = overview_blp2(image)
        filename = f'WB_{world}_overview.blp'
        (output / filename).write_bytes(raw)
        row['overview'] = {'file': filename, 'width': image.width, 'height': image.height,
                           'sha256': hashlib.sha256(raw).hexdigest()}
    if standalone:
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8')
        print(f"{len(manifest['worldspaces'])} world map overviews written to {output}", flush=True)
    return manifest


def inventory(data, archives=None):
    # Stable explicit priority: base textures, then DLC, then CC/other archives in lexical order.
    # Mod load-order/loose DDS overrides require an explicit --archive list, in override order.
    paths = archives if archives is not None else sorted(data.glob('*.bsa'), key=lambda p: (
        0 if p.name.casefold().startswith('skyrim - ') else
        1 if p.name.casefold().startswith(('dawnguard', 'hearthfires', 'dragonborn')) else 2, p.name.casefold()))
    sources = {}
    for path in paths:
        archive = Bsa(path)
        for name in sorted(archive.files):
            match = PATTERN.fullmatch(name)
            if not match:
                continue
            world, x, y = match[1], int(match[2]), int(match[3])
            sources[world, x, y] = archive, name
    if not sources:
        raise ValueError(f'no level-4 terrain diffuse tiles in {data}')
    return sources


def build(data=DATA, output=ROOT / 'build/skyrim_minimap', archives=None, plugins=None):
    sources = inventory(data, archives)
    records = worldspace_records(data, plugins)
    output.mkdir(parents=True, exist_ok=True)
    worlds = {}
    for world, x, y in sorted(sources):
        worlds.setdefault(world, {'tiles': []})['tiles'].append((x, y))
    previews = {}
    for world, row in worlds.items():
        coords = row['tiles']
        lo_x, lo_y = min(x for x, _ in coords), min(y for _, y in coords)
        hi_x, hi_y = max(x for x, _ in coords) + LOD_CELLS, max(y for _, y in coords) + LOD_CELLS
        if any((x - lo_x) % LOD_CELLS or (y - lo_y) % LOD_CELLS for x, y in coords):
            raise ValueError(f'inconsistent level-4 grid in {world}')
        row['bounds_units'] = [lo_x * CELL_UNITS, lo_y * CELL_UNITS, hi_x * CELL_UNITS, hi_y * CELL_UNITS]
        row['bounds_yards'] = [v / UNITS_PER_YARD for v in row['bounds_units']]
        row['preview'] = world + '.png'
        row['parent'] = records.get(world, {}).get('parent')
        previews[world] = Image.new('RGB', ((hi_x - lo_x) // LOD_CELLS * 32, (hi_y - lo_y) // LOD_CELLS * 32), '#121719')
        row['tiles'] = []
    terrain = TerrainTiles(sources, records)
    index = ['WOWBRIDGE_MINIMAP 1 64 16384']
    for (world, x, y), (archive, name) in sorted(sources.items()):
        _, source_hash = terrain.raw(world, x, y)
        composite, stats = terrain.composite(world, x, y)
        image = terrain_look(composite)
        encoded = blp2(image)
        filename = f'WB_{world}_{x}_{y}.blp'
        (output / filename).write_bytes(encoded)
        row = worlds[world]
        row['tiles'].append({'grid': [(x * CELL_UNITS - row['bounds_units'][0]) // TILE_UNITS, (y * CELL_UNITS - row['bounds_units'][1]) // TILE_UNITS], 'cell': [x, y], 'origin_units': [x * CELL_UNITS, y * CELL_UNITS],
                             'file': filename, 'wow_path': 'Interface\\Minimap\\' + filename,
                             'sha256': hashlib.sha256(encoded).hexdigest(),
                             'source': archive.path.name + ':' + name,
                             'source_sha256': source_hash, **stats})
        index.append(f'{world}\t{x}\t{y}\t{filename}')
        bounds = row['bounds_units']
        previews[world].paste(image.resize((32, 32), Image.Resampling.LANCZOS),
                             (((x * CELL_UNITS - bounds[0]) // TILE_UNITS) * 32, ((bounds[3] - y * CELL_UNITS) // TILE_UNITS - 1) * 32))
    for world, image in previews.items():
        image.save(output / worlds[world]['preview'])
    manifest = {'version': 1, 'tile_pixels': 256, 'tile_span_units': TILE_UNITS,
                'units_per_yard': UNITS_PER_YARD, 'lod_level': 4, 'orientation': 'east-right,north-up',
                'format': 'BLP2 DXT1, nine mip levels', 'look': 'fixed RGB curve: (v-96)*1.08+104',
                'empty_lod': 'source RGB channels <= 8; parent composited at native world coordinates',
                'worldspace_records': records, 'worldspaces': worlds}
    build_overviews(output, manifest)
    # Publish lookup files last. Remove stale generated tiles only after a successful build.
    expected = {tile['file'] for row in worlds.values() for tile in row['tiles']}
    expected.update(row['overview']['file'] for row in worlds.values())
    for stale in output.glob('WB_*.blp'):
        if stale.name not in expected:
            stale.unlink()
    (output / 'index.tsv').write_text('\n'.join(index) + '\n', encoding='utf-8')
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print(f'{len(sources)} terrain tiles in {len(worlds)} worldspaces written to {output}', flush=True)
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=DATA)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/skyrim_minimap')
    parser.add_argument('--archive', type=Path, action='append', help='explicit archives, lowest priority first')
    parser.add_argument('--plugin', type=Path, action='append', help='explicit WRLD plugins, lowest priority first (masters load automatically)')
    parser.add_argument('--overviews-only', action='store_true', help='add continent images to an existing generated atlas')
    args = parser.parse_args()
    if args.overviews_only:
        build_overviews(args.output)
    else:
        build(args.data, args.output, args.archive, args.plugin)
