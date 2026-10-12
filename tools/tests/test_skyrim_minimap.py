import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import lz4.frame
from PIL import Image

from tools import package, skyrim_minimap as minimap
from data import esm


def archive(path, world, coords, colour):
    """Small real v105 archive: embedded filenames, LZ4, RGB DDS terrain."""
    folder = f'textures\\terrain\\{world}'.encode() + b'\0'
    names = [f'{world}.4.{x}.{y}.dds'.encode() for x, y in coords]
    dds = io.BytesIO()
    image = colour if isinstance(colour, Image.Image) else Image.new('RGB', (256, 256), colour)
    image.save(dds, format='DDS')
    payloads = []
    for name in names:
        embedded = folder[:-1] + b'\\' + name
        payloads.append(bytes([len(embedded)]) + embedded + struct.pack('<I', len(dds.getvalue())) +
                        lz4.frame.compress(dds.getvalue()))
    names_blob = b''.join(name + b'\0' for name in names)
    at = 36 + 24 + 1 + len(folder) + len(names) * 16 + len(names_blob)
    rows = []
    for payload in payloads:
        rows.append(struct.pack('<QII', 0, len(payload), at))
        at += len(payload)
    path.write_bytes(struct.pack('<4s8I', b'BSA\0', 105, 36, 0x107, 1, len(names), len(folder), len(names_blob), 2) +
                     struct.pack('<QIIQ', 0, len(names), 0, 60 + len(names_blob)) +
                     bytes([len(folder)]) + folder + b''.join(rows) + names_blob + b''.join(payloads))


class MinimapBuildTest(unittest.TestCase):
    def test_parent_records_master_remapping_overrides_and_cycles(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            esm.write(root / 'base.esm', [esm.Record('WRLD', 1, subs=[('EDID', b'Parent\0')])], [], 'test', light=False)
            esm.write(root / 'other.esm', [esm.Record('WRLD', 1, subs=[('EDID', b'Other\0')])], [], 'test', light=False)
            esm.write(root / 'child.esp', [esm.Record('WRLD', 0x02000001, subs=[
                ('EDID', b'Child\0'), ('WNAM', struct.pack('<I', 0x01000001))])],
                ['other.esm', 'base.esm'], 'test', light=False)
            records = minimap.worldspace_records(root, [root / 'child.esp'])
            self.assertEqual(records['child']['parent'], 'parent')
            esm.write(root / 'override.esp', [esm.Record('WRLD', 0x02000001, subs=[
                ('EDID', b'Child\0'), ('WNAM', struct.pack('<I', 0x01000001))])],
                ['base.esm', 'other.esm', 'child.esp'], 'test', light=False)
            records = minimap.worldspace_records(root, [root / 'override.esp'])
            self.assertEqual(records['child']['parent'], 'other')
            self.assertEqual(records['child']['source_plugin'], 'override.esp')
            esm.write(root / 'cycle.esp', [esm.Record('WRLD', 1, subs=[
                ('EDID', b'Parent\0'), ('WNAM', struct.pack('<I', 0x02000001))])],
                ['base.esm', 'other.esm', 'child.esp'], 'test', light=False)
            with self.assertRaisesRegex(ValueError, 'cyclic WRLD'):
                minimap.worldspace_records(root, [root / 'cycle.esp'])

    def test_offset_parent_region_preserves_land_and_source_edges(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            # Negative child origin (-3,-1) overlaps all four parent tiles.
            colours = {(-4, -4): (20, 40, 60), (0, -4): (40, 60, 80),
                       (-4, 0): (60, 80, 100), (0, 0): (80, 100, 120)}
            for i, (coord, colour) in enumerate(colours.items()):
                archive(root / f'parent{i}.bsa', 'parent', [coord], colour)
            child = Image.new('RGB', (256, 256))
            child.paste((110, 70, 30), (80, 80, 180, 180))
            child.putpixel((10, 10), (8, 8, 8))
            child.putpixel((11, 10), (0, 9, 0))  # dark coloured terrain survives
            archive(root / 'child.bsa', 'child', [(-3, -1)], child)
            esm.write(root / 'worlds.esm', [
                esm.Record('WRLD', 1, subs=[('EDID', b'Parent\0')]),
                esm.Record('WRLD', 2, subs=[('EDID', b'Child\0'), ('WNAM', struct.pack('<I', 1))])],
                [], 'test', light=False)
            sources = minimap.inventory(root)
            records = minimap.worldspace_records(root)
            terrain = minimap.TerrainTiles(sources, records)
            image, stats = terrain.composite('child', -3, -1)
            self.assertEqual(image.getpixel((100, 100)), (110, 70, 30))
            self.assertEqual(image.getpixel((11, 10)), (0, 9, 0))
            for pixel, want in [((10, 10), colours[-4, 0]), ((191, 191), colours[-4, 0]),
                                ((192, 10), colours[0, 0]), ((10, 192), colours[-4, -4]),
                                ((192, 192), colours[0, -4])]:
                self.assertEqual(image.getpixel(pixel), want)
            self.assertEqual(stats['parent_pixels'], 256 * 256 - 100 * 100 - 1)
            self.assertEqual(stats['uncovered_empty_pixels'], 0)
            manifest = minimap.build(root, root / 'out')
            self.assertEqual(manifest['worldspaces']['child']['parent'], 'parent')
            self.assertEqual(manifest['worldspaces']['child']['tiles'][0]['parent_pixels'], stats['parent_pixels'])

    def test_missing_parent_tiles_chain_and_uncovered_padding(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive(root / 'grand.bsa', 'grand', [(0, 0)], (30, 50, 70))
            archive(root / 'middle.bsa', 'middle', [(0, 0)], (0, 0, 0))
            archive(root / 'child.bsa', 'child', [(0, 0), (4, 0)], (0, 0, 0))
            sources = minimap.inventory(root)
            records = {'child': {'parent': 'missing'}, 'missing': {'parent': 'middle'},
                       'middle': {'parent': 'grand'}, 'grand': {'parent': None}}
            terrain = minimap.TerrainTiles(sources, records)
            image, stats = terrain.composite('child', 0, 0)
            self.assertEqual(image.getpixel((0, 0)), (30, 50, 70))
            self.assertEqual(stats['parent_pixels'], 65536)
            image, stats = terrain.composite('child', 4, 0)
            self.assertEqual(image.getpixel((0, 0)), (0, 0, 0))
            self.assertEqual(stats['uncovered_empty_pixels'], 65536)
            records['child']['parent'] = None
            image, stats = minimap.TerrainTiles(sources, records).composite('child', 0, 0)
            self.assertEqual(image.getpixel((0, 0)), (0, 0, 0))

    def test_dark_or_green_source_without_black_padding_is_preserved(self):
        for colour in [(8, 8, 8), (0, 255, 0), (0, 9, 0)]:
            self.assertEqual(minimap.empty_lod_mask(Image.new('RGB', (256, 256), colour)).histogram()[255], 0)

    def test_flat_fill_padding_is_empty(self):
        # MarkarthWorld pads unbuilt land with one exact colour over part of a tile; terrain beside it is kept.
        tile = Image.effect_noise((256, 256), 40).point(lambda v: 60 + v // 2).convert('RGB')   # no black texels
        tile.paste((75, 73, 61), (0, 0, 256, 40))
        mask = minimap.empty_lod_mask(tile)
        self.assertEqual(mask.histogram()[255], 256 * 40)

    def test_real_bsa_offset_grid_override_determinism_and_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive(root / 'Skyrim - Textures.bsa', 'testworld', [(-3, -1), (1, -1)], (40, 80, 120))
            archive(root / 'ccOverride.bsa', 'testworld', [(1, -1)], (80, 100, 140))
            # _n normal maps never participate; tested by the exact matching expression.
            self.assertIsNone(minimap.PATTERN.fullmatch('textures/terrain/testworld/testworld.4.1.-1_n.dds'))
            output = root / 'out'
            manifest = minimap.build(root, output)
            row = manifest['worldspaces']['testworld']
            self.assertEqual(row['bounds_units'], [-12288, -4096, 20480, 12288])
            self.assertEqual([tile['grid'] for tile in row['tiles']], [[0, 0], [1, 0]])
            self.assertTrue(row['tiles'][1]['source'].startswith('ccOverride.bsa:'))
            first = {p.name: p.read_bytes() for p in output.iterdir()}
            minimap.build(root, output)
            self.assertEqual(first, {p.name: p.read_bytes() for p in output.iterdir()})
            raw = (output / row['tiles'][0]['file']).read_bytes()
            self.assertEqual(raw[8:12], bytes([2, 0, 0, 1]))   # DXT1, no alpha, mips
            offset, size = struct.unpack_from('<I', raw, 20)[0], struct.unpack_from('<I', raw, 84)[0]
            self.assertEqual(size, 256 * 256 // 2)
            header = io.BytesIO()
            Image.new('RGB', (256, 256)).save(header, 'DDS', pixel_format='DXT1')
            image = Image.open(io.BytesIO(header.getvalue()[:128] + raw[offset:offset + size])).convert('RGB')
            want = minimap.terrain_look(Image.new('RGB', (1, 1), (40, 80, 120))).getpixel((0, 0))
            self.assertTrue(all(abs(a - b) <= 8 for a, b in zip(image.getpixel((10, 10)), want)))   # DXT1's 5:6:5 colour
            with patch.object(package, 'MINIMAP_SOURCE', output):
                package.pack_minimap(root / 'package')
                target = root / 'package' / package.MINIMAP_DESTINATION
                self.assertEqual({p.name for p in target.iterdir()}, {'manifest.json', 'index.tsv', row['overview']['file']} | {t['file'] for t in row['tiles']})
                overview = output / row['overview']['file']
                self.assertEqual(hashlib.sha256(overview.read_bytes()).hexdigest(), row['overview']['sha256'])
                overview.write_bytes(b'corrupt')
                with self.assertRaisesRegex(ValueError, 'changed minimap asset'):
                    package.pack_minimap(root / 'bad-overview-package')
                self.assertFalse((root / 'bad-overview-package').exists())
                minimap.build_overviews(output)
                tile = output / row['tiles'][0]['file']
                tile.write_bytes(b'corrupt')
                with self.assertRaisesRegex(ValueError, 'changed minimap asset'):
                    package.pack_minimap(root / 'bad-package')
                self.assertFalse((root / 'bad-package').exists())

    def test_missing_or_inconsistent_terrain_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(ValueError, 'no level-4'):
                minimap.build(root, root / 'out')
            archive(root / 'bad.bsa', 'testworld', [(0, 0), (1, 0)], (20, 30, 40))
            with self.assertRaisesRegex(ValueError, 'inconsistent'):
                minimap.build(root, root / 'out')

    def test_overviews_preserve_north_east_sparse_alpha_and_upgrade(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive(root / 'south.bsa', 'testworld', [(-4, -4)], (200, 50, 20))
            archive(root / 'north.bsa', 'testworld', [(0, 0)], (20, 200, 50))
            output = root / 'out'
            manifest = minimap.build(root, output)
            row = manifest['worldspaces']['testworld']
            raw = (output / row['overview']['file']).read_bytes()
            self.assertEqual(struct.unpack_from('<II', raw, 12), (64, 64))
            offset = struct.unpack_from('<I', raw, 20)[0]
            image = Image.frombytes('RGBA', (64, 64), raw[offset:offset + 64 * 64 * 4], 'raw', 'BGRA')
            self.assertGreater(image.getpixel((48, 16))[1], image.getpixel((16, 48))[1])
            self.assertGreater(image.getpixel((16, 48))[0], image.getpixel((48, 16))[0])
            self.assertEqual(image.getpixel((16, 16))[3], 0)
            self.assertEqual(image.getpixel((48, 48))[3], 0)
            self.assertEqual(image.getpixel((48, 16))[3], 255)
            del row['overview']
            (output / 'manifest.json').write_text(json.dumps(manifest))
            minimap.build_overviews(output)
            self.assertEqual(raw, (output / 'WB_testworld_overview.blp').read_bytes())
            self.assertIn('overview', json.loads((output / 'manifest.json').read_text())['worldspaces']['testworld'])

    def test_generated_game_atlas_hashes_and_index(self):
        output = minimap.ROOT / 'build/skyrim_minimap'
        manifest = json.loads((output / 'manifest.json').read_text())
        self.assertIn('tamriel', manifest['worldspaces'])
        self.assertIn('dlc2solstheimworld', manifest['worldspaces'])
        expected = ['WOWBRIDGE_MINIMAP 1 64 16384']
        for world, row in sorted(manifest['worldspaces'].items()):
            for tile in sorted(row['tiles'], key=lambda t: t['cell']):
                raw = (output / tile['file']).read_bytes()
                self.assertEqual(hashlib.sha256(raw).hexdigest(), tile['sha256'])
                self.assertEqual(struct.unpack_from('<II', raw, 12), (256, 256))
                x, y = tile['cell']
                self.assertEqual(tile['origin_units'], [x * 4096, y * 4096])
                expected.append(f"{world}\t{x}\t{y}\t{tile['file']}")
        self.assertEqual((output / 'index.tsv').read_text(), '\n'.join(expected) + '\n')


if __name__ == '__main__':
    unittest.main()
