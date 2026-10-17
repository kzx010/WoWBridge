"""Staff attachment regression, independent of catalog/DBC build inputs."""
import os
import struct
import unittest
from pathlib import Path

from data import build_esp as B, esm


MODEL = r'Weapons\Iron\WarHammer.nif'


def staff_record():
    template = esm.Record('WEAP', 1, subs=[
        ('EDID', esm.zstring('IronWarhammer')), ('MODL', esm.zstring(MODEL)),
        ('MODT', b'old texture hashes'), ('KWDA', B.u32(2)),
        ('DATA', struct.pack('<IfH', 60, 24, 18)),
        ('DNAM', bytes([6]) + bytes(99)), ('CRDT', bytes(24)),
    ])
    wood = esm.Record('KYWD', 2, subs=[('EDID', esm.zstring('WeapMaterialWood'))])
    item = {'entry': 873, 'name': 'Staff', 'class': 2, 'subclass': 10, 'InventoryType': 17, 'displayid': 42}
    templates = {'IronWarhammer': template, 'WeapMaterialWood': wood}
    stats = {'damage': 22, 'speed': .7, 'value': 100}
    return item, templates, stats, B.weapon(item, templates, 0x800, stats, file=2)


class StaffParentTests(unittest.TestCase):
    def test_staff_keeps_melee_type_and_compatible_model_even_with_reviewed_look(self):
        item, templates, stats, record = staff_record()
        self.assertEqual(record.get('MODL'), esm.zstring(MODEL))
        self.assertIsNone(record.get('MODT'))
        self.assertEqual(record.get('DNAM')[0], 6)  # two-handed axe/warhammer animations
        self.assertEqual(struct.unpack('<IfH', record.get('DATA')), (100, 24, 22))
        self.assertAlmostEqual(struct.unpack_from('<f', record.get('DNAM'), 4)[0], .7)
        # A reviewed vanilla staff must not overwrite the compatible mesh later in the build.
        templates['ReviewedStaff'] = esm.Record('WEAP', 3)
        curated = {42: [{'edid': 'ReviewedStaff', 'subclass': 10}]}
        self.assertIsNone(B.look(item, stats, templates, curated))

    def test_model_prn_exists_on_vanilla_skeleton(self):
        from data.skyrim_magic import Bsa
        from tools.nif import Reader

        data = Path(os.environ.get('SKYRIM_DATA', B.MASTER.parent))
        paths = sorted(data.glob('Skyrim - Meshes*.bsa'))
        if not paths:
            self.skipTest('Skyrim mesh BSAs unavailable; set SKYRIM_DATA to the install Data directory')
        archives = [Bsa(path) for path in paths]

        def metadata(name):
            archive = next(a for a in archives if name.casefold() in a.files)
            r = Reader(archive.read(name.casefold()))
            self.assertEqual(r.take(len(b'Gamebryo File Format, Version 20.2.0.7\n')),
                             b'Gamebryo File Format, Version 20.2.0.7\n')
            version, endian, user, count, bs = r.get('IBIII')
            self.assertEqual((version, endian, user, bs), (0x14020007, 1, 12, 100))
            for _ in range(3):
                r.string(small=True)
            types = [r.string() for _ in range(r.get('H'))]
            ids = [r.get('H') for _ in range(count)]
            sizes = [r.get('I') for _ in range(count)]
            nstrings = r.get('I')
            r.get('I')
            strings = [r.string() for _ in range(nstrings)]
            r.take(r.get('I') * 4)
            nodes, extras = set(), {}
            for tid, size in zip(ids, sizes):
                kind, block = types[tid], Reader(r.take(size))
                if kind in ('NiNode', 'BSFadeNode'):
                    nodes.add(strings[block.get('i')])
                elif kind == 'NiStringExtraData':
                    key, value = block.get('2i')
                    extras[strings[key]] = strings[value]
            return nodes, extras

        nodes, _ = metadata('meshes/actors/character/character assets/skeleton.nif')
        _, old = metadata('meshes/weapons/staff01/staff01.nif')
        _, replacement = metadata('meshes/' + staff_record()[3].get('MODL').rstrip(b'\0').decode().replace('\\', '/'))
        self.assertEqual(old['Prn'], 'WeaponStaff')
        self.assertNotIn(old['Prn'], nodes)
        self.assertEqual(replacement['Prn'], 'WeaponBack')
        self.assertIn(replacement['Prn'], nodes)
        self.assertIn('WEAPON', nodes)
