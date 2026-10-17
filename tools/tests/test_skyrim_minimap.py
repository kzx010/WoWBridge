import hashlib
import io
import json
import math
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

import lz4.frame
from PIL import Image, ImageChops

from tools import nif, package, skyrim_minimap as minimap
from data import esm


def archive(path, world, coords, colour):
    """Small real v105 archive: embedded filenames, LZ4, RGB DDS terrain."""
    dds_archive(path, f'textures\\terrain\\{world}', {f'{world}.4.{x}.{y}.dds': colour for x, y in coords})


def dds_archive(path, folder, images):
    files = {}
    for name, colour in images.items():
        dds = io.BytesIO()
        image = colour if isinstance(colour, Image.Image) else Image.new('RGB', (256, 256), colour)
        image.save(dds, format='DDS')
        files[name] = dds.getvalue()
    file_archive(path, folder, files)


def file_archive(path, folder, files):
    folder = folder.encode() + b'\0'
    names = [name.encode() for name in files]
    payloads = []
    for name, raw in zip(names, files.values()):
        embedded = folder[:-1] + b'\\' + name
        payloads.append(bytes([len(embedded)]) + embedded + struct.pack('<I', len(raw)) + lz4.frame.compress(raw))
    names_blob = b''.join(name + b'\0' for name in names)
    at = 36 + 24 + 1 + len(folder) + len(names) * 16 + len(names_blob)
    rows = []
    for payload in payloads:
        rows.append(struct.pack('<QII', 0, len(payload), at))
        at += len(payload)
    path.write_bytes(struct.pack('<4s8I', b'BSA\0', 105, 36, 0x107, 1, len(names), len(folder), len(names_blob), 2) +
                     struct.pack('<QIIQ', 0, len(names), 0, 60 + len(names_blob)) +
                     bytes([len(folder)]) + folder + b''.join(rows) + names_blob + b''.join(payloads))


def land_plugin(path, world, cell, land, masters=(), records=()):
    """An exterior CELL with compressed LAND in its actual temporary children."""
    def group(label, kind, body):
        return b'GRUP' + struct.pack('<I4siHHI', 24 + len(body), label, kind, 0, 0, 0) + body

    esm.write(path, list(records), list(masters), 'test', light=False)
    raw = esm._record(land)
    body = struct.pack('<I', len(raw) - 24) + zlib.compress(raw[24:])
    compressed = raw[:4] + struct.pack('<II', len(body), land.flags | esm.COMPRESSED) + raw[12:24] + body
    children = group(struct.pack('<I', cell.form), 6,
                     group(struct.pack('<I', cell.form), 9, compressed))
    block = group(bytes(4), 4, group(bytes(4), 5, esm._record(cell) + children))
    world_children = group(struct.pack('<I', world.form), 1, block)
    path.write_bytes(path.read_bytes() + group(b'WRLD', 0, esm._record(world) + world_children))



def ref_plugin(path, worlds, masters=(), records=()):
    """Exterior references in actual cell children: [(WRLD, CELL, temporary refs, persistent refs)].

    The first temporary reference is compressed; persistent ones sit in the cell's persistent children group.
    """
    def group(label, kind, body):
        return b'GRUP' + struct.pack('<I4siHHI', 24 + len(body), label, kind, 0, 0, 0) + body

    def compressed(record):
        raw = esm._record(record)
        body = struct.pack('<I', len(raw) - 24) + zlib.compress(raw[24:])
        return raw[:4] + struct.pack('<II', len(body), record.flags | esm.COMPRESSED) + raw[12:24] + body

    esm.write(path, list(records), list(masters), 'test', light=False)
    body = b''
    for world, cell, temporary, persistent in worlds:
        label = struct.pack('<I', cell.form)
        children = group(label, 6, (group(label, 8, b''.join(map(esm._record, persistent))) if persistent else b'') +
                         group(label, 9, b''.join(compressed(r) if i == 0 else esm._record(r)
                                                  for i, r in enumerate(temporary))))
        block = group(bytes(4), 4, group(bytes(4), 5, esm._record(cell) + children))
        body += esm._record(world) + group(struct.pack('<I', world.form), 1, block)
    path.write_bytes(path.read_bytes() + group(b'WRLD', 0, body))


def static(form, model, bounds):
    return esm.Record('STAT', form, subs=[('EDID', b'Test\0'), ('OBND', struct.pack('<6h', *bounds)),
                                          ('MODL', model.encode() + b'\0')])


def reference(form, base, x, y, rotation=0.0, scale=None, flags=0, parent=None, tilt=(0.0, 0.0)):
    subs = [('NAME', struct.pack('<I', base))]
    if parent is not None:
        subs.append(('XESP', struct.pack('<IB3x', *parent)))
    if scale is not None:
        subs.append(('XSCL', struct.pack('<f', scale)))
    return esm.Record('REFR', form, flags, subs + [('DATA', struct.pack('<6f', x, y, 0, *tilt, rotation))])


def city_ground_fixture(root):
    """Two adjoining child cells across a tile edge; only the western cell has child LAND."""
    def land(form, axis, reverse=False):
        heights = [[6 * min(v, 32 - v) for v in (range(33) if axis == 'x' else [y] * 33)] for y in range(33)]
        deltas = [heights[y][x] - (heights[y][x - 1] if x else heights[y - 1][0] if y else 0)
                  for y in range(33) for x in range(33)]
        return esm.Record('LAND', form, subs=[('VHGT', struct.pack('<f1089b', 100, *deltas) + bytes(3))] + [
            ('BTXT', struct.pack('<IBxh', 0x10 + 2 * ((q + reverse) % 2), q, -1)) for q in range(4)])

    parent = esm.Record('WRLD', 0x3c, subs=[('EDID', b'Tamriel\0')])
    child = esm.Record('WRLD', 0x01000001, subs=[('EDID', b'City\0'), ('WNAM', struct.pack('<I', 0x3c)),
                                               ('PNAM', struct.pack('<H', 2)),
                                               ('NAM0', struct.pack('<2f', -40960, -40960)),
                                               ('NAM9', struct.pack('<2f', 40960, 40960))])
    definitions = [esm.Record('LTEX', form, subs=[('TNAM', struct.pack('<I', form + 1))]) for form in (0x10, 0x12)] + [
        esm.Record('TXST', form, subs=[('TX00', f'landscape\\{name}.dds\0'.encode())])
        for form, name in [(0x11, 'grass'), (0x13, 'stone')]]
    house = static(0x50, 'Architecture\\Test\\House.nif', (-256, -256, 0, 256, 256, 400))
    ref_plugin(root / 'base.esm', [(parent, esm.Record('CELL', 0x20 + x, subs=[('XCLC', struct.pack('<ii', x, 1))]),
                                  [land(0x30 + x, 'y')], []) for x in (2, 3, 4, 5, 8)], records=definitions + [house])
    ref_plugin(root / 'child.esp', [
        (child, esm.Record('CELL', 0x01000002, subs=[('XCLC', struct.pack('<ii', 3, 1))]),
         [land(0x01000004, 'x', True), reference(0x01000005, 0x50, 3.5 * 4096, 1.5 * 4096)], []),
        (child, esm.Record('CELL', 0x01000003, subs=[('XCLC', struct.pack('<ii', 4, 1))]), [], []),
        (child, esm.Record('CELL', 0x01000006, flags=0x400, subs=[('XCLC', struct.pack('<ii', 0, 0))]), [], [])],
        masters=['base.esm'])
    dds_archive(root / 'textures.bsa', 'textures\\landscape',
                {'grass.dds': (85, 100, 60), 'stone.dds': (115, 105, 95)})
    for x in (0, 4, 8):
        image = Image.new('RGB', (256, 256))
        image.putdata([(90 + (px + py) % 31, 100 + (px + 2 * py) % 29, 80 + (2 * px + py) % 27)
                       for py in range(256) for px in range(256)])
        # The city's LOD placeholder is flat and deliberately unlike the outside ring.
        if x in (0, 4):
            left = 192 if x == 0 else 0
            image.paste((160, 140, 110), (left, 128, left + 64, 192))
        archive(root / f'terrain{x}.bsa', 'tamriel', [(x, 0)], image)
    records, lands, buildings = minimap.worldspace_records(root, [root / 'child.esp'], True, True)
    textures = {layer['texture']: None for land in lands.values() for layer in land['layers']}
    terrain = minimap.TerrainTiles(minimap.inventory(root, textures=textures), records)
    fallback = minimap.LandTiles(terrain, lands, textures)
    return records, lands, buildings, terrain, fallback


def padding_fixture(root):
    """MarkarthWorld's two tiles one cell off Tamriel's grid: flat fill across their border, black padding, terrain."""
    np = minimap.np
    py, px = np.mgrid[0:256, 0:256]

    def tamriel(x, y):
        # A distinct colour at every world texel, far from the fill colour.
        wx, wy = x * 64 + px, y * 64 + 255 - py
        return Image.fromarray(np.uint8(np.stack([100 + (wx * 7 + wy * 3) % 50, 120 + (wx * 5 + wy * 11) % 40,
                                                  90 + (wx + 2 * wy) % 30], axis=2)))

    own = np.stack([140 + (px * 3 + py) % 40, 90 + (px + py * 5) % 30, 60 + (px * 2 + py * 3) % 20], axis=2)
    left, right = own.copy(), own.copy()
    # As MarkarthWorld's LOD pads: four levels of grey noise, blended edge texels eight levels out.
    noise = np.array([75, 73, 61]) + ((px * 7 + py * 3) % 6 - 4)[:, :, None]
    left[64:192, 192:], right[64:192, :64] = noise[64:192, 192:], noise[64:192, :64]
    left[64:192:2, 192] = (69, 67, 55)
    right[100:150, 200:250] = noise[100:150, 200:250]
    left[:64, :64] = 0
    left[200:210, 100:110] = (75, 73, 61)   # a small natural patch of the fill colour stays
    for x in (-8, -4, 0):
        for y in (-4, 0):
            archive(root / f'tamriel{x}{y}.bsa', 'tamriel', [(x, y)], tamriel(x, y))
    for x, image in [(-7, left), (-3, right)]:
        archive(root / f'markarth{x}.bsa', 'markarthworld', [(x, -3)], Image.fromarray(np.uint8(image)))
    esm.write(root / 'worlds.esm', [
        esm.Record('WRLD', 1, subs=[('EDID', b'Tamriel\0')]),
        esm.Record('WRLD', 2, subs=[('EDID', b'MarkarthWorld\0'), ('WNAM', struct.pack('<I', 1)),
                                    ('PNAM', struct.pack('<H', 0x44))])], [], 'test', light=False)
    records = minimap.worldspace_records(root)
    terrain = minimap.TerrainTiles(minimap.inventory(root), records)
    return terrain, (lambda world, x, y: terrain.composite(world, x, y)[0])


def nif_file(blocks):
    """Minimal SSE header, sized blocks, string table and root footer, independent of the reader."""
    def string(value):
        return struct.pack('<I', len(value)) + value

    types = list(dict.fromkeys(kind for kind, _ in blocks))
    return (b'Gamebryo File Format, Version 20.2.0.7\n' + struct.pack('<IBIII', 0x14020007, 1, 12, len(blocks), 100) +
            b'\x01\0' * 3 + struct.pack('<H', len(types)) + b''.join(string(t.encode()) for t in types) +
            b''.join(struct.pack('<H', types.index(t)) for t, _ in blocks) +
            b''.join(struct.pack('<I', len(raw)) for _, raw in blocks) +
            struct.pack('<III', 1, 4, 4) + b'Test' + bytes(4) + b''.join(raw for _, raw in blocks) +
            struct.pack('<Ii', 1, 0))


def nif_av(translation=(0, 0, 0), rotation=(1, 0, 0, 0, 1, 0, 0, 0, 1), scale=1, flags=14):
    return (struct.pack('<iIiI', 0, 0, -1, flags) + struct.pack('<3f9ffi', *translation, *rotation, scale, -1))


def nif_shape(vertices, triangles, full=True, alpha=-1, shader=-1, flags=14):
    stride = 16 if full else 8
    descriptor = stride // 4 | ((1 | (0x400 if full else 0)) << 44)
    packed = b''.join(struct.pack('<4f' if full else '<4e', *v, 0) for v in vertices)
    indices = b''.join(struct.pack('<3H', *t) for t in triangles)
    return (nif_av(flags=flags) + bytes(16) + struct.pack('<iiiQHHI', -1, shader, alpha, descriptor,
                                                       len(triangles), len(vertices), len(packed + indices)) + packed + indices)


def l_mesh(full=True, rotation=(1, 0, 0, 0, 1, 0, 0, 0, 1), translation=(0, 0, 0), scale=1,
           shape_type='BSTriShape', branch_type='NiNode', flags=14, alpha_flags=None):
    # Two rectangles make an L: 8x8 pixels with the north-east 6x6 pixels open.
    vertices = [(0, 0, 0), (512, 0, 0), (512, 128, 0), (128, 128, 0), (128, 512, 0), (0, 512, 0)]
    triangles = [(0, 1, 2), (0, 2, 3), (0, 3, 4), (0, 4, 5)]
    blocks = [('NiNode', nif_av() + struct.pack('<IiI', 1, 1, 0)),
              (branch_type, nif_av(translation, rotation, scale) + struct.pack('<IiI', 1, 2, 0)),
              (shape_type, nif_shape(vertices, triangles, full, alpha=3 if alpha_flags is not None else -1,
                                     flags=flags))]
    if alpha_flags is not None:
        blocks.append(('NiAlphaProperty', struct.pack('<iIiHB', -1, 0, -1, alpha_flags, 128)))
    return nif_file(blocks)


class MinimapBuildTest(unittest.TestCase):
    def test_nif_l_silhouette_full_half_subindex_and_model_cache(self):
        for full, kind in [(True, 'BSTriShape'), (False, 'BSTriShape'), (True, 'BSSubIndexTriShape')]:
            with self.subTest(full=full, kind=kind), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                archive(root / 'terrain.bsa', 'tamriel', [(0, 0)], (120, 140, 100))
                file_archive(root / 'meshes.bsa', 'meshes\\architecture\\test',
                             {'house.nif': l_mesh(full=full, shape_type=kind)})
                world = esm.Record('WRLD', 0x3c, subs=[('EDID', b'Tamriel\0')])
                cell = esm.Record('CELL', 0x20, subs=[('XCLC', bytes(8))])
                ref_plugin(root / 'Skyrim.esm', [(world, cell, [reference(0x30, 0x10, 4096, 4096),
                                                             reference(0x31, 0x10, 8192, 4096)], [])],
                           records=[static(0x10, 'Architecture\\Test\\House.nif', (0, 0, 0, 512, 512, 400))])
                records, buildings = minimap.worldspace_records(root, include_buildings=True)
                name = 'meshes/architecture/test/house.nif'
                models = {name: None}
                minimap.inventory(root, models=models)
                bsa = models[name][0]
                with patch.object(bsa, 'read', wraps=bsa.read) as read:
                    roofs = minimap.BuildingTiles(buildings, records, models)
                    self.assertEqual(read.call_count, 1)
                mask = roofs.mask('tamriel', 0, 0).crop((1, 1, 257, 257))
                self.assertEqual(mask.histogram()[255], 56)  # 2 * (64 - 36), not 128
                self.assertEqual(mask.getpixel((65, 185)), 255)
                self.assertEqual(mask.getpixel((70, 185)), 0)
                self.assertEqual(mask.getpixel((70, 191)), 255)
                manifest = minimap.build(root, root / 'out')
                tile = manifest['worldspaces']['tamriel']['tiles'][0]
                self.assertEqual((tile['building_pixels'], tile['building_fallbacks']), (56, 0))

    def test_nif_node_and_reference_transform_composition(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            # NIF matrices rotate counter-clockwise. Then REFR turns clockwise;
            # translations and both scales must compose in their respective spaces.
            raw = l_mesh(rotation=(0, -1, 0, 1, 0, 0, 0, 0, 1), translation=(128, 256, 0), scale=2)
            file_archive(root / 'meshes.bsa', 'meshes\\architecture\\test', {'house.nif': raw})
            name = 'meshes/architecture/test/house.nif'
            models = {name: None}
            minimap.inventory(root, models=models, allow_empty=True)
            building = {'world': 'tamriel', 'model': name, 'position': (4096, 4096),
                        'rotation': math.pi / 2, 'scale': 0.5, 'corners': ((0, 0), (512, 512))}
            roofs = minimap.BuildingTiles([building], {}, models)
            mask = roofs.mask('tamriel', 0, 0).crop((1, 1, 257, 257))
            # Net orientation is unchanged, with origin at (4224,4032): column 66, row 193.
            self.assertEqual(mask.getbbox(), (66, 185, 74, 193))
            self.assertEqual(mask.histogram()[255], 28)
            self.assertEqual(mask.getpixel((67, 185)), 255)
            self.assertEqual(mask.getpixel((73, 185)), 0)
            self.assertEqual(mask.getpixel((73, 192)), 255)

    def test_nif_visibility_and_alpha_tested_roofs(self):
        for raw in [l_mesh(branch_type='BSOrderedNode'), l_mesh(flags=15), l_mesh(alpha_flags=1)]:
            self.assertEqual(len(nif.triangles(raw)), 0)
        # Alpha testing alone is used on real thatch roofs and must retain them.
        self.assertEqual(len(nif.triangles(l_mesh(alpha_flags=0x12ec))), 4)
        with self.assertRaises(ValueError):
            nif.triangles(l_mesh()[:-20])

    def test_legacy_nif_tri_shape_data(self):
        vertices = [(0, 0, 0), (512, 0, 0), (0, 512, 0)]
        geometry = (struct.pack('<IHBBB', 0, 3, 0, 0, 1) +
                    b''.join(struct.pack('<3f', *v) for v in vertices) +
                    struct.pack('<HIB', 0, 0, 0) + bytes(16) +
                    struct.pack('<BHiHIB3H', 0, 0, -1, 1, 3, 1, 0, 1, 2))
        shape = nif_av(translation=(64, 128, 0), scale=2) + struct.pack('<iiIiBii', 2, -1, 0, -1, 0, -1, -1)
        raw = nif_file([('NiNode', nif_av() + struct.pack('<IiI', 1, 1, 0)),
                        ('NiTriShape', shape), ('NiTriShapeData', geometry)])
        self.assertEqual(nif.triangles(raw).tolist(), [[[64, 128], [1088, 128], [64, 1152]]])

    def test_nif_materials_and_large_composites(self):
        vertices = [(0, 0, 0), (5120, 0, 0), (0, 5120, 0)]

        def model(texture, flags=0):
            shader = (struct.pack('<IiIiII', 0, -1, 0, -1, flags, 0) + bytes(16) +
                      struct.pack('<i3ffIf', 3, 0, 0, 0, 1, 0, 1))
            path = texture.encode()
            return nif_file([('NiNode', nif_av() + struct.pack('<IiI', 1, 1, 0)),
                             ('BSTriShape', nif_shape(vertices, [(0, 1, 2)], shader=2)),
                             ('BSLightingShaderProperty', shader),
                             ('BSShaderTextureSet', struct.pack('<II', 1, len(path)) + path)])

        self.assertEqual(len(nif.triangles(model('textures/roof.dds'))), 1)
        for raw in [model('textures/architecture/street01.dds'), model('textures/roof.dds', 1 << 26)]:
            self.assertEqual(len(nif.triangles(raw)), 0)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            name = 'meshes/architecture/test/castle.nif'
            file_archive(root / 'meshes.bsa', 'meshes\\architecture\\test', {'castle.nif': model('textures/roof.dds')})
            models = {name: None}
            minimap.inventory(root, models=models, allow_empty=True)
            building = {'world': 'tamriel', 'model': name, 'position': (4096, 4096),
                        'rotation': 0, 'scale': 1, 'bounds': (0, 0, 0, 5120, 5120, 400),
                        'corners': ((4096, 4096), (9216, 4096), (9216, 9216), (4096, 9216))}
            roofs = minimap.BuildingTiles([building], {}, models)
            self.assertFalse(roofs.buildings[0]['fallback'])
            self.assertGreater(roofs.mask('tamriel', 0, 0).histogram()[255], 3000)
            unreadable = minimap.BuildingTiles([building], {})
            self.assertEqual((len(unreadable.buildings), unreadable.fallback_skipped), (0, 1))

    def test_unreadable_nif_rectangle_fallback_and_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive(root / 'terrain.bsa', 'tamriel', [(0, 0)], (120, 140, 100))
            file_archive(root / 'meshes.bsa', 'meshes\\architecture\\test', {'house.nif': b'unreadable'})
            world = esm.Record('WRLD', 0x3c, subs=[('EDID', b'Tamriel\0')])
            cell = esm.Record('CELL', 0x20, subs=[('XCLC', bytes(8))])
            ref_plugin(root / 'Skyrim.esm', [(world, cell, [reference(0x30, 0x10, 4096, 4096)], [])],
                       records=[static(0x10, 'Architecture\\Test\\House.nif', (0, 0, 0, 512, 512, 400))])
            manifest = minimap.build(root, root / 'out')
            tile = manifest['worldspaces']['tamriel']['tiles'][0]
            self.assertEqual((tile['building_pixels'], tile['building_fallbacks']), (64, 1))
            self.assertEqual(manifest['building_fallback_references'], 1)
            self.assertEqual(manifest['building_fallback_models'],
                             {'meshes/architecture/test/house.nif': 'truncated NIF'})

    def test_land_texture_colour_master_remapping_overrides_and_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            colour = (80, 60, 110)
            esm.write(root / 'base.esm', [
                esm.Record('LTEX', 1, subs=[('TNAM', struct.pack('<I', 2))]),
                esm.Record('TXST', 2, subs=[('TX00', b'Landscape\\Old.dds\0')])], [], 'test', light=False)
            world = esm.Record('WRLD', 0x01000001, subs=[('EDID', b'Cairn\0')])
            cell = esm.Record('CELL', 0x01000002, subs=[('XCLC', struct.pack('<ii', 0, 0))])
            land = esm.Record('LAND', 0x01000003, subs=[
                ('VHGT', struct.pack('<f', 100) + bytes(1092)),
                ('VCLR', bytes([128, 255, 64]) * 1089)] + [
                ('BTXT', struct.pack('<IBxh', 1, quadrant, -1)) for quadrant in range(4)])
            land_plugin(root / 'child.esp', world, cell, land, ['base.esm'])
            esm.write(root / 'unrelated.esm', [], [], 'test', light=False)
            # Override the TXST via a different master index and a mixed-case,
            # already-prefixed path; the LAND still references the base LTEX.
            override = esm.Record('TXST', 0x01000002, subs=[('TX00', b'Textures\\Landscape\\Purple.dds\0')])
            esm.write(root / 'override.esp', [override], ['unrelated.esm', 'base.esm', 'child.esp'], 'test', light=False)
            dds_archive(root / 'Skyrim - Textures.bsa', 'textures\\landscape', {'purple.dds': (10, 20, 30)})
            dds_archive(root / 'zzOverride.bsa', 'textures\\landscape', {'purple.dds': colour})
            archive(root / 'tamriel.bsa', 'tamriel', [(0, 0)], (200, 180, 120))
            records, lands = minimap.worldspace_records(root, [root / 'override.esp'], include_land=True)
            own = lands['cairn', 0, 0]
            self.assertEqual({layer['texture'] for layer in own['layers']}, {'textures/landscape/purple.dds'})
            textures = {layer['texture']: None for layer in own['layers']}
            sources = minimap.inventory(root, allow_empty=True, textures=textures)
            fallback = minimap.LandTiles(minimap.TerrainTiles(sources, records), lands, textures)
            bsa = textures['textures/landscape/purple.dds'][0]
            with patch.object(bsa, 'read', wraps=bsa.read) as read:
                image, stats = fallback.render('cairn', 0, 0)
                again, _ = fallback.render('cairn', 0, 0)
                self.assertEqual(read.call_count, 1)
            expected = minimap.render_land_cell(own, colour)
            self.assertEqual(expected.getpixel((0, 0)), (72, 60, 94))
            self.assertEqual(image.crop((0, 192, 64, 256)).tobytes(), expected.tobytes())
            self.assertEqual(image.tobytes(), again.tobytes())
            # The production builder must supply the same texture inventory.
            output = root / 'out'
            manifest = minimap.build(root, output, plugins=[root / 'override.esp'])
            tile = manifest['worldspaces']['cairn']['tiles'][0]
            self.assertEqual((output / tile['file']).read_bytes(), minimap.blp2(minimap.terrain_look(image)))
            self.assertEqual(tile['source_sha256'], stats['source_sha256'])
            dds_archive(root / 'zzOverride.bsa', 'textures\\landscape', {'purple.dds': (70, 50, 100)})
            changed = minimap.build(root, output, plugins=[root / 'override.esp'])
            self.assertNotEqual(changed['worldspaces']['cairn']['tiles'][0]['source_sha256'], tile['source_sha256'])

    def test_land_texture_quadrants_alpha_layers_and_relief(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base, extra, top = (40, 60, 80), (120, 100, 80), (80, 40, 120)
            dds_archive(root / 'textures.bsa', 'textures\\landscape',
                        {'base.dds': base, 'extra.dds': extra, 'top.dds': top})
            world = esm.Record('WRLD', 1, subs=[('EDID', b'World\0')])
            cell = esm.Record('CELL', 2, subs=[('XCLC', bytes(8))])
            land = esm.Record('LAND', 3, subs=[('VHGT', struct.pack('<f', 100) + bytes(1092))] + [
                ('BTXT', struct.pack('<IBxh', 4, q, -1)) for q in range(4)] + [
                # Deliberately unsorted layer headers. Missing VTXT positions
                # have zero opacity; the NE layer without VTXT has no effect.
                ('ATXT', struct.pack('<IBxh', 8, 0, 1)),
                ('VTXT', b''.join(struct.pack('<H2xf', p, 0.5) for p in range(289))),
                ('ATXT', struct.pack('<IBxh', 6, 0, 0)),
                ('VTXT', b''.join(struct.pack('<H2xf', p, 0.25) for p in range(289))),
                ('ATXT', struct.pack('<IBxh', 6, 1, 0)),
                ('VTXT', struct.pack('<H2xf', 1, 1)),
                ('ATXT', struct.pack('<IBxh', 6, 2, 0)),
                ('VTXT', b''.join(struct.pack('<H2xf', p, 1) for p in range(289))),
                ('ATXT', struct.pack('<IBxh', 6, 3, 0))])
            definitions = [esm.Record('LTEX', form, subs=[('TNAM', struct.pack('<I', form + 1))])
                           for form in (4, 6, 8)] + [
                esm.Record('TXST', form, subs=[('TX00', f'landscape\\{name}.dds\0'.encode())])
                for form, name in [(5, 'base'), (7, 'extra'), (9, 'top')]]
            land_plugin(root / 'world.esm', world, cell, land, records=definitions)
            records, lands = minimap.worldspace_records(root, include_land=True)
            own = lands['world', 0, 0]
            textures = {layer['texture']: None for layer in own['layers']}
            terrain = minimap.TerrainTiles(minimap.inventory(root, allow_empty=True, textures=textures), records)
            fallback = minimap.LandTiles(terrain, lands, textures)
            tones = fallback.texture_tones(own)
            self.assertEqual(tones[8][8], (70, 55, 100))
            self.assertEqual(tones[8][24], base)
            self.assertEqual(tones[24][8], extra)
            self.assertEqual(tones[24][24], base)
            self.assertEqual(tones[0][17], extra)
            self.assertEqual(tones[0][18], base)
            image, _ = fallback.render('world', 0, 0)
            self.assertEqual(image.getpixel((16, 208)), extra)   # NW, north-up
            self.assertEqual(image.getpixel((16, 240)), (70, 55, 100))   # SW
            # A uniform slope and vertex tint use the existing relief formula.
            sloped = dict(own, height=struct.pack('<f', 100) + bytes([1]) * 1089 + bytes(3),
                          colour=bytes([128]) * 3267)
            shaded = minimap.render_land_cell(sloped, base, tones=tones)
            dx = dy = 8 / 128
            shade = 0.65 + 0.35 * (0.4 * dx - 0.5 * dy + 0.8) / (math.sqrt(1 + dx * dx + dy * dy) * 0.8)
            want = tuple(round(c * shade * (0.8 + 0.2 * 128 / 255)) for c in extra)
            self.assertEqual(shaded.getpixel((16, 16)), want)

    def test_land_palette_never_samples_unrelated_worldspace(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive(root / 'tamriel.bsa', 'tamriel', [(0, 0)], (200, 180, 120))
            esm.write(root / 'worlds.esm', [
                esm.Record('WRLD', 1, subs=[('EDID', b'Tamriel\0')]),
                esm.Record('WRLD', 2, subs=[('EDID', b'Cairn\0'), ('WNAM', struct.pack('<I', 1)),
                                          ('PNAM', struct.pack('<H', 4))]),
                esm.Record('WRLD', 3, subs=[('EDID', b'City\0'), ('WNAM', struct.pack('<I', 1)),
                                          ('PNAM', struct.pack('<H', 2))])], [], 'test', light=False)
            records = minimap.worldspace_records(root)
            own = {'height': struct.pack('<f', 100) + bytes(1092), 'colour': None, 'source': 'fixture'}
            terrain = minimap.TerrainTiles(minimap.inventory(root), records)
            fallback = minimap.LandTiles(terrain, {('cairn', 0, 0): own})
            with patch.object(terrain, 'raw', side_effect=AssertionError('unrelated diffuse sampled')):
                self.assertIsNone(fallback.palette('cairn', 0, 0))
                image, _ = fallback.render('cairn', 0, 0)
            self.assertEqual(image.getpixel((16, 240)), minimap.LAND_PALETTE)
            self.assertEqual(fallback.palette('city', 0, 0), (200, 180, 120))

    def test_city_ground_child_land_texture_tone_and_buildings(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            records, lands, buildings, terrain, fallback = city_ground_fixture(root)
            cities = minimap.CityGroundTiles(terrain, lands, fallback)
            before, _ = terrain.composite('tamriel', 0, 0)
            with patch.object(minimap, 'render_land_cell', wraps=minimap.render_land_cell) as render:
                painted, stats = cities.draw(before, 'tamriel', 0, 0)
            own = lands['city', 3, 1]
            self.assertTrue(any(call.args[0] is own and call.kwargs['tones'] == fallback.texture_tones(own)
                                for call in render.call_args_list))
            self.assertEqual(stats['city_ground_cells'], [dict(cell=[3, 1], world='city', land_world='city',
                                                              source='child.esp:LAND:01000004')])
            self.assertGreater(len(painted.crop((200, 136, 248, 184)).getcolors(4096)), 8)
            # Parent edits cannot replace a child's winning LAND.
            changed = dict(lands)
            changed['tamriel', 3, 1] = dict(own, height=struct.pack('<f', 0) + bytes(1092))
            again, _ = minimap.CityGroundTiles(terrain, changed, fallback).draw(before, 'tamriel', 0, 0)
            self.assertEqual(again.tobytes(), painted.tobytes())
            changed['city', 3, 1] = lands['tamriel', 3, 1]
            different, _ = minimap.CityGroundTiles(terrain, changed, fallback).draw(before, 'tamriel', 0, 0)
            self.assertNotEqual(different.crop((200, 136, 248, 184)).tobytes(),
                                painted.crop((200, 136, 248, 184)).tobytes())
            roofs = minimap.BuildingTiles(buildings, records)
            expected, count = roofs.draw(painted, 'tamriel', 0, 0)
            self.assertEqual(count, 1)
            self.assertNotEqual(expected.tobytes(), painted.tobytes())
            manifest = minimap.build(root, root / 'out', plugins=[root / 'child.esp'])
            tile = manifest['worldspaces']['tamriel']['tiles'][0]
            self.assertEqual(tile['city_ground_cells'], stats['city_ground_cells'])
            self.assertEqual((root / 'out' / tile['file']).read_bytes(), minimap.blp2(minimap.terrain_look(expected)))

    def test_city_ground_parent_land_for_cell_without_child_land(self):
        with tempfile.TemporaryDirectory() as tmp:
            records, lands, _, terrain, fallback = city_ground_fixture(Path(tmp))
            self.assertEqual(records['city']['cells'], [(3, 1), (4, 1)])
            self.assertNotIn(('city', 4, 1), lands)
            cities = minimap.CityGroundTiles(terrain, lands, fallback)
            before, _ = terrain.composite('tamriel', 4, 0)
            painted, stats = cities.draw(before, 'tamriel', 4, 0)
            self.assertEqual(stats['city_ground_cells'], [dict(cell=[4, 1], world='city', land_world='tamriel',
                                                              source='base.esm:LAND:00000034')])
            self.assertNotEqual(painted.crop((8, 136, 56, 184)).tobytes(), before.crop((8, 136, 56, 184)).tobytes())
            changed = dict(lands)
            changed['tamriel', 4, 1] = dict(lands['city', 3, 1])
            different, _ = minimap.CityGroundTiles(terrain, changed, fallback).draw(before, 'tamriel', 4, 0)
            self.assertNotEqual(different.crop((8, 136, 56, 184)).tobytes(), painted.crop((8, 136, 56, 184)).tobytes())
            # Without PNAM use-parent-LOD there is no city paint at all.
            terrain.records = dict(records, city=dict(records['city'], parent=None))
            unchanged, stats = minimap.CityGroundTiles(terrain, lands, fallback).draw(before, 'tamriel', 4, 0)
            self.assertIs(unchanged, before)
            self.assertEqual(stats['city_ground_cells'], [])

    def test_city_ground_nonflat_diffuse_inside_child_coverage_is_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            records, lands, buildings, terrain, fallback = city_ground_fixture(Path(tmp))
            # The child's footprint includes countryside with usable parent LAND, just like the real cities.
            records['city']['cells'].append((2, 1))
            cities = minimap.CityGroundTiles(terrain, lands, fallback)
            self.assertNotIn(('tamriel', 2, 1), cities.cells)
            self.assertIn(('tamriel', 3, 1), cities.cells)
            before, _ = terrain.composite('tamriel', 0, 0)
            ground, stats = cities.draw(before, 'tamriel', 0, 0)
            self.assertEqual([entry['cell'] for entry in stats['city_ground_cells']], [[3, 1]])
            roofs = minimap.BuildingTiles(buildings, records)
            baseline, _ = roofs.draw(before, 'tamriel', 0, 0)
            painted, _ = roofs.draw(ground, 'tamriel', 0, 0, before)
            box = (128, 128, 192, 192)
            self.assertEqual(painted.crop(box).tobytes(), baseline.crop(box).tobytes())
            old, new = (minimap.dxt1(minimap.terrain_look(image)) for image in (baseline, painted))
            for row in range(32, 48):
                self.assertEqual(old[row * 512 + 256:row * 512 + 384],
                                 new[row * 512 + 256:row * 512 + 384])

    def test_city_ground_local_variance_accepts_only_compression_sized_noise(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, lands, _, terrain, fallback = city_ground_fixture(Path(tmp))
            original_raw = terrain.raw
            for amplitude, expected in [(2, True), (10, False)]:
                image, digest = original_raw('tamriel', 0, 0)
                image = image.copy()
                for y in range(128, 192):
                    for x in range(192, 256):
                        image.putpixel((x, y), tuple(c + amplitude * ((x + y) % 2) for c in (150, 130, 100)))

                def raw(world, x, y):
                    return (image, digest) if (world, x, y) == ('tamriel', 0, 0) else original_raw(world, x, y)

                with patch.object(terrain, 'raw', side_effect=raw):
                    cities = minimap.CityGroundTiles(terrain, lands, fallback)
                    self.assertEqual(('tamriel', 3, 1) in cities.cells, expected)

    def test_city_ground_partially_flat_cell_paints_only_mask_pixels(self):
        with tempfile.TemporaryDirectory() as tmp:
            records, lands, buildings, terrain, fallback = city_ground_fixture(Path(tmp))
            original_raw = terrain.raw
            image, digest = original_raw('tamriel', 0, 0)
            image = image.copy()
            # Detailed terrain fills the western half of an otherwise flat city cell.
            detailed, _ = terrain.composite('tamriel', 8, 0)
            image.paste(detailed.crop((0, 128, 32, 192)), (192, 128))
            # A small disconnected flat island must not pass the 400-pixel region minimum.
            image.paste((160, 140, 110), (194, 140, 208, 154))

            def raw(world, x, y):
                return (image, digest) if (world, x, y) == ('tamriel', 0, 0) else original_raw(world, x, y)

            with patch.object(terrain, 'raw', side_effect=raw):
                cities = minimap.CityGroundTiles(terrain, lands, fallback)
                painted, stats = cities.draw(image, 'tamriel', 0, 0)
            mask = minimap.np.asarray(cities.mask('tamriel', 0, 0)) != 0
            changed = minimap.np.any(minimap.np.asarray(painted) != minimap.np.asarray(image), axis=2)
            self.assertTrue(changed[136:184, 232:252].any())
            self.assertFalse(changed[128:192, 192:224].any())
            self.assertFalse(mask[128:192, 192:224].any())
            self.assertFalse(changed[~mask].any())
            self.assertEqual(stats['city_ground_pixels'], int(changed.sum()))
            self.assertEqual(stats['city_ground_mask_pixels'], int(mask.sum()))
            self.assertLess(stats['city_ground_mask_pixels'], 32 * 64)
            # A roof spans both halves: compositing it must preserve its non-mask pixels too.
            building = dict(buildings[0], position=(3.5 * 4096, 1.5 * 4096))
            building['corners'] = minimap.building_footprint(building['bounds'], *building['position'], 0, 1)
            roofs = minimap.BuildingTiles([building], records)
            baseline, _ = roofs.draw(image, 'tamriel', 0, 0)
            composed, _ = roofs.draw(painted, 'tamriel', 0, 0, image)
            self.assertTrue(minimap.np.array_equal(minimap.np.asarray(baseline)[~mask],
                                                 minimap.np.asarray(composed)[~mask]))

    def test_city_ground_missing_diffuse_and_black_padding_are_not_placeholders(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, lands, _, terrain, fallback = city_ground_fixture(Path(tmp))
            sources = dict(terrain.sources)
            del sources['tamriel', 0, 0]
            missing = minimap.TerrainTiles(sources, terrain.records)
            self.assertNotIn(('tamriel', 3, 1), minimap.CityGroundTiles(missing, lands, fallback).cells)
            with patch.object(terrain, 'raw', return_value=(Image.new('RGB', (256, 256)), 'unused')):
                self.assertEqual(minimap.CityGroundTiles(terrain, lands, fallback).cells, {})

    def test_city_ground_pixel_region_minimum_spans_tile_border(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, lands, _, terrain, fallback = city_ground_fixture(Path(tmp))
            original_raw = terrain.raw
            detailed, _ = original_raw('tamriel', 8, 0)
            images = {}
            for x, box in [(0, (244, 148, 256, 172)), (4, (0, 148, 12, 172))]:
                image = detailed.copy()
                image.paste((160, 140, 110), box)
                images[x] = image

            def raw(world, x, y):
                return (images[x], 'unused') if y == 0 and x in images else original_raw(world, x, y)

            with patch.object(terrain, 'raw', side_effect=raw):
                cities = minimap.CityGroundTiles(terrain, lands, fallback)
                self.assertEqual(len(cities.components), 1)
                self.assertEqual(cities.components[0][1].histogram()[255], 22 * 22)
                for x, px in [(0, 255), (4, 0)]:
                    image = images[x]
                    mask = cities.mask('tamriel', x, 0)
                    self.assertLess(mask.histogram()[255], minimap.CITY_GROUND_MIN_PIXELS)
                    painted, stats = cities.draw(image, 'tamriel', x, 0)
                    self.assertGreater(stats['city_ground_pixels'], 0)
                    self.assertNotEqual(painted.getpixel((px, 160)), image.getpixel((px, 160)))
                    self.assertTrue(minimap.np.array_equal(minimap.np.asarray(image)[minimap.np.asarray(mask) == 0],
                                                         minimap.np.asarray(painted)[minimap.np.asarray(mask) == 0]))

    def test_city_ground_region_colour_cannot_drift_along_a_smooth_gradient(self):
        pixels = minimap.np.broadcast_to(minimap.np.arange(80, 176, dtype='uint8')[None, :, None], (64, 96, 3))
        image = Image.fromarray(pixels.copy())
        coverage = Image.new('L', image.size, 255)
        masks = list(minimap.CityGroundTiles.placeholder_masks(image, coverage, coverage))
        self.assertTrue(masks)
        for mask in masks:
            samples = pixels[minimap.np.asarray(mask) != 0].astype(float)
            self.assertGreaterEqual(len(samples), minimap.CITY_GROUND_MIN_PIXELS)
            self.assertLessEqual(minimap.np.abs(samples - minimap.np.median(samples, axis=0)).max(),
                                 minimap.CITY_GROUND_COLOUR_TOLERANCE)
            self.assertLess(len(samples), 64 * 96 // 2)

    def test_city_ground_child_relief_inherits_missing_texture_quadrants(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, lands, _, terrain, fallback = city_ground_fixture(Path(tmp))
            own = lands['city', 3, 1]
            parent = lands['tamriel', 3, 1]
            lands['city', 3, 1] = dict(own, layers=[])
            cities = minimap.CityGroundTiles(terrain, lands, fallback)
            self.assertEqual(cities.texture_tones('city', 3, 1), fallback.texture_tones(parent))
            before, _ = terrain.composite('tamriel', 0, 0)
            with patch.object(minimap, 'render_land_cell', wraps=minimap.render_land_cell) as render:
                cities.draw(before, 'tamriel', 0, 0)
            self.assertTrue(any(call.args[0]['height'] == own['height'] and
                                call.kwargs['tones'] == fallback.texture_tones(parent)
                                for call in render.call_args_list))
            # A defined child quadrant overrides its parent; undefined quadrants retain parent materials.
            lands['city', 3, 1] = dict(own, layers=[own['layers'][0]])
            tones = minimap.CityGroundTiles(terrain, lands, fallback).texture_tones('city', 3, 1)
            self.assertEqual(tones[8][8], fallback.texture_tones(own)[8][8])
            self.assertEqual(tones[8][24], fallback.texture_tones(parent)[8][24])

    def test_city_ground_cell_bounds_feather_tile_seams_and_unchanged_diffuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, lands, _, terrain, fallback = city_ground_fixture(Path(tmp))
            cities = minimap.CityGroundTiles(terrain, lands, fallback)
            self.assertEqual(len(cities.components), 1)
            ground, alpha, low_x, high_y = cities.render(0)
            # Two connected cells across the tile edge share full opacity, rather than fading to the placeholder.
            seam_x, middle_y = (4 - low_x) * 64, (high_y - 2) * 64 + 32
            self.assertEqual((alpha.getpixel((seam_x - 1, middle_y)), alpha.getpixel((seam_x, middle_y))), (255, 255))
            left, top = (3 - low_x) * 64, (high_y - 2) * 64
            self.assertEqual(alpha.getpixel((left, middle_y)), 0)
            self.assertGreater(alpha.getpixel((left + 2, middle_y)), 0)
            self.assertLess(alpha.getpixel((left + 2, middle_y)), 255)
            self.assertEqual(alpha.getpixel((left + 4, middle_y)), 255)
            # Before feathering, the whole patch matches RGB mean and luminance contrast of its outside ring.
            mask = cities.components[0][1]
            inside = minimap.np.asarray(mask) != 0
            samples = minimap.np.asarray(ground)[inside]
            diffuse, coverage = cities.diffuse_region('tamriel', low_x, high_y, mask.size)
            ring = ImageChops.multiply(ImageChops.subtract(mask.filter(minimap.ImageFilter.MaxFilter(17)), mask),
                                       coverage)
            outside = minimap.np.asarray(diffuse)[minimap.np.asarray(ring) != 0]
            self.assertTrue(minimap.np.allclose(samples.mean(axis=0), minimap.np.mean(outside, axis=0), atol=1))
            weights = minimap.np.array([0.299, 0.587, 0.114])
            self.assertAlmostEqual((samples @ weights).std(), (minimap.np.asarray(outside) @ weights).std(), delta=1)
            for x in (0, 4, 8):
                before, _ = terrain.composite('tamriel', x, 0)
                painted, stats = cities.draw(before, 'tamriel', x, 0)
                for cx in range(4):
                    for cy in range(4):
                        if (x + cx, cy) in ((3, 1), (4, 1)):
                            continue
                        box = (cx * 64, (3 - cy) * 64, (cx + 1) * 64, (4 - cy) * 64)
                        self.assertEqual(painted.crop(box).tobytes(), before.crop(box).tobytes())
                self.assertEqual(terrain.composite('tamriel', x, 0)[0].tobytes(), before.tobytes())
                if x == 8:
                    self.assertIs(painted, before)
                    self.assertEqual(stats['city_ground_cells'], [])
                    self.assertEqual(minimap.blp2(painted), minimap.blp2(before))

    def test_city_ground_constant_ring_retains_relief(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, lands, _, terrain, fallback = city_ground_fixture(Path(tmp))
            before = Image.new('RGB', (256, 256), (100, 110, 90))
            cities = minimap.CityGroundTiles(terrain, lands, fallback)
            with patch.object(terrain, 'composite', return_value=(before, {})):
                painted, _ = cities.draw(before, 'tamriel', 0, 0)
            self.assertGreater(len(painted.crop((200, 136, 248, 184)).getcolors(4096)), 8)
            self.assertEqual(painted.crop((0, 0, 192, 256)).tobytes(), before.crop((0, 0, 192, 256)).tobytes())

    def test_city_ground_keeps_neighbouring_building_pixels_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            records, lands, buildings, terrain, fallback = city_ground_fixture(Path(tmp))
            # A roof straddles the western city edge, so its local palette samples both cells.
            building = dict(buildings[0], position=(3 * 4096, 1.5 * 4096))
            building['corners'] = minimap.building_footprint(building['bounds'], *building['position'], 0, 1)
            roofs = minimap.BuildingTiles([building], records)
            original, _ = terrain.composite('tamriel', 0, 0)
            baseline, _ = roofs.draw(original, 'tamriel', 0, 0)
            ground, _ = minimap.CityGroundTiles(terrain, lands, fallback).draw(original, 'tamriel', 0, 0)
            painted, _ = roofs.draw(ground, 'tamriel', 0, 0, original)
            self.assertEqual(painted.crop((0, 0, 192, 256)).tobytes(), baseline.crop((0, 0, 192, 256)).tobytes())
            old, new = (minimap.dxt1(minimap.terrain_look(image)) for image in (baseline, painted))
            for row in range(64):
                self.assertEqual(old[row * 512:row * 512 + 384], new[row * 512:row * 512 + 384])
            self.assertNotEqual(painted.crop((192, 156, 196, 164)).tobytes(),
                                baseline.crop((192, 156, 196, 164)).tobytes())

    def test_real_tamriel_land_cell_relief_and_manifest(self):
        # Riverwood's shipped Tamriel CELL: scan metadata, stop after one LAND.
        cell, land = None, None
        for world, cell_form, record in esm.exterior_land(minimap.DATA / 'Skyrim.esm'):
            if world != 0x3c:
                continue
            if record.type == 'CELL' and record.get('XCLC') and struct.unpack_from('<ii', record.get('XCLC')) == (4, -15):
                cell = record
            elif record.type == 'LAND' and cell is not None and cell_form == cell.form:
                land = record
                break
        self.assertIsNotNone(cell, 'Tamriel CELL (4,-15) is required')
        self.assertIsNotNone(land, 'Tamriel CELL (4,-15) LAND is required')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            land_plugin(root / 'Skyrim.esm', esm.Record('WRLD', 0x3c, subs=[('EDID', b'Tamriel\0')]), cell, land)
            records, lands = minimap.worldspace_records(root, include_land=True)
            self.assertIn(('tamriel', 4, -15), lands)
            # A neutral VCLR isolates height lighting from colour variation.
            neutral = dict(lands['tamriel', 4, -15], colour=bytes([255]) * (33 * 33 * 3))
            rendered = minimap.render_land_cell(neutral, (96, 91, 75))
            self.assertEqual(rendered.size, (64, 64))
            self.assertGreater(len(rendered.getcolors(4096)), 8)
            flat = dict(neutral, height=struct.pack('<f', 657) + bytes(1092))
            self.assertEqual(len(minimap.render_land_cell(flat, (96, 91, 75)).getcolors()), 1)
            # A neighbouring diffuse tile stays byte-identical. The other
            # archive covers LAND itself, so adding it must suppress fallback.
            archive(root / 'neighbour.bsa', 'tamriel', [(0, -16)], (96, 91, 75))
            output = root / 'out'
            manifest = minimap.build(root, output)
            tiles = {tuple(tile['cell']): tile for tile in manifest['worldspaces']['tamriel']['tiles']}
            fallback = tiles[4, -16]
            self.assertTrue(fallback['land_derived'])
            self.assertEqual(fallback['land_cells'], [[4, -15]])
            self.assertIn('Skyrim.esm:LAND:', fallback['source'])
            with Image.open(output / fallback['file']) as tile:
                self.assertEqual(tile.size, (256, 256))
                # Cell Y=-15 occupies the third row of this north-up tile.
                self.assertGreater(len(tile.convert('RGB').crop((0, 128, 64, 192)).getcolors(4096)), 8)
            diffuse = minimap.blp2(minimap.terrain_look(Image.new('RGB', (256, 256), (96, 91, 75))))
            self.assertEqual((output / tiles[0, -16]['file']).read_bytes(), diffuse)
            self.assertNotIn('land_derived', tiles[0, -16])
            archive(root / 'cover.bsa', 'tamriel', [(4, -16)], (96, 91, 75))
            manifest = minimap.build(root, output)
            covered = next(tile for tile in manifest['worldspaces']['tamriel']['tiles'] if tile['cell'] == [4, -16])
            self.assertNotIn('land_derived', covered)
            self.assertEqual((output / covered['file']).read_bytes(), diffuse)
            # A world with no diffuse archive can still build a LAND-only tile.
            manifest = minimap.build(root, root / 'land-only', archives=[])
            self.assertEqual(len(manifest['worldspaces']['tamriel']['tiles']), 1)
            self.assertTrue(manifest['worldspaces']['tamriel']['tiles'][0]['land_derived'])

    def test_building_footprint_rotation_scale_relief_and_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            terrain = (120, 140, 100)
            archive(root / 'tamriel.bsa', 'tamriel', [(0, 0), (4, 0)], terrain)
            # Local east (x) 0..512 by north (y) -128..128, doubled by XSCL and turned 90 degrees: Skyrim turns
            # clockwise seen from above, so the long side runs south from the origin. Tilt is ignored.
            house = static(0x10, 'Architecture\\Test\\TestHouse01.nif', (0, -128, 0, 512, 128, 400))
            world = esm.Record('WRLD', 0x3c, subs=[('EDID', b'Tamriel\0')])
            cell = esm.Record('CELL', 0x20, subs=[('XCLC', struct.pack('<ii', 2, 1))])
            ref_plugin(root / 'Skyrim.esm', [(world, cell, [
                reference(0x30, 0x10, 8192, 8192, math.pi / 2, 2.0, tilt=(0.3, 0.2))], [])], records=[house])
            records, _, buildings = minimap.worldspace_records(root, include_land=True, include_buildings=True)
            self.assertEqual([b['source'] for b in buildings], ['Skyrim.esm:REFR:00000030'])
            for got, want in zip(buildings[0]['corners'], [(7936, 8192), (7936, 7168), (8448, 7168), (8448, 8192)]):
                self.assertAlmostEqual(got[0], want[0], 3)
                self.assertAlmostEqual(got[1], want[1], 3)
            before, _ = minimap.TerrainTiles(minimap.inventory(root), records).composite('tamriel', 0, 0)
            roofs = minimap.BuildingTiles(buildings, records)
            image, count = roofs.draw(before, 'tamriel', 0, 0)
            self.assertEqual(count, 1)
            # x 7936..8448 and y 7168..8192 are columns 124..131 and rows 128..143 of the north-up tile.
            self.assertEqual(ImageChops.difference(before, image).getbbox(), (124, 128, 132, 144))
            roof, lit, shaded = image.getpixel((127, 135)), image.getpixel((124, 135)), image.getpixel((131, 135))
            for pixel, want in [((127, 128), lit), ((131, 128), lit), ((124, 143), lit), ((127, 143), shaded)]:
                self.assertEqual(image.getpixel(pixel), want)
            luma = lambda c: 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]
            self.assertLess(abs(luma(roof) - 0.6 * luma(terrain)), 2)
            self.assertLess(max(roof) - min(roof), 0.6 * (max(terrain) - min(terrain)))
            self.assertGreater(luma(lit), luma(roof))
            self.assertLess(luma(shaded), luma(roof))
            # The manifest counts footprints per tile; a diffuse tile without one stays byte-identical.
            output = root / 'out'
            manifest = minimap.build(root, output)
            tiles = {tuple(t['cell']): t for t in manifest['worldspaces']['tamriel']['tiles']}
            self.assertEqual(tiles[0, 0]['building_footprints'], 1)
            self.assertEqual(tiles[4, 0]['building_footprints'], 0)
            self.assertEqual((output / tiles[0, 0]['file']).read_bytes(), minimap.blp2(minimap.terrain_look(image)))
            plain = minimap.blp2(minimap.terrain_look(Image.new('RGB', (256, 256), terrain)))
            self.assertEqual((output / tiles[4, 0]['file']).read_bytes(), plain)

    def test_building_filters_enable_state_and_overrides(self):
        for model, want in [('Architecture\\Whiterun\\WRBuildings\\WRHouse02.nif', True),
                            ('meshes\\DLC02\\Architecture\\RavenRock\\DLC2RRBuilding01.nif', True),
                            ('Dungeons\\Imperial\\Exterior\\ImpExtTower01.nif', True),
                            ('Architecture\\Whiterun\\WRTerrain\\WRBreezeBase.nif', False),
                            ('Architecture\\Whiterun\\WRBuildings\\WRHouse02LOD.nif', False),
                            ('Architecture\\Farmhouse\\Walkway\\WalkwayBend03.nif', False),
                            ('Dungeons\\Nordic\\Exterior\\NorTowerRuins01.nif', False)]:
            self.assertEqual(minimap.building_model(model), want, model)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            statics = [static(0x10, 'Architecture\\Test\\TestHouse01.nif', (-400, -400, 0, 400, 400, 500)),
                       static(0x11, 'Architecture\\Test\\TestShack01.nif', (-100, -100, 0, 100, 100, 200)),
                       static(0x12, 'Landscape\\Rocks\\RockL01.nif', (-800, -800, 0, 800, 800, 600)),
                       static(0x13, 'Architecture\\Whiterun\\WRClutter\\WRCart01.nif', (-400, -400, 0, 400, 400, 300)),
                       static(0x14, 'Markers\\XMarker.nif', (-8, -8, 0, 8, 8, 8)),
                       static(0x15, 'Architecture\\Test\\TestCastle01.nif', (-2100, -300, 0, 2100, 300, 800))]
            world = esm.Record('WRLD', 0x3c, subs=[('EDID', b'Tamriel\0')])
            cell = esm.Record('CELL', 0x20, subs=[('XCLC', struct.pack('<ii', 0, 0))])
            refs = [reference(0x30, 0x10, 1000, 1000),                       # deleted by the override below
                    reference(0x31, 0x11, 2000, 1000),                       # clutter-sized: 200 units
                    reference(0x32, 0x11, 3000, 1000, scale=2.0),             # 400 units after XSCL
                    reference(0x33, 0x12, 4000, 1000),                       # not architecture
                    reference(0x34, 0x13, 5000, 1000),                       # architecture clutter folder
                    reference(0x35, 0x10, 6000, 1000, flags=0x800),          # initially disabled
                    reference(0x36, 0x14, 0, 0, flags=0x800),                # a disabled enable-parent marker
                    reference(0x37, 0x10, 7000, 1000, parent=(0x36, 0)),     # follows the disabled marker
                    reference(0x38, 0x10, 8000, 1000, parent=(0x36, 1)),     # opposite of the marker: enabled
                    reference(0x3a, 0x15, 12000, 1000),                      # 4200 units: longer than a cell
                    reference(0x3b, 0x15, 11000, 1000, scale=0.9)]           # 3780 units after XSCL
            ref_plugin(root / 'base.esm', [(world, cell, refs, [reference(0x39, 0x10, 9000, 1000)])],
                       records=statics)
            # The override lists its masters in another order: it deletes one house and moves the persistent one.
            esm.write(root / 'other.esm', [], [], 'test', light=False)
            ref_plugin(root / 'override.esp', [(esm.Record('WRLD', 0x0100003c, subs=[('EDID', b'Tamriel\0')]),
                                                esm.Record('CELL', 0x01000020, subs=[('XCLC', bytes(8))]),
                                                [reference(0x01000030, 0x01000010, 1000, 1000, flags=0x20)],
                                                [reference(0x01000039, 0x01000010, 10000, 2000)])],
                       ['other.esm', 'base.esm'])
            _, buildings = minimap.worldspace_records(root, [root / 'override.esp'], include_buildings=True)
            self.assertEqual([(b['source'], round(b['corners'][0][0]), round(b['corners'][0][1])) for b in buildings],
                             [('base.esm:REFR:00000032', 2800, 800), ('base.esm:REFR:00000038', 7600, 600),
                              ('override.esp:REFR:01000039', 9600, 1600), ('base.esm:REFR:0000003A', 9900, 700),
                              ('base.esm:REFR:0000003B', 9110, 730)])
            # The size cap applies only when geometry cannot be read, not while collecting references.
            roofs = minimap.BuildingTiles(buildings, {})
            self.assertEqual(roofs.fallback_skipped, 1)

    def test_child_world_buildings_use_parent_lod_tiles(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive(root / 'tamriel.bsa', 'tamriel', [(0, 0)], (90, 100, 80))
            archive(root / 'markarth.bsa', 'markarth', [(0, 0)], (110, 100, 90))
            house = static(0x10, 'Architecture\\Test\\TestHouse01.nif', (-256, -256, 0, 256, 256, 400))
            worlds = [esm.Record('WRLD', 0x3c, subs=[('EDID', b'Tamriel\0')]),
                      esm.Record('WRLD', 0x3d, subs=[('EDID', b'City\0'), ('WNAM', struct.pack('<I', 0x3c)),
                                                     ('PNAM', struct.pack('<H', 0x02))]),
                      esm.Record('WRLD', 0x3e, subs=[('EDID', b'Markarth\0'), ('WNAM', struct.pack('<I', 0x3c)),
                                                     ('PNAM', struct.pack('<H', 0))])]
            ref_plugin(root / 'Skyrim.esm', [
                (worlds[1], esm.Record('CELL', 0x21, subs=[('XCLC', struct.pack('<ii', 1, 1))]),
                 [reference(0x31, 0x10, 4096 + 2048, 4096 + 2048)], []),
                (worlds[2], esm.Record('CELL', 0x22, subs=[('XCLC', struct.pack('<ii', 2, 2))]),
                 [reference(0x32, 0x10, 8192 + 2048, 8192 + 2048)], []),
                (worlds[0], esm.Record('CELL', 0x20, subs=[('XCLC', bytes(8))]), [], [])], records=[house])
            output = root / 'out'
            manifest = minimap.build(root, output)
            tamriel = manifest['worldspaces']['tamriel']['tiles'][0]
            markarth = manifest['worldspaces']['markarth']['tiles'][0]
            self.assertEqual((tamriel['building_footprints'], markarth['building_footprints']), (1, 1))
            records, _, buildings = minimap.worldspace_records(root, include_land=True, include_buildings=True)
            roofs = minimap.BuildingTiles(buildings, records)
            self.assertEqual(roofs.tile('city', 0, 0), roofs.tile('tamriel', 0, 0))
            terrain = minimap.TerrainTiles(minimap.inventory(root), records)
            for world, box in [('tamriel', (92, 156, 100, 164)), ('markarth', (156, 92, 164, 100))]:
                before, _ = terrain.composite(world, 0, 0)
                image, _ = roofs.draw(before, world, 0, 0)
                # The City house (cell 1,1) is on Tamriel's tile; Markarth's own LOD keeps its house to itself.
                self.assertEqual(ImageChops.difference(before, image).getbbox(), box)

    def test_land_height_delta_base_and_north_up(self):
        deltas = [0] * 1089
        deltas[0], deltas[1], deltas[33], deltas[34] = -2, 3, 4, -5
        data = struct.pack('<f1089b', 100, *deltas) + bytes(3)
        heights = minimap.land_heights(data)
        self.assertEqual(heights[0][:3], [784, 808, 808])
        self.assertEqual(heights[1][:3], [816, 776, 776])
        colours = bytearray([255] * (1089 * 3))
        colours[:33 * 3] = bytes([0]) * (33 * 3)
        flat = struct.pack('<f', 100) + bytes(1092)
        image = minimap.render_land_cell({'height': flat, 'colour': bytes(colours)}, (100, 100, 100))
        self.assertEqual(image.getpixel((0, 0)), (100, 100, 100))
        self.assertEqual(image.getpixel((0, 63)), (80, 80, 80))

    def test_land_override_offset_grid_and_parent_coverage(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            esm.write(root / 'base.esm', [esm.Record('WRLD', 1, subs=[('EDID', b'Parent\0')])], [], 'test', light=False)
            esm.write(root / 'other.esm', [], [], 'test', light=False)
            flat = struct.pack('<f', 100) + bytes(1092)
            cell = esm.Record('CELL', 0x01000003, subs=[('XCLC', struct.pack('<ii', -4, -1))])
            world = esm.Record('WRLD', 0x01000002, subs=[('EDID', b'Child\0'), ('WNAM', struct.pack('<I', 1)), ('PNAM', struct.pack('<H', 0x02))])
            land = esm.Record('LAND', 0x01000004, subs=[('VHGT', flat)])
            land_plugin(root / 'child.esp', world, cell, land, ['base.esm'])
            override = struct.pack('<f', 200) + bytes(1092)
            world.form, cell.form, land.form = 0x02000002, 0x02000003, 0x02000004
            world.set('WNAM', struct.pack('<I', 0x01000001)), ('PNAM', struct.pack('<H', 0x02))
            land.set('VHGT', override)
            land_plugin(root / 'override.esp', world, cell, land, ['other.esm', 'base.esm', 'child.esp'])
            records, lands = minimap.worldspace_records(root, [root / 'override.esp'], include_land=True)
            self.assertEqual(lands['child', -4, -1]['height'], override)
            self.assertTrue(lands['child', -4, -1]['source'].startswith('override.esp:LAND:'))
            archive(root / 'parent.bsa', 'parent', [(-8, -4)], (60, 80, 100))
            archive(root / 'child.bsa', 'child', [(-3, -1)], (80, 100, 120))
            terrain = minimap.TerrainTiles(minimap.inventory(root), records)
            fallback = minimap.LandTiles(terrain, lands)
            self.assertEqual(list(fallback.tiles), [('child', -7, -1)])
            image, stats = fallback.render('child', -7, -1)
            self.assertEqual(image.size, (256, 256))
            self.assertEqual(image.getpixel((10, 250)), (60, 80, 100))
            self.assertEqual(stats['parent_pixels'], 3 * 64 * 64)
            self.assertEqual(stats['uncovered_empty_pixels'], 12 * 64 * 64)
            # A winning deletion removes LAND rather than reviving its master.
            land.flags = 0x20
            land_plugin(root / 'override.esp', world, cell, land, ['other.esm', 'base.esm', 'child.esp'])
            _, lands = minimap.worldspace_records(root, [root / 'override.esp'], include_land=True)
            self.assertEqual(lands, {})

    def test_parent_records_master_remapping_overrides_and_cycles(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            esm.write(root / 'base.esm', [esm.Record('WRLD', 1, subs=[('EDID', b'Parent\0')])], [], 'test', light=False)
            esm.write(root / 'other.esm', [esm.Record('WRLD', 1, subs=[('EDID', b'Other\0')])], [], 'test', light=False)
            esm.write(root / 'child.esp', [esm.Record('WRLD', 0x02000001, subs=[
                ('EDID', b'Child\0'), ('WNAM', struct.pack('<I', 0x01000001)), ('PNAM', struct.pack('<H', 0x02))])],
                ['other.esm', 'base.esm'], 'test', light=False)
            records = minimap.worldspace_records(root, [root / 'child.esp'])
            self.assertEqual(records['child']['parent'], 'parent')
            esm.write(root / 'override.esp', [esm.Record('WRLD', 0x02000001, subs=[
                ('EDID', b'Child\0'), ('WNAM', struct.pack('<I', 0x01000001)), ('PNAM', struct.pack('<H', 0x02))])],
                ['base.esm', 'other.esm', 'child.esp'], 'test', light=False)
            records = minimap.worldspace_records(root, [root / 'override.esp'])
            self.assertEqual(records['child']['parent'], 'other')
            self.assertEqual(records['child']['source_plugin'], 'override.esp')
            esm.write(root / 'cycle.esp', [esm.Record('WRLD', 1, subs=[
                ('EDID', b'Parent\0'), ('WNAM', struct.pack('<I', 0x02000001)), ('PNAM', struct.pack('<H', 0x02))])],
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
                esm.Record('WRLD', 2, subs=[('EDID', b'Child\0'), ('WNAM', struct.pack('<I', 1)), ('PNAM', struct.pack('<H', 0x02))])],
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

    def test_markarth_padding_takes_tamriel_pixels_at_the_same_coordinates(self):
        np = minimap.np
        with tempfile.TemporaryDirectory() as tmp:
            terrain, ground = padding_fixture(Path(tmp))
            self.assertIsNone(terrain.records['markarthworld']['parent'])   # PNAM: its own LOD
            padding = minimap.PaddingTiles(ground, sorted(terrain.sources))
            before = [np.asarray(terrain.composite('markarthworld', x, -3)[0]) for x in (-7, -3)]
            after, masks = [], []
            for x, image in zip((-7, -3), before):
                filled, stats = padding.draw(Image.fromarray(image), 'markarthworld', x, -3)
                mask = np.asarray(padding.fill('markarthworld')[1])[:, (x + 7) * 64:(x + 7) * 64 + 256] != 0
                self.assertEqual(stats, {'padding_world': 'tamriel', 'padding_mask_pixels': int(mask.sum()),
                                         'padding_pixels': int(np.any(np.asarray(filled) != image, axis=2).sum())})
                after.append(np.asarray(filled))
                masks.append(mask)
            before, after, mask = (np.concatenate(images, axis=1) for images in (before, after, masks))
            tamriel = np.concatenate([np.asarray(terrain.region('tamriel', x, -3)[0]) for x in (-7, -3)], axis=1)
            # Non-padding pixels, including a small natural patch of the fill colour, are unchanged.
            self.assertTrue(np.array_equal(after[~mask], before[~mask]))
            self.assertFalse(mask[200:210, 100:110].any())
            self.assertTrue(mask[64:192, 192:320].all() and mask[:64, :64].all() and mask[64:192:2, 192].all())
            # Padding four or more pixels inside, across the tile border too, is Tamriel's texel at the same X/Y.
            for rows, columns in [(slice(68, 188), slice(196, 316)), (slice(0, 60), slice(0, 60)),
                                  (slice(104, 146), slice(460, 502))]:
                self.assertTrue(np.array_equal(after[rows, columns], tamriel[rows, columns]))
            # The feather blends from adjoining terrain: neither the fill colour nor black shows.
            filled = after[mask].astype(int)
            self.assertGreater(np.abs(filled - (75, 73, 61)).max(axis=1).min(), minimap.PADDING_EDGE)
            self.assertGreater(filled.max(axis=1).min(), 8)
            for column, alpha in [(192, 0), (193, 0.259), (194, 0.741)]:
                spread = minimap.spread(before, ~mask, 3)[120, column]
                self.assertTrue(np.allclose(after[120, column], spread + alpha * (tamriel[120, column] - spread), atol=1.5))
            # Unrendered source coverage keeps its padding; other worlds are untouched.
            missing = minimap.PaddingTiles(ground, [key for key in terrain.sources if key != ('tamriel', 0, -4)])
            image = Image.fromarray(before[:, 256:])
            kept, _ = missing.draw(image, 'markarthworld', -3, -3)
            self.assertEqual(kept.crop((200, 100, 250, 150)).tobytes(), image.crop((200, 100, 250, 150)).tobytes())
            self.assertEqual(padding.draw(image, 'tamriel', -3, -3), (image, {}))

    def test_markarth_padding_build_leaves_other_worlds_identical(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            terrain, ground = padding_fixture(root)
            manifest = minimap.build(root, root / 'out')
            self.assertEqual(manifest['padding_sources'], {'markarthworld': 'tamriel'})
            padding = minimap.PaddingTiles(ground, sorted(terrain.sources))
            for tile in manifest['worldspaces']['markarthworld']['tiles']:
                image, stats = padding.draw(terrain.composite('markarthworld', *tile['cell'])[0], 'markarthworld', *tile['cell'])
                self.assertGreater(stats['padding_pixels'], 0)
                self.assertEqual({key: tile[key] for key in stats}, stats)
                self.assertEqual((root / 'out' / tile['file']).read_bytes(), minimap.blp2(minimap.terrain_look(image)))
            with patch.dict(minimap.PADDING_SOURCES, clear=True):
                plain = minimap.build(root, root / 'plain')
            self.assertEqual(manifest['worldspaces']['tamriel'], plain['worldspaces']['tamriel'])
            for tile in plain['worldspaces']['markarthworld']['tiles']:
                self.assertNotIn('padding_pixels', tile)
                self.assertNotEqual((root / 'plain' / tile['file']).read_bytes(), (root / 'out' / tile['file']).read_bytes())

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
