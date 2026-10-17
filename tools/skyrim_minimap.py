#!/usr/bin/env python3
"""Build a north-up minimap atlas from Skyrim SE's terrain diffuse DDSs and LAND.

Uses the repository's lazy BSA v105/LZ4 reader and Pillow's BC1/BC3 decoder.
No game, client archives, network, or native renderer is needed at generation time.
"""
import argparse
from functools import lru_cache
import hashlib
import io
import json
import math
from pathlib import Path
import re
import struct
import sys
import zlib

from PIL import Image, ImageChops, ImageFilter, ImageStat
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from data import esm
from data.skyrim_magic import Bsa, plugin_header
from tools import nif

ROOT = Path(__file__).resolve().parents[1]
DATA = Path(__file__).resolve().parents[2] / 'steamapps/common/Skyrim Special Edition/Data'
PATTERN = re.compile(r'textures/terrain/([a-z0-9_]+)/\1\.4\.(-?\d+)\.(-?\d+)\.dds$')
CELL_UNITS, LOD_CELLS, UNITS_PER_YARD = 4096, 4, 64
TILE_UNITS = CELL_UNITS * LOD_CELLS
TILE_PIXELS = 256
PIXELS_PER_CELL = TILE_PIXELS // LOD_CELLS
LAND_PALETTE = (96, 91, 75)
# Native diffuse pixels, before the display curve; four-city calibration in docs/minimap.md.
CITY_GROUND_MAX_VARIANCE = 8
CITY_GROUND_COLOUR_TOLERANCE = 8
CITY_GROUND_MIN_PIXELS = 400
CITY_GROUND_FEATHER = 3
# Own-LOD worlds on another world's coordinates (its WNAM child, but PNAM does not use its LOD) whose LOD repeats that
# world's texels: their padding shows its rendering. Only MarkarthWorld's repeats Tamriel's (docs/minimap.md).
PADDING_SOURCES = {'markarthworld': 'tamriel'}
# MarkarthWorld's flat fill: (75, 73, 61) with up to four levels of noise; cell-edge texels blend up to eight levels.
PADDING_COLOUR, PADDING_NOISE, PADDING_EDGE = (75, 73, 61), 4, 8
# Building footprints: placed STATs of settlement architecture (base game and DLC folders) and Imperial fort exteriors,
# minus ground, paths and clutter pieces that share those folders, and the cities' LOD stand-ins in Tamriel.
BUILDING_MODEL = re.compile(r'(?:[^/]+/)?(?:architecture|dungeons/imperial/exterior)/.+\.nif')
BUILDING_EXCLUDE = re.compile(
    r'clutter|interior|terrain|ground|land|base|rubble|debris|(?<!raven)rock|cliff|mountain|plateau|'
    r'tree|leaves|plant|ivy|road|street|path|walkway|stair|steps|plaza|platform|floor|terrace|'
    r'dock|pier|bridge|canal|river|pond|pool|water|fence|rope|banner|bone|shadow|lod')
BUILDING_MIN_UNITS = 300
# Unreadable composite models must not cover streets/courtyards with one huge fallback rectangle.
BUILDING_MAX_UNITS = CELL_UNITS


def land_layers(record, resolve):
    """BTXT bases and ATXT alpha layers; VTXT belongs to the preceding ATXT."""
    layers, current = [], None
    for tag, data in record.subs:
        if tag in ('BTXT', 'ATXT'):
            if len(data) != 8:
                raise ValueError(f'expected 8-byte LAND {tag}, got {len(data)} bytes')
            form, quadrant, layer = struct.unpack('<IBxh', data)
            if quadrant > 3 or tag == 'ATXT' and layer < 0:
                raise ValueError('invalid LAND texture quadrant/layer')
            current = {'quadrant': quadrant, 'layer': -1 if tag == 'BTXT' else layer,
                       'texture': resolve(form) if form else None, 'opacity': b''}
            layers.append(current)
        elif tag == 'VTXT' and current is not None and current['layer'] >= 0:
            current['opacity'] = data
    return layers


def building_model(model):
    """A STAT model path that can be a building: architecture or fort exteriors, no ground/path/clutter pieces."""
    path = model.replace('\\', '/').casefold().removeprefix('meshes/')
    return bool(BUILDING_MODEL.fullmatch(path)) and not BUILDING_EXCLUDE.search(path)


def building_footprint(bounds, x, y, rotation, scale):
    """World corners of a reference's OBND rectangle: scaled, turned clockwise (seen from above) by its z rotation.

    Skyrim's z rotation turns local north (+y) towards east (+x); x/y tilt is ignored.
    """
    x1, y1, _, x2, y2, _ = bounds
    c, s = math.cos(rotation), math.sin(rotation)
    return tuple((x + (px * c + py * s) * scale, y + (py * c - px * s) * scale)
                 for px, py in ((x1, y1), (x2, y1), (x2, y2), (x1, y2)))


def worldspace_records(data, plugins=None, include_land=False, include_buildings=False):
    """Winning WRLD records, resolving WNAM through each plugin's own MAST table.

    Explicit plugins are lowest priority first; their masters load before them.
    Automatic discovery follows the shipped masters, then other ESM/ESL files.
    Like archive discovery, this is not a mod manager's active load order.
    With include_land, also return winning exterior LAND keyed by world/cell,
    and retain LOD-parent worlds' non-persistent exterior CELL footprints.
    With include_buildings, also return the initially enabled building footprints (see building_model).
    """
    available = {p.name.casefold(): p for p in data.iterdir()
                 if p.suffix.casefold() in ('.esm', '.esl', '.esp')}
    base = ['skyrim.esm', 'update.esm', 'dawnguard.esm', 'hearthfires.esm', 'dragonborn.esm']
    requested = plugins if plugins is not None else [available[n] for n in base if n in available] + [
        p for n, p in sorted(available.items()) if n not in base and p.suffix.casefold() in ('.esm', '.esl')]
    records, cells, lands, loaded, loading = {}, {}, {}, set(), set()
    textures, texture_sets = {}, {}
    statics, placed, states = {}, {}, {}

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
                raise ValueError(f'invalid terrain form {form:08X} in {path.name}')
            return files[index], form & 0xffffff

        types = ({'WRLD', 'LTEX', 'TXST'} if include_land else {'WRLD'}) | ({'STAT'} if include_buildings else set())
        plugin = esm.read(path, types)
        for record in plugin.get('STAT', []):
            if record.flags & 0x1000:
                continue
            key, model, bounds = resolve(record.form), record.get('MODL'), record.get('OBND')
            model = model.rstrip(b'\0').decode('cp1252') if model else ''
            if record.flags & 0x20 or not bounds or len(bounds) != 12 or not building_model(model):
                statics.pop(key, None)
            else:
                statics[key] = (model, struct.unpack('<6h', bounds))
        for tag, target, sub in [('LTEX', textures, 'TNAM'), ('TXST', texture_sets, 'TX00')]:
            for record in plugin.get(tag, []):
                if record.flags & 0x1000:
                    continue
                key = resolve(record.form)
                if record.flags & 0x20:
                    target.pop(key, None)
                    continue
                value = record.get(sub)
                if tag == 'LTEX':
                    form = struct.unpack('<I', value)[0] if value else 0
                    target[key] = resolve(form) if form else None
                else:
                    texture_path = value.rstrip(b'\0').decode('cp1252').replace('\\', '/').casefold() if value else ''
                    target[key] = ('textures/' + texture_path.removeprefix('textures/')) if texture_path else None
        for record in plugin['WRLD']:
            if record.flags & 0x1000:  # ignored override
                continue
            key = resolve(record.form)
            if record.flags & 0x20:  # deleted
                records.pop(key, None)
                continue
            # A worldspace shows its parent's terrain only when its PNAM flags say to use the parent's LOD
            # data (the walled cities do; Markarth, Blackreach, the Soul Cairn and the cave worldspaces do not).
            parent, use = record.get('WNAM'), record.get('PNAM')
            flags = struct.unpack('<H', use[:2])[0] if use and len(use) >= 2 else 0
            form = struct.unpack('<I', parent)[0] if parent and flags & 0x02 else 0
            records[key] = {'edid': record.edid.lower(), 'parent_key': resolve(form) if form else None,
                            'source_plugin': path.name}
        references = frozenset({'REFR', 'ACHR'} if include_buildings else ())

        def place(world, key, record):
            # Enable state follows the parent (XESP; bit 0 inverts it), else the initially-disabled flag.
            parent = record.get('XESP') or b''
            parent = (resolve(struct.unpack_from('<I', parent)[0]), parent[4] & 1) if len(parent) > 4 else None
            if record.flags & 0x20 or not (record.flags & 0x800 or parent):
                states.pop(key, None)
            else:
                states[key] = (bool(record.flags & 0x800), parent)
            # Keep references to building STATs known so far; the winning STAT is checked again at the end.
            base, position, scale = record.get('NAME'), record.get('DATA'), record.get('XSCL')
            base = resolve(struct.unpack('<I', base)[0]) if base and len(base) == 4 else None
            if record.flags & 0x20 or base not in statics or not position or len(position) != 24:
                placed.pop(key, None)
                return
            x, y, _, _, _, rotation = struct.unpack('<6f', position)
            placed[key] = (resolve(world), base, x, y, rotation,
                           struct.unpack('<f', scale)[0] if scale and len(scale) == 4 else 1.0,
                           f'{path.name}:REFR:{record.form:08X}')

        if include_land or include_buildings:
            for world, cell, record in esm.exterior_land(path, references):
                if record.flags & 0x1000:
                    continue
                key = resolve(record.form)
                if record.type in references:
                    place(world, key, record)
                elif not include_land:
                    continue
                elif record.type == 'CELL':
                    if record.flags & 0x20:
                        cells.pop(key, None)
                    elif record.get('XCLC'):
                        cells[key] = (resolve(world), *struct.unpack_from('<ii', record.get('XCLC')),
                                      bool(record.flags & 0x400))
                elif record.flags & 0x20:
                    lands.pop(key, None)
                else:
                    height, colour = record.get('VHGT'), record.get('VCLR')
                    lands[key] = (resolve(cell), {'height': height, 'colour': colour,
                                                'layers': land_layers(record, resolve),
                                                'source': f'{path.name}:LAND:{record.form:08X}'})
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
    result = [worlds]
    if include_land:
        for key, record in records.items():
            if record['parent_key'] is not None:
                worlds[record['edid']]['cells'] = sorted({(x, y) for world, x, y, persistent in cells.values()
                                                        if world == key and not persistent})
        terrain = {}
        for cell, land in lands.values():
            if cell not in cells or land['height'] is None:
                continue
            world, x, y, _ = cells[cell]
            if world in records:
                for layer in land['layers']:
                    layer['texture'] = texture_sets.get(textures.get(layer['texture']))
                terrain[records[world]['edid'], x, y] = land
        result.append(terrain)
    if include_buildings:
        def enabled(key, depth=0):
            disabled, parent = states.get(key, (False, None))
            if parent is None or depth > 64:   # a cycle of enable parents keeps its own flag
                return not disabled
            return enabled(parent[0], depth + 1) != bool(parent[1])

        buildings = []
        for key, (world, base, x, y, rotation, scale, source) in sorted(placed.items()):
            if base not in statics or world not in records or not enabled(key):
                continue
            model, bounds = statics[base]
            sides = sorted(side * scale for side in (bounds[3] - bounds[0], bounds[4] - bounds[1]))
            if sides[0] < BUILDING_MIN_UNITS:
                continue
            buildings.append({'world': records[world]['edid'], 'model': model, 'source': source,
                              'position': (x, y), 'rotation': rotation, 'scale': scale, 'bounds': bounds,
                              'corners': building_footprint(bounds, x, y, rotation, scale)})
        result.append(buildings)
    return result[0] if len(result) == 1 else tuple(result)


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


def land_heights(data):
    """VHGT's southwest base and signed deltas, in Skyrim units (eight per step)."""
    if len(data) != 1096:
        raise ValueError(f'expected 33x33 VHGT, got {len(data)} bytes')
    base = struct.unpack_from('<f', data)[0]
    if not math.isfinite(base):
        raise ValueError('non-finite LAND base height')
    deltas = struct.unpack_from('<1089b', data, 4)
    rows = []
    for y in range(33):
        base += deltas[y * 33]
        row = [base * 8]
        for x in range(1, 33):
            row.append(row[-1] + deltas[y * 33 + x] * 8)
        rows.append(row)
    return rows


def render_land_cell(land, palette, corners=None, tones=None):
    """North-up relief with diffuse corners or a southwest-first texture tone grid."""
    heights = land_heights(land['height'])
    colours = land['colour']
    if colours is not None and len(colours) != 33 * 33 * 3:
        raise ValueError(f'expected 33x33 VCLR, got {len(colours)} bytes')
    pixels = []
    for y in range(32, -1, -1):
        for x in range(33):
            left, right = max(0, x - 1), min(32, x + 1)
            south, north = max(0, y - 1), min(32, y + 1)
            dx = (heights[y][right] - heights[y][left]) / ((right - left) * CELL_UNITS / 32)
            dy = (heights[north][x] - heights[south][x]) / ((north - south) * CELL_UNITS / 32)
            # Flat terrain retains the palette brightness. Height differences
            # define surface normals; absolute height never rescales a tile.
            light = max(0, (0.4 * dx - 0.5 * dy + 0.8) / math.sqrt(1 + dx * dx + dy * dy))
            shade = 0.65 + 0.35 * light / 0.8
            tint = colours[(y * 33 + x) * 3:(y * 33 + x + 1) * 3] if colours else (255, 255, 255)
            if tones is not None:
                palette = tones[y][x]
            elif corners is not None:
                sw, se, nw, ne = corners
                palette = tuple((sw[i] * (32 - x) + se[i] * x) * (32 - y) / 1024 +
                                (nw[i] * (32 - x) + ne[i] * x) * y / 1024 for i in range(3))
            pixels.append(tuple(max(0, min(255, round(c * shade * (0.8 + 0.2 * v / 255))))
                                for c, v in zip(palette, tint)))
    image = Image.new('RGB', (33, 33))
    image.putdata(pixels)
    return image.resize((PIXELS_PER_CELL, PIXELS_PER_CELL), Image.Resampling.BILINEAR)


class LandTiles:
    """Group exterior LAND on the existing four-cell grid, filling missing tiles only."""
    def __init__(self, terrain, lands, textures=None):
        self.terrain, self.tiles = terrain, {}
        self.textures = textures or {}
        for (world, x, y), land in sorted(lands.items()):
            ox, oy = terrain.origins.get(world, (0, 0))
            tx, ty = ox + (x - ox) // LOD_CELLS * LOD_CELLS, oy + (y - oy) // LOD_CELLS * LOD_CELLS
            if (world, tx, ty) not in terrain.sources:
                self.tiles.setdefault((world, tx, ty), []).append((x, y, land))

    @lru_cache(maxsize=128)
    def diffuse_colour(self, world, x, y):
        image, _ = self.terrain.raw(world, x, y)
        mask = ImageChops.invert(empty_lod_mask(image))
        if not mask.getbbox():
            return None
        return tuple(ImageStat.Stat(image, mask).mean)

    @lru_cache(maxsize=256)
    def palette(self, world, x, y):
        # Keep source means within the LOD chain for seam tone beside real tiles.
        current = world
        while current is not None and current not in self.terrain.origins:
            current = self.terrain.parent(current)
        coords = [key for key in self.terrain.sources if key[0] == current]
        nearest = sorted(coords, key=lambda key: ((key[1] + 2 - x) ** 2 + (key[2] + 2 - y) ** 2, key))[:4]
        samples = []
        for key in nearest:
            colour = self.diffuse_colour(*key)
            if colour is not None:
                distance = max(1, (key[1] + 2 - x) ** 2 + (key[2] + 2 - y) ** 2)
                samples.append((colour, 1 / distance))
        if not samples:
            return None
        return tuple(sum(colour[i] * weight for colour, weight in samples) / sum(weight for _, weight in samples)
                     for i in range(3))

    @lru_cache(maxsize=None)
    def texture_colour(self, name):
        """Decode and average each winning diffuse DDS once, retaining its input hash."""
        source = self.textures.get(name)
        if source is None:
            return None, None
        archive, path = source
        raw = archive.read(path)
        with Image.open(io.BytesIO(raw)) as image:
            colour = tuple(ImageStat.Stat(image.convert('RGB')).mean)
        return colour, hashlib.sha256(raw).digest()

    def texture_tones(self, land, defaults=None):
        """Blend each quadrant's 17x17 alpha layers over its base in layer order."""
        quadrants = [[defaults[(q // 2) * 16 + p // 17][(q % 2) * 16 + p % 17]
                      if defaults is not None else LAND_PALETTE for p in range(289)] for q in range(4)]
        for layer in sorted(land.get('layers', []), key=lambda layer: layer['layer']):
            colour, _ = self.texture_colour(layer['texture'])
            if colour is None:
                continue
            pixels = quadrants[layer['quadrant']]
            if layer['layer'] < 0:
                pixels[:] = [colour] * 289
                continue
            data = layer['opacity']
            if len(data) % 8:
                raise ValueError(f'expected 8-byte LAND VTXT entries, got {len(data)} bytes')
            for position, opacity in struct.iter_unpack('<H2xf', data):
                if position >= 289 or not math.isfinite(opacity):
                    raise ValueError('invalid LAND VTXT position/opacity')
                opacity = max(0, min(1, opacity))
                pixels[position] = tuple(a * (1 - opacity) + b * opacity for a, b in zip(pixels[position], colour))
        # Quadrants share their border vertices. Average those duplicates before
        # relief shading and resizing, preserving southwest-first LAND indexing.
        tones = [[[] for _ in range(33)] for _ in range(33)]
        for quadrant, pixels in enumerate(quadrants):
            ox, oy = (quadrant % 2) * 16, (quadrant // 2) * 16
            for position, colour in enumerate(pixels):
                tones[oy + position // 17][ox + position % 17].append(colour)
        return [[tuple(sum(c[i] for c in samples) / len(samples) for i in range(3))
                 for samples in row] for row in tones]

    def render(self, world, x, y):
        image, coverage = self.terrain.region(self.terrain.parent(world), x, y)
        mask = Image.new('L', image.size)
        source_hash = hashlib.sha256()
        cells, sources = [], []
        for cx, cy, land in self.tiles[world, x, y]:
            corners = [self.palette(world, px, py) for px, py in
                       ((cx, cy), (cx + 1, cy), (cx, cy + 1), (cx + 1, cy + 1))]
            use_textures = any(colour is None for colour in corners)
            cell = (render_land_cell(land, LAND_PALETTE, tones=self.texture_tones(land)) if use_textures else
                    render_land_cell(land, corners[0], corners))
            at = ((cx - x) * PIXELS_PER_CELL, (y + LOD_CELLS - 1 - cy) * PIXELS_PER_CELL)
            image.paste(cell, at)
            mask.paste(255, (at[0], at[1], at[0] + PIXELS_PER_CELL, at[1] + PIXELS_PER_CELL))
            cells.append([cx, cy])
            sources.append(land['source'])
            source_hash.update(struct.pack('<ii', cx, cy) + land['height'] + (land['colour'] or b''))
            if use_textures:
                for layer in land.get('layers', []):
                    _, digest = self.texture_colour(layer['texture'])
                    source_hash.update(struct.pack('<Bh', layer['quadrant'], layer['layer']) +
                                       (layer['texture'] or '').encode('utf-8') + b'\0' + layer['opacity'] + (digest or b''))
        empty = TILE_PIXELS ** 2 - mask.histogram()[255]
        filled = ImageChops.multiply(ImageChops.invert(mask), coverage).histogram()[255]
        return image, {'land_derived': True, 'land_cells': cells, 'source': ';'.join(sources),
                       'source_sha256': source_hash.hexdigest(), 'empty_pixels': empty,
                       'parent_pixels': filled, 'uncovered_empty_pixels': empty - filled}


def pixel_components(mask, minimum=CITY_GROUND_MIN_PIXELS):
    """Eight-connected native pixel coordinates; a zero border keeps traversal within the image."""
    remaining = np.pad(mask, 1).ravel()
    width = mask.shape[1] + 2
    shifts = (-width - 1, -width, -width + 1, -1, 1, width - 1, width, width + 1)
    for first in np.flatnonzero(remaining):
        if not remaining[first]:
            continue
        remaining[first] = False
        pending, region = [int(first)], []
        while pending:
            current = pending.pop()
            region.append(current)
            for shift in shifts:
                neighbour = current + shift
                if remaining[neighbour]:
                    remaining[neighbour] = False
                    pending.append(neighbour)
        if len(region) >= minimum:
            positions = np.array(region)
            yield positions // width - 1, positions % width - 1


def padding_mask(pixels):
    """Own-LOD flat fill in an RGB array: connected regions of the noisy fill colour, with their blended edge texels."""
    deviation = np.abs(pixels.astype(int) - PADDING_COLOUR).max(axis=2)
    fill = np.zeros(deviation.shape, dtype=np.uint8)
    for rows, columns in pixel_components(deviation <= PADDING_NOISE):
        fill[rows, columns] = 255
    edge = np.asarray(Image.fromarray(fill).filter(ImageFilter.MaxFilter(3))) != 0
    return (fill != 0) | edge & (deviation <= PADDING_EDGE)


def feather(mask):
    """Opacity rising over CITY_GROUND_FEATHER pixels inward from a mask's perimeter and holes: eroded smoothstep."""
    alpha, eroded = np.zeros((mask.height, mask.width)), mask
    for _ in range(CITY_GROUND_FEATHER):
        eroded = eroded.filter(ImageFilter.MinFilter(3))
        alpha += np.asarray(eroded) / (255 * CITY_GROUND_FEATHER)
    return Image.fromarray(np.uint8(np.rint(255 * alpha * alpha * (3 - 2 * alpha))))


def spread(pixels, known, steps):
    """Extend known texels outward: each step paints the next ring with the mean of its known 3x3 neighbours."""
    pixels, known = pixels.astype(float), known.copy()
    height, width = known.shape
    for _ in range(steps):
        values = np.pad(pixels * known[:, :, None], ((1, 1), (1, 1), (0, 0)))
        weights = np.pad(known, 1).astype(float)
        total = sum(values[y:y + height, x:x + width] for y in range(3) for x in range(3))
        count = sum(weights[y:y + height, x:x + width] for y in range(3) for x in range(3))
        ring = ~known & (count > 0)
        pixels[ring] = total[ring] / count[ring][:, None]
        known |= ring
    return pixels


class CityGroundTiles:
    """LAND on connected near-constant diffuse pixels inside a LOD child's CELL footprint.

    Pixel regions share tone matching and a three-pixel feather across cell/tile borders.
    This never feeds back into TerrainTiles: detection and ring samples use original diffuse.
    """
    def __init__(self, terrain, lands, fallback):
        self.terrain, self.lands, self.fallback = terrain, lands, fallback
        self.cells, self.components, self.component_cells = {}, [], {}
        candidates = {}
        for world, record in sorted(terrain.records.items()):
            if record['parent'] is None:
                continue
            chain = [world]
            while terrain.parent(chain[-1]) is not None:
                chain.append(terrain.parent(chain[-1]))
            for x, y in record.get('cells', []):
                owner = next((name for name in chain if (name, x, y) in lands), None)
                if owner is None:
                    continue
                # Overlapping children prefer their own LAND, then the deeper child, then editor ID.
                priority = (owner != world, -len(chain), world)
                key = (chain[-1], x, y)
                if key not in candidates or priority < candidates[key][0]:
                    candidates[key] = (priority, lands[owner, x, y], world, owner)
        remaining = set(candidates)
        while remaining:
            first = min(remaining)
            remaining.remove(first)
            pending, cells = [first], []
            while pending:
                world, x, y = pending.pop()
                cells.append((world, x, y))
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        key = (world, x + dx, y + dy)
                        if key in remaining:
                            remaining.remove(key)
                            pending.append(key)
            # Detect in world coordinates, so neither CELL nor DDS boundaries split pixel regions.
            low_x, low_y = min(x for _, x, _ in cells) - 1, min(y for _, _, y in cells) - 1
            high_x, high_y = max(x for _, x, _ in cells) + 2, max(y for _, _, y in cells) + 2
            size = ((high_x - low_x) * PIXELS_PER_CELL, (high_y - low_y) * PIXELS_PER_CELL)
            diffuse, coverage = self.diffuse_region(world, low_x, high_y, size)
            footprint = Image.new('L', size)
            for _, x, y in cells:
                left, top = (x - low_x) * PIXELS_PER_CELL, (high_y - y - 1) * PIXELS_PER_CELL
                footprint.paste(255, (left, top, left + PIXELS_PER_CELL, top + PIXELS_PER_CELL))
            for mask in self.placeholder_masks(diffuse, coverage, footprint):
                selected = []
                for key in sorted(cells):
                    _, x, y = key
                    left, top = (x - low_x) * PIXELS_PER_CELL, (high_y - y - 1) * PIXELS_PER_CELL
                    if mask.crop((left, top, left + PIXELS_PER_CELL, top + PIXELS_PER_CELL)).getbbox():
                        selected.append(key)
                        self.cells[key] = candidates[key][1:]
                        self.component_cells.setdefault(key, []).append(len(self.components))
                # Keep a cell margin for smoothing/ring samples without retaining the entire city canvas.
                patch_low_x = min(x for _, x, _ in selected) - 1
                patch_high_y = max(y for _, _, y in selected) + 2
                patch_high_x = max(x for _, x, _ in selected) + 2
                patch_low_y = min(y for _, _, y in selected) - 1
                left, top = (patch_low_x - low_x) * PIXELS_PER_CELL, (high_y - patch_high_y) * PIXELS_PER_CELL
                box = (left, top, left + (patch_high_x - patch_low_x) * PIXELS_PER_CELL,
                       top + (patch_high_y - patch_low_y) * PIXELS_PER_CELL)
                self.components.append((selected, mask.crop(box), patch_low_x, patch_high_y))

    def diffuse_region(self, world, low_x, high_y, size):
        """Original diffuse and non-black coverage, with no LAND, buildings or parent padding fill."""
        diffuse, coverage = Image.new('RGB', size), Image.new('L', size)
        origin = self.terrain.origins.get(world)
        if origin is None:
            return diffuse, coverage
        low_y = high_y - size[1] // PIXELS_PER_CELL
        high_x = low_x + size[0] // PIXELS_PER_CELL
        ox, oy = origin
        for x in range(ox + (low_x - ox) // LOD_CELLS * LOD_CELLS, high_x, LOD_CELLS):
            for y in range(oy + (low_y - oy) // LOD_CELLS * LOD_CELLS, high_y, LOD_CELLS):
                if (world, x, y) not in self.terrain.sources:
                    continue
                image, _ = self.terrain.raw(world, x, y)
                at = ((x - low_x) * PIXELS_PER_CELL, (high_y - y - LOD_CELLS) * PIXELS_PER_CELL)
                diffuse.paste(image, at)
                coverage.paste(Image.fromarray(np.uint8(np.asarray(image).max(axis=2) > 8) * 255), at)
        return diffuse, coverage

    @staticmethod
    def placeholder_masks(diffuse, coverage, footprint):
        pixels = np.asarray(diffuse, dtype=float)
        height, width = pixels.shape[:2]
        padded = np.pad(pixels, ((1, 1), (1, 1), (0, 0)), mode='edge')
        neighbours = [padded[y:y + height, x:x + width] for y in range(3) for x in range(3)]
        mean = sum(neighbours) / 9
        variance = np.maximum(sum(p * p for p in neighbours) / 9 - mean * mean, 0).mean(axis=2)
        # Require available, non-black neighbours too, so missing data cannot seed a false flat edge.
        valid = np.asarray(coverage.filter(ImageFilter.MinFilter(3))) != 0
        flat = valid & (np.asarray(footprint) != 0) & (variance <= CITY_GROUND_MAX_VARIANCE)
        pending = list(pixel_components(flat))
        while pending:
            rows, columns = pending.pop()
            median = np.median(pixels[rows, columns], axis=0)
            keep = np.abs(pixels[rows, columns] - median).max(axis=1) <= CITY_GROUND_COLOUR_TOLERANCE
            if keep.all():
                mask = np.zeros(flat.shape, dtype=np.uint8)
                mask[rows, columns] = 255
                yield Image.fromarray(mask)
            else:
                trimmed = np.zeros(flat.shape, dtype=bool)
                trimmed[rows[keep], columns[keep]] = True
                # Trimming can disconnect a region and shift its median; recheck both until stable.
                pending.extend(pixel_components(trimmed))

    def mask(self, world, x, y):
        """Binary placeholder coverage on a native tile, before the inward feather."""
        mask = Image.new('L', (TILE_PIXELS, TILE_PIXELS))
        indices = {index for cx in range(x, x + LOD_CELLS) for cy in range(y, y + LOD_CELLS)
                   for index in self.component_cells.get((world, cx, cy), [])}
        for index in sorted(indices):
            _, region, low_x, high_y = self.components[index]
            left, top = (x - low_x) * PIXELS_PER_CELL, (high_y - y - LOD_CELLS) * PIXELS_PER_CELL
            mask = ImageChops.lighter(mask, region.crop((left, top, left + TILE_PIXELS, top + TILE_PIXELS)))
        return mask

    @lru_cache(maxsize=None)
    def texture_tones(self, world, x, y):
        # Many city LANDs only override heights/colours, or a subset of texture quadrants.
        # Use parent material underneath those omissions, rather than a bright default-palette square.
        land = self.lands.get((world, x, y), {})
        parent = self.terrain.parent(world)
        bases = {layer['quadrant'] for layer in land.get('layers', []) if layer['layer'] < 0}
        defaults = self.texture_tones(parent, x, y) if parent is not None and len(bases) < 4 else None
        return self.fallback.texture_tones(land, defaults)

    @lru_cache(maxsize=32)
    def render(self, index):
        cells, mask, low_x, high_y = self.components[index]
        world = cells[0][0]
        size = mask.size
        ground, relief = Image.new('RGB', size), Image.new('RGB', size)
        diffuse, coverage = self.diffuse_region(world, low_x, high_y, size)
        for key in cells:
            _, x, y = key
            land, child, _ = self.cells[key]
            image = render_land_cell(land, LAND_PALETTE, tones=self.texture_tones(child, x, y))
            at = ((x - low_x) * PIXELS_PER_CELL, (high_y - y - 1) * PIXELS_PER_CELL)
            ground.paste(image, at)
            relief.paste(render_land_cell(land, (128, 128, 128)), at)
        ground = Image.composite(ground, Image.new('RGB', size), mask)
        relief = Image.composite(relief, Image.new('RGB', size), mask)
        # Join material tones over a quarter cell, retaining two-pixel-smoothed height/VCLR relief.
        # Dividing the blurred textured render by its blurred neutral render removes broad lighting;
        # reapplying the detailed neutral render keeps LAND contours without material-sized squares.
        blurred_mask = np.asarray(mask.filter(ImageFilter.GaussianBlur(2)), dtype=float) / 255
        pixels = np.asarray(ground.filter(ImageFilter.GaussianBlur(16)), dtype=float)
        pixels /= np.maximum(np.asarray(relief.filter(ImageFilter.GaussianBlur(16)), dtype=float), 1)
        pixels *= np.asarray(relief.filter(ImageFilter.GaussianBlur(2)), dtype=float)
        pixels /= np.maximum(blurred_mask[:, :, None], 1 / 255)
        inside = np.asarray(mask) != 0
        # Nearby selected regions are not valid diffuse tone references for this region either.
        non_mask = coverage.copy()
        for other_cells, other_mask, other_low_x, other_high_y in self.components:
            if other_cells[0][0] == world:
                at = ((other_low_x - low_x) * PIXELS_PER_CELL, (high_y - other_high_y) * PIXELS_PER_CELL)
                non_mask.paste(0, at, other_mask)
        ring = ImageChops.multiply(mask.filter(ImageFilter.MaxFilter(17)), non_mask)
        outside = np.asarray(diffuse)[np.asarray(ring) != 0]
        if len(outside):
            samples = pixels[inside]
            # Match RGB mean and luminance contrast without independently amplifying material hues.
            weights = np.array([0.299, 0.587, 0.114])
            luminance, reference = pixels @ weights, outside @ weights
            mean, contrast = (samples @ weights).mean(), (samples @ weights).std()
            gain = reference.std() / contrast if contrast > 1 and reference.std() > 1 else 1
            chroma = pixels - luminance[:, :, None]
            chroma_mean = (samples - (samples @ weights)[:, None]).mean(axis=0)
            pixels = (outside.mean(axis=0) + ((luminance - mean) * gain)[:, :, None] +
                      0.25 * (chroma - chroma_mean))
        ground = Image.fromarray(np.uint8(np.clip(np.rint(pixels), 0, 255)))
        # Feather inward only. Shared cell edges stay fully painted; boundary pixels retain original diffuse.
        return ground, feather(mask), low_x, high_y

    def draw(self, image, world, x, y):
        cells = sorted(key for cx in range(x, x + LOD_CELLS) for cy in range(y, y + LOD_CELLS)
                       if (key := (world, cx, cy)) in self.cells)
        stats = {'city_ground_pixels': 0, 'city_ground_mask_pixels': self.mask(world, x, y).histogram()[255],
                 'city_ground_cells': [dict(cell=[cx, cy], world=self.cells[world, cx, cy][1],
                                          land_world=self.cells[world, cx, cy][2],
                                          source=self.cells[world, cx, cy][0]['source']) for _, cx, cy in cells]}
        if not cells:
            return image, stats
        original = image
        image = image.copy()
        for index in sorted({index for key in cells for index in self.component_cells[key]}):
            ground, alpha, low_x, high_y = self.render(index)
            left, top = (x - low_x) * PIXELS_PER_CELL, (high_y - y - LOD_CELLS) * PIXELS_PER_CELL
            box = (left, top, left + TILE_PIXELS, top + TILE_PIXELS)
            image.paste(ground.crop(box), (0, 0), alpha.crop(box))
        stats['city_ground_pixels'] = int(np.any(np.asarray(image) != np.asarray(original), axis=2).sum())
        return image, stats


class PaddingTiles:
    """Own-LOD padding (empty LOD or flat fill) showing the PADDING_SOURCES world's rendering at the same coordinates.

    Detection and the feather run on the world's whole canvas, so tile borders split neither. Pixels outside the
    padding, and every other world, stay byte-identical.
    """
    def __init__(self, ground, tiles, sources=PADDING_SOURCES):
        # ground(world, x, y): a tile's composed terrain before padding, buildings and the display curve.
        self.ground, self.sources, self.tiles = ground, sources, {}
        for world, x, y in tiles:
            self.tiles.setdefault(world, []).append((x, y))

    def canvas(self, world, low_x, high_y, size):
        """A world's composed tiles over a cell rectangle, north-up: RGB, coverage and each tile's empty LOD."""
        image, coverage, empty = Image.new('RGB', size), Image.new('L', size), Image.new('L', size)
        high_x, low_y = low_x + size[0] // PIXELS_PER_CELL, high_y - size[1] // PIXELS_PER_CELL
        for x, y in self.tiles.get(world, []):
            if low_x - LOD_CELLS < x < high_x and low_y - LOD_CELLS < y < high_y:
                tile = self.ground(world, x, y)
                at = ((x - low_x) * PIXELS_PER_CELL, (high_y - y - LOD_CELLS) * PIXELS_PER_CELL)
                image.paste(tile, at)
                coverage.paste(255, at + (at[0] + TILE_PIXELS, at[1] + TILE_PIXELS))
                empty.paste(empty_lod_mask(tile), at)
        return np.asarray(image), np.asarray(coverage) != 0, np.asarray(empty) != 0

    @lru_cache(maxsize=None)
    def fill(self, world):
        coords = self.tiles[world]
        low_x, high_y = min(x for x, _ in coords), max(y for _, y in coords) + LOD_CELLS
        size = ((max(x for x, _ in coords) + LOD_CELLS - low_x) * PIXELS_PER_CELL,
                (high_y - min(y for _, y in coords)) * PIXELS_PER_CELL)
        own, coverage, empty = self.canvas(world, low_x, high_y, size)
        under, available, _ = self.canvas(self.sources[world], low_x, high_y, size)
        padding = (padding_mask(own) | empty) & coverage
        terrain = coverage & ~padding
        # Feather inward from own terrain as city ground does, over that terrain spread into the padding, so the fill
        # colour never shows. Padding the source does not render stays.
        base = Image.fromarray(np.uint8(np.clip(np.rint(spread(own, terrain, CITY_GROUND_FEATHER)), 0, 255)))
        filled = Image.composite(Image.fromarray(under), base, feather(Image.fromarray(np.uint8(~terrain) * 255)))
        return filled, Image.fromarray(np.uint8(padding & available) * 255), low_x, high_y

    def draw(self, image, world, x, y):
        if world not in self.sources:
            return image, {}
        filled, mask, low_x, high_y = self.fill(world)
        left, top = (x - low_x) * PIXELS_PER_CELL, (high_y - y - LOD_CELLS) * PIXELS_PER_CELL
        mask = mask.crop((left, top, left + TILE_PIXELS, top + TILE_PIXELS))
        out = Image.composite(filled.crop((left, top, left + TILE_PIXELS, top + TILE_PIXELS)), image, mask)
        return out, {'padding_world': self.sources[world], 'padding_mask_pixels': mask.histogram()[255],
                     'padding_pixels': int(np.any(np.asarray(out) != np.asarray(image), axis=2).sum())}


class BuildingTiles:
    """Building footprints drawn over composed terrain, shared by every world in one LOD chain.

    A child world that uses its parent's LOD (the walled cities) puts its buildings on the parent's tiles at the same
    world coordinates, and its own fallback tiles show the parent's buildings around the city; a world with its own
    LOD keeps its own. Footprints are bucketed by cell, so a tile reads only its sixteen cells.
    """
    def __init__(self, buildings, records, models=None):
        self.records, self.buildings, self.cells = records, [], {}
        self.models, self.outlines, self.failures = models or {}, {}, {}
        self.fallback_skipped = 0
        for source in buildings:
            building = dict(source)
            name = 'meshes/' + source['model'].replace('\\', '/').casefold().removeprefix('meshes/')
            outline = self.outline(name)
            if outline is None:
                bounds, scale = source.get('bounds'), source.get('scale', 1)
                if bounds and max(bounds[3] - bounds[0], bounds[4] - bounds[1]) * scale > BUILDING_MAX_UNITS:
                    self.fallback_skipped += 1
                    continue
                building['fallback'] = True
            else:
                if not len(outline):
                    continue
                c, s = math.cos(source['rotation']), math.sin(source['rotation'])
                triangles = outline @ np.array([[c, -s], [s, c]]) * source['scale'] + source['position']
                building['triangles'] = triangles
                points = triangles.reshape(-1, 2)
                low, high = points.min(axis=0), points.max(axis=0)
                building['corners'] = (low, high)
                building['fallback'] = False
            index = len(self.buildings)
            self.buildings.append(building)
            xs, ys = zip(*building['corners'])
            root = self.root(building['world'])
            for cx in range(math.floor(min(xs) / CELL_UNITS), math.floor(max(xs) / CELL_UNITS) + 1):
                for cy in range(math.floor(min(ys) / CELL_UNITS), math.floor(max(ys) / CELL_UNITS) + 1):
                    self.cells.setdefault((root, cx, cy), []).append(index)

    def outline(self, name):
        """Parse/project each winning archive model once, including failures and empty visible meshes."""
        if name not in self.outlines:
            try:
                if not self.models.get(name):
                    raise FileNotFoundError(name)
                archive, path = self.models[name]
                self.outlines[name] = nif.triangles(archive.read(path))
            except (KeyError, IndexError, OSError, ValueError, struct.error, OverflowError, RuntimeError, zlib.error) as error:
                self.outlines[name] = None
                self.failures[name] = str(error)
        return self.outlines[name]

    def root(self, world):
        while self.records.get(world, {}).get('parent') is not None:
            world = self.records[world]['parent']
        return world

    def tile(self, world, x, y):
        """Footprints whose bounds overlap this tile, in the shared deterministic order."""
        root, found = self.root(world), set()
        for cx in range(x, x + LOD_CELLS):
            for cy in range(y, y + LOD_CELLS):
                found.update(self.cells.get((root, cx, cy), ()))
        left, top = x * CELL_UNITS, y * CELL_UNITS + TILE_UNITS
        out = []
        for index in sorted(found):
            xs, ys = zip(*self.buildings[index]['corners'])
            if min(xs) < left + TILE_UNITS and max(xs) > left and min(ys) < top and max(ys) > top - TILE_UNITS:
                out.append(self.buildings[index])
        return out

    @lru_cache(maxsize=128)
    def mask(self, world, x, y):
        """Centre-sampled union of projected triangles, with one pixel of neighbour margin."""
        size, units = TILE_PIXELS + 2, TILE_UNITS / TILE_PIXELS
        mask = np.zeros((size, size), dtype=np.uint8)
        left, top = x * CELL_UNITS, y * CELL_UNITS + TILE_UNITS
        # The extra cell margin includes roofs just outside this tile for edge relief.
        buildings = self.tile(world, x, y)
        found = {id(building): building for building in buildings}
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            for building in self.tile(world, nx, ny):
                found[id(building)] = building
        for building in found.values():
            if building['fallback']:
                corners = np.asarray(building['corners'])
                triangles = corners[[[0, 1, 2], [0, 2, 3]]]
            else:
                triangles = building['triangles']
            projected = (triangles - (left, top)) * (1 / units, -1 / units) + 1
            for triangle in projected:
                self.rasterize(mask, triangle)
        return Image.fromarray(mask)

    @staticmethod
    def rasterize(mask, triangle):
        """Fill a triangle's scanline intervals where pixel centres lie on or inside it."""
        (ax, ay), (bx, by), (cx, cy) = triangle
        if abs((bx - ax) * (cy - ay) - (by - ay) * (cx - ax)) < 1e-12:
            return
        height, width = mask.shape
        start = max(0, math.ceil(min(ay, by, cy) - 0.5))
        end = min(height - 1, math.floor(max(ay, by, cy) - 0.5))
        edges = ((ax, ay, bx, by), (bx, by, cx, cy), (cx, cy, ax, ay))
        for row in range(start, end + 1):
            y, crossings = row + 0.5, []
            for x1, y1, x2, y2 in edges:
                if min(y1, y2) <= y <= max(y1, y2):
                    if y1 == y2:
                        crossings.extend((x1, x2))
                    else:
                        crossings.append(x1 + (y - y1) * (x2 - x1) / (y2 - y1))
            if crossings:
                lo = max(0, math.ceil(min(crossings) - 0.5 - 1e-9))
                hi = min(width - 1, math.floor(max(crossings) - 0.5 + 1e-9))
                if lo <= hi:
                    mask[row, lo:hi + 1] = 255

    def draw(self, image, world, x, y, original=None):
        """Roofs in a dark, desaturated local terrain tone, lit north-west and shaded south-east; merged outlines."""
        buildings = self.tile(world, x, y)
        if not buildings:
            return image, 0
        size = TILE_PIXELS + 2
        mask = self.mask(world, x, y)
        inside = mask.crop((1, 1, size - 1, size - 1))
        lit = ImageChops.subtract(inside, ImageChops.multiply(mask.crop((1, 0, size - 1, size - 2)),
                                                              mask.crop((0, 1, size - 2, size - 1))))
        shaded = ImageChops.subtract(inside, ImageChops.multiply(mask.crop((1, 2, size - 1, size)),
                                                                 mask.crop((2, 1, size, size - 1))))
        # The local palette is the terrain blurred over about a quarter cell: half its saturation, 0.6x its luminance.
        local = image.filter(ImageFilter.BoxBlur(16))
        if original is not None:
            # City ground must not recolour roofs in untouched cells via the palette blur's neighbouring samples.
            changed = Image.fromarray(np.uint8(np.any(np.asarray(image) != np.asarray(original), axis=2)) * 255)
            local = Image.composite(local, original.filter(ImageFilter.BoxBlur(16)), changed)
        roof = Image.blend(local.convert('L').convert('RGB'), local, 0.5).point(lambda v: round(v * 0.6))
        image = Image.composite(roof, image, inside)
        image = Image.composite(roof.point(lambda v: round(v * 0.65)), image, shaded)
        image = Image.composite(roof.point(lambda v: min(255, round(v * 1.45))), image, lit)
        return image, len(buildings)


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


def inventory(data, archives=None, allow_empty=False, textures=None, models=None):
    # Stable explicit priority: base textures, then DLC, then CC/other archives in lexical order.
    # Mod load-order/loose DDS overrides require an explicit --archive list, in override order.
    paths = archives if archives is not None else sorted(data.glob('*.bsa'), key=lambda p: (
        0 if p.name.casefold().startswith('skyrim - ') else
        1 if p.name.casefold().startswith(('dawnguard', 'hearthfires', 'dragonborn')) else 2, p.name.casefold()))
    sources = {}
    for path in paths:
        archive = Bsa(path)
        for name in sorted(archive.files):
            if textures is not None and name in textures:
                textures[name] = archive, name
            if models is not None and name in models:
                models[name] = archive, name
            match = PATTERN.fullmatch(name)
            if not match:
                continue
            world, x, y = match[1], int(match[2]), int(match[3])
            sources[world, x, y] = archive, name
    if not sources and not allow_empty:
        raise ValueError(f'no level-4 terrain diffuse tiles in {data}')
    return sources


def build(data=DATA, output=ROOT / 'build/skyrim_minimap', archives=None, plugins=None):
    records, lands, buildings = worldspace_records(data, plugins, include_land=True, include_buildings=True)
    textures = {layer['texture']: None for land in lands.values() for layer in land['layers'] if layer['texture']}
    models = {'meshes/' + b['model'].replace('\\', '/').casefold().removeprefix('meshes/'): None for b in buildings}
    sources = inventory(data, archives, allow_empty=True, textures=textures, models=models)
    terrain = TerrainTiles(sources, records)
    fallback = LandTiles(terrain, lands, textures)
    cities = CityGroundTiles(terrain, lands, fallback)
    roofs = BuildingTiles(buildings, records, {name: value for name, value in models.items() if value})
    tiles = sorted(set(sources) | set(fallback.tiles))
    if not tiles:
        raise ValueError(f'no level-4 terrain diffuse tiles or exterior LAND in {data}')

    def ground(world, x, y):
        image = terrain.composite(world, x, y)[0] if (world, x, y) in sources else fallback.render(world, x, y)[0]
        return cities.draw(image, world, x, y)[0]

    padding = PaddingTiles(ground, tiles)
    output.mkdir(parents=True, exist_ok=True)
    worlds = {}
    for world, x, y in tiles:
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
    index = ['WOWBRIDGE_MINIMAP 1 64 16384']
    for world, x, y in tiles:
        if (world, x, y) in sources:
            archive, name = sources[world, x, y]
            _, source_hash = terrain.raw(world, x, y)
            composite, stats = terrain.composite(world, x, y)
            stats.update(source=archive.path.name + ':' + name, source_sha256=source_hash)
        else:
            composite, stats = fallback.render(world, x, y)
        original = composite
        composite, city_stats = cities.draw(composite, world, x, y)
        stats.update(city_stats)
        composite, padding_stats = padding.draw(composite, world, x, y)
        stats.update(padding_stats)
        composite, stats['building_footprints'] = roofs.draw(composite, world, x, y,
                                                           original if city_stats['city_ground_cells'] else None)
        stats['building_fallbacks'] = sum(b['fallback'] for b in roofs.tile(world, x, y))
        stats['building_pixels'] = roofs.mask(world, x, y).crop((1, 1, 257, 257)).histogram()[255] if stats['building_footprints'] else 0
        image = terrain_look(composite)
        encoded = blp2(image)
        filename = f'WB_{world}_{x}_{y}.blp'
        (output / filename).write_bytes(encoded)
        row = worlds[world]
        row['tiles'].append({'grid': [(x * CELL_UNITS - row['bounds_units'][0]) // TILE_UNITS, (y * CELL_UNITS - row['bounds_units'][1]) // TILE_UNITS], 'cell': [x, y], 'origin_units': [x * CELL_UNITS, y * CELL_UNITS],
                             'file': filename, 'wow_path': 'Interface\\Minimap\\' + filename,
                             'sha256': hashlib.sha256(encoded).hexdigest(),
                             **stats})
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
                'padding_sources': dict(padding.sources),
                'buildings': f'initially enabled STAT mesh silhouettes (OBND minimum {BUILDING_MIN_UNITS}; unreadable rectangle maximum {BUILDING_MAX_UNITS} units) of '
                             f'{BUILDING_MODEL.pattern} minus {BUILDING_EXCLUDE.pattern}, per LOD chain',
                'building_fallback_models': roofs.failures,
                'building_fallback_references': sum(b['fallback'] for b in roofs.buildings),
                'building_fallback_skipped': roofs.fallback_skipped,
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
    print(f'{len(tiles)} terrain tiles in {len(worlds)} worldspaces written to {output}', flush=True)
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=DATA)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/skyrim_minimap')
    parser.add_argument('--archive', type=Path, action='append', help='explicit archives, lowest priority first')
    parser.add_argument('--plugin', type=Path, action='append', help='explicit terrain plugins, lowest priority first (masters load automatically)')
    parser.add_argument('--overviews-only', action='store_true', help='add continent images to an existing generated atlas')
    args = parser.parse_args()
    if args.overviews_only:
        build_overviews(args.output)
    else:
        build(args.data, args.output, args.archive, args.plugin)
