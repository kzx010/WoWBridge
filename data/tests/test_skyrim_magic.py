import json
from pathlib import Path
import struct
import tempfile
import unittest

from data import esm
from data.skyrim_magic import Bsa, Strings, inventory


class MagicInventoryTest(unittest.TestCase):
    def test_overrides_resolve_to_owner_and_effects_keep_their_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = bytearray(152)
            struct.pack_into('<I', data, 0, 1 | 4)
            struct.pack_into('<ii', data, 12, 20, 41)
            struct.pack_into('<ii', data, 64, 0, 24)
            effect = esm.Record('MGEF', 0x200, subs=[('EDID', esm.zstring('FireDamage')),
                                                  ('FULL', esm.zstring('Burn')), ('DATA', bytes(data))])
            spell = esm.Record('SPEL', 0x100, subs=[('EDID', esm.zstring('Flames')),
                ('FULL', esm.zstring('Flames')), ('SPIT', bytes(36)),
                ('EFID', struct.pack('<I', 0x200)), ('EFIT', struct.pack('<fII', 8.0, 0, 3)),
                ('CTDA', bytes(32)), ('EFID', struct.pack('<I', 0x200)), ('EFIT', struct.pack('<fII', 2.0, 10, 7))])
            esm.write(root / 'Skyrim.esm', [effect, spell], [], 'fixture', light=False)
            effect.set('FULL', esm.zstring('Updated Burn'))
            esm.write(root / 'Update.esm', [effect], ['Skyrim.esm'], 'fixture', light=False)
            spell.form = 0x02000300
            spell.set('FULL', esm.zstring('Dawnguard Flames'))
            # References still point to Skyrim.esm, index zero in Dawnguard's masters.
            esm.write(root / 'Dawnguard.esm', [spell], ['Skyrim.esm', 'Update.esm'], 'fixture', light=False)
            for name in ('HearthFires.esm', 'Dragonborn.esm'):
                esm.write(root / name, [], ['Skyrim.esm', 'Update.esm'], 'fixture', light=False)
            result = inventory(root)
            forms = {r['key']: r for r in result['forms']}
            self.assertEqual(result['counts'], {'MGEF': 1, 'SPEL': 2})
            self.assertEqual(forms['Skyrim.esm:000200']['name'], 'Updated Burn')
            self.assertEqual(forms['Skyrim.esm:000200']['source_plugin'], 'Update.esm')
            self.assertEqual(forms['Skyrim.esm:000200']['actor_value'], 'Health')
            self.assertEqual(forms['Skyrim.esm:000200']['school'], 'Destruction')
            self.assertTrue(forms['Skyrim.esm:000200']['hostile'])
            effects = forms['Dawnguard.esm:000300']['effects']
            self.assertEqual([e['form'] for e in effects], ['Skyrim.esm:000200'] * 2)
            self.assertEqual([e['duration'] for e in effects], [3, 7])
            self.assertEqual(len(effects[0]['conditions']), 1)
            self.assertEqual(effects[1]['conditions'], [])

    def test_localized_names_in_sse_lz4_archive(self):
        import lz4.frame
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            text = 'Localized Flames'.encode() + b'\0'
            strings = struct.pack('<4I', 1, len(text), 42, 0) + text
            filename = b'Skyrim_english.strings\0'
            folder = b'Strings\0'
            embedded = b'Strings\\Skyrim_english.strings'
            packed = bytes([len(embedded)]) + embedded + struct.pack('<I', len(strings)) + lz4.frame.compress(strings)
            # v105 folder offsets include the filename-table length; sequential block
            # reading also supports this convention without guessing at those offsets.
            at = 36 + 24 + 1 + len(folder) + 16 + len(filename)
            header = struct.pack('<4s8I', b'BSA\0', 105, 36, 0x107, 1, 1, len(folder), len(filename), 0)
            folder_record = struct.pack('<QIIQ', 0, 1, 0, 60 + len(filename))
            block = bytes([len(folder)]) + folder + struct.pack('<QII', 0, len(packed), at)
            path = root / 'Skyrim - Interface.bsa'
            path.write_bytes(header + folder_record + block + filename + packed)
            self.assertEqual(Bsa(path).read('strings/skyrim_english.strings'), strings)
            resolver = Strings(root, 'english')
            record = esm.Record('SPEL', 1, subs=[('FULL', struct.pack('<I', 42))])
            self.assertEqual(resolver.text(record, 'FULL', 'Skyrim.esm', True), 'Localized Flames')
            record.set('FULL', struct.pack('<I', 43))
            with self.assertRaisesRegex(ValueError, 'missing english'):
                resolver.text(record, 'FULL', 'Skyrim.esm', True)


if __name__ == '__main__':
    unittest.main()
