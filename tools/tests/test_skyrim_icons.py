from collections import Counter
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from tools import package
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / 'build/skyrim_icons'


class GeneratedIconsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.inventory = json.loads((ASSETS / 'inventory.json').read_text())
        cls.manifest = json.loads((ASSETS / 'manifest.json').read_text())

    def test_every_form_and_effect_has_a_real_icon(self):
        forms = {r['key']: r for r in self.inventory['forms']}
        mapping = self.manifest['forms']
        self.assertEqual(set(forms), set(mapping))
        self.assertEqual(dict(Counter(r['record_type'] for r in forms.values())), self.inventory['counts'])
        expected = set()
        for key, row in mapping.items():
            with self.subTest(form=key):
                expected.add(row['icon'] + '.blp')
                raw = (ASSETS / (row['icon'] + '.blp')).read_bytes()
                self.assertEqual(hashlib.sha256(raw).hexdigest(), row['sha256'])
                self.assertEqual(row['effects'], [e['form'] for e in forms[key]['effects']])
                self.assertTrue(all(e in mapping for e in row['effects']))
                self.assertEqual(row['plugin'], forms[key]['plugin'])
                self.assertEqual(row['local_form_id'], forms[key]['local_form_id'])
        self.assertEqual(expected, {p.name for p in ASSETS.glob('*.blp')})
        self.assertEqual(len(expected), self.manifest['icon_count'])
        self.assertEqual(len({r['sha256'] for r in mapping.values()}), len(mapping))
        self.assertTrue(all((ASSETS / p).is_file() for p in self.manifest['contact_sheets']))

    def test_fire_spell_family_is_visually_distinct(self):
        spells = {r['name']: r for r in self.manifest['forms'].values()
                  if r['plugin'] == 'Skyrim.esm' and r['record_type'] == 'SPEL' and r['name'] in ('Flames', 'Firebolt', 'Fireball')}
        self.assertEqual(set(spells), {'Flames', 'Firebolt', 'Fireball'})
        self.assertEqual(len({r['sha256'] for r in spells.values()}), 3)
        self.assertEqual({r['recipe']['colour'] for r in spells.values()}, {'fire'})
        self.assertEqual({r['recipe']['variant'] for r in spells.values()}, {'stream', 'bolt', 'burst'})

    def test_client_bevel_survives_encoding_for_spells_and_applied_effects(self):
        self.assertEqual(self.manifest['bevel'], {'source': 'Spell_Nature_HealingTouch', 'inset': 4})
        # Helpful buffs draw the icon directly, so the baked edge must carry the
        # highlight and shadow through palette encoding, independent of the art.
        for key, row in self.manifest['forms'].items():
            with self.subTest(form=key), Image.open(ASSETS / (row['icon'] + '.blp')) as encoded:
                im = encoded.convert('RGB')
                top = sum(sum(im.getpixel((x, 1))) for x in range(8, 56))
                bottom = sum(sum(im.getpixel((x, 62))) for x in range(8, 56))
                self.assertGreater(top, bottom * 1.5)

    def test_shouts_use_common_art_with_different_effect_motifs(self):
        shouts = {r['name']: r for r in self.manifest['forms'].values()
                  if r['plugin'] == 'Skyrim.esm' and r['record_type'] == 'SHOU' and
                  r['name'] in ('Unrelenting Force', 'Whirlwind Sprint', 'Slow Time', 'Become Ethereal')}
        self.assertEqual(len(shouts), 4)
        self.assertEqual({r['recipe']['base'] for r in shouts.values()}, {'INV_Misc_Head_Dragon_01'})
        self.assertEqual(len({r['recipe']['motif'] for r in shouts.values()}), 4)
        self.assertNotIn('frost', {r['recipe']['colour'] for r in shouts.values()})

    def test_packaging_copies_all_icons_to_mod_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            package.pack_icons(Path(tmp))
            target = Path(tmp) / 'SKSE/Plugins/WoWBridge/Icons'
            expected = {r['icon'] + '.blp' for r in self.manifest['forms'].values()} | {'manifest.json', 'inventory.json'}
            self.assertEqual({p.name for p in target.iterdir()}, expected)
            self.assertEqual((target / 'manifest.json').read_bytes(), (ASSETS / 'manifest.json').read_bytes())


if __name__ == '__main__':
    unittest.main()
