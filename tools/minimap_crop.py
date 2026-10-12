#!/usr/bin/env python3
"""Render a north-up world-position crop directly from the generated BLP tiles.

One output pixel is one native terrain texel (64 Skyrim units / one yard).
Parents fill missing tile rectangles as in wowcore; no overview PNG is resized.
"""
import argparse
import io
import json
import math
from pathlib import Path
import struct
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.skyrim_minimap import DATA, ROOT, worldspace_records


def decode_tile(path):
    return decode_blp(path.read_bytes(), str(path))


def decode_blp(raw, name='<memory>'):
    if raw[:4] != b'BLP2':
        raise ValueError(f'not BLP2: {name}')
    width, height = struct.unpack_from('<II', raw, 12)
    offset, size = struct.unpack_from('<I', raw, 20)[0], struct.unpack_from('<I', raw, 84)[0]
    payload = raw[offset:offset + size]
    if (width, height) != (256, 256) or len(payload) != size:
        raise ValueError(f'invalid terrain BLP: {name}')
    if raw[8:11] == bytes([2, 0, 0]):  # DXT1 opaque
        header = io.BytesIO()
        Image.new('RGB', (width, height)).save(header, 'DDS', pixel_format='DXT1')
        return Image.open(io.BytesIO(header.getvalue()[:128] + payload)).convert('RGB')
    if raw[8] == 3:  # previous uncompressed BGRA atlas
        return Image.frombytes('RGBA', (width, height), payload, 'raw', 'BGRA').convert('RGB')
    raise ValueError(f'unsupported terrain BLP encoding: {name}')


def render(atlas, world, x, y, size, records):
    """X/Y in Skyrim units; nearest native-texel-aligned centre, north up."""
    manifest = json.loads((atlas / 'manifest.json').read_text())
    left, top = round(x / 64) - size // 2, round(y / 64) + size // 2
    image = Image.new('RGB', (size, size), '#121719')
    chain, current = [], world.lower()
    while current:
        if current in chain:
            raise ValueError(f'cyclic worldspace parents: {world}')
        chain.append(current)
        current = records.get(current, {}).get('parent')
    for layer in reversed(chain):
        for tile in manifest['worldspaces'].get(layer, {}).get('tiles', []):
            tx, ty = tile['cell']
            px, py = tx * 64 - left, top - (ty + 4) * 64
            if px >= size or py >= size or px + 256 <= 0 or py + 256 <= 0:
                continue
            path = atlas / tile['file']
            if path.is_file():
                image.paste(decode_tile(path), (px, py))
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--atlas', type=Path, default=ROOT / 'build/skyrim_minimap')
    parser.add_argument('--before', type=Path, help='optional original atlas for a side-by-side comparison')
    parser.add_argument('--data', type=Path, default=DATA)
    parser.add_argument('--plugin', type=Path, action='append')
    parser.add_argument('--world', required=True)
    position = parser.add_mutually_exclusive_group(required=True)
    position.add_argument('--units', type=float, nargs=2, metavar=('X', 'Y'))
    position.add_argument('--cell', type=int, nargs=2, metavar=('X', 'Y'), help='centre of the named tour cell')
    parser.add_argument('--radius-yards', type=int, default=200)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.radius_yards <= 0 or args.radius_yards > 4096:
        parser.error('radius must be in 1..4096 yards')
    x, y = args.units if args.units else [(v + 0.5) * 4096 for v in args.cell]
    if not all(math.isfinite(v) for v in (x, y)):
        parser.error('position must be finite')
    size = args.radius_yards * 2
    records = worldspace_records(args.data, args.plugin)
    after = render(args.atlas, args.world, x, y, size, records)
    if args.before:
        before = render(args.before, args.world, x, y, size, records)
        result = Image.new('RGB', (size * 2 + 8, size + 32), '#121719')
        result.paste(before, (0, 32))
        result.paste(after, (size + 8, 32))
        draw = ImageDraw.Draw(result)
        draw.text((8, 8), f'Before: {args.world} ({x:g}, {y:g})', fill='white')
        draw.text((size + 16, 8), 'After: native texels, north up', fill='white')
    else:
        result = after
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result.save(args.output)
    print(args.output)


if __name__ == '__main__':
    main()
