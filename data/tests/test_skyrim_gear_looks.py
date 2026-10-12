import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from data import esm
from data import build_skyrim_gear_looks as looks


def gear(kind='ARMO', **extra):
    return dict(key='Skyrim.esm:000100', plugin='Skyrim.esm', local_form_id=256,
                source_plugin='Skyrim.esm', record_type=kind, edid='ArmorIronCuirass', name='Iron Armor',
                keywords=['ArmorMaterialIron'], models=[], biped_slots=[32], armor_type=1,
                animation_type=None, **extra)


def candidate(entry=10, display=100, name='Iron Gray Cuirass', subclass=4, inv=5, level=15,
              assets=('Plate_Gray',), item_class=4):
    return dict(entry=entry, displayid=display, name=name, **{'class': item_class},
                subclass=subclass, InventoryType=inv, ItemLevel=level, Quality=1, sheath=0,
                assets=list(assets), asset_text=' '.join(assets).casefold())


class GearInventoryTest(unittest.TestCase):
    def test_overrides_masters_templates_deleted_ignored_and_zero_slots(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            keyword = esm.Record('KYWD', 0x20, subs=[('EDID', esm.zstring('ArmorMaterialSteel'))])
            addon = esm.Record('ARMA', 0x30, subs=[('MOD3', esm.zstring(r'Armor\Steel\Female.nif'))])
            base = esm.Record('ARMO', 0x100, subs=[('EDID', esm.zstring('SteelCuirass')),
                ('FULL', esm.zstring('Steel Armor')), ('BOD2', struct.pack('<II', 4, 1)),
                ('KWDA', struct.pack('<I', 0x20)), ('MODL', struct.pack('<I', 0x30))])
            deleted = copy.deepcopy(base)
            deleted.form = 0x101
            zero = copy.deepcopy(base)
            zero.form = 0x102
            zero.set('BOD2', struct.pack('<II', 0, 1))
            weapon = esm.Record('WEAP', 0x200, subs=[('EDID', esm.zstring('SteelWarhammer')),
                ('FULL', esm.zstring('Steel Warhammer')), ('DNAM', bytes([6]) + bytes(99)),
                ('MODL', esm.zstring(r'Weapons\Steel\Warhammer.nif'))])
            weapon_variant = copy.deepcopy(weapon)
            weapon_variant.form = 0x201
            weapon_variant.drop('MODL')
            weapon_variant.set('CNAM', struct.pack('<I', 0x200))
            # TNAM refers to a sound, not a weapon template; don't resolve it.
            weapon_variant.set('TNAM', struct.pack('<I', 0x999))
            esm.write(root / 'Skyrim.esm', [keyword, addon, base, deleted, zero, weapon, weapon_variant], [], 'test', light=False)
            updated = copy.deepcopy(base)
            updated.set('FULL', esm.zstring('Updated Steel'))
            deleted.flags = 0x20
            esm.write(root / 'Update.esm', [updated, deleted], ['Skyrim.esm'], 'test', light=False)
            ignored = copy.deepcopy(base)
            ignored.flags = 0x1000
            variant = copy.deepcopy(base)
            variant.form = 0x02000800
            variant.set('EDID', esm.zstring('EnchSteelCuirass'))
            variant.drop('MODL', 'KWDA')
            variant.set('TNAM', struct.pack('<I', 0x100))
            esm.write(root / 'DLC.esm', [ignored, variant], ['Skyrim.esm', 'Update.esm'], 'test', light=False)
            inv = looks.inventory(root, plugins=['DLC.esm'])
            rows = {r['key']: r for r in inv['forms']}
            self.assertEqual(inv['plugins'], ['Skyrim.esm', 'Update.esm', 'DLC.esm'])
            self.assertEqual(set(rows), {'Skyrim.esm:000100', 'Skyrim.esm:000200', 'Skyrim.esm:000201', 'DLC.esm:000800'})
            self.assertEqual(rows['Skyrim.esm:000100']['source_plugin'], 'Update.esm')
            self.assertEqual(rows['Skyrim.esm:000100']['name'], 'Updated Steel')
            self.assertEqual(rows['DLC.esm:000800']['models'], [r'Armor\Steel\Female.nif'])
            self.assertEqual(rows['DLC.esm:000800']['keywords'], ['ArmorMaterialSteel'])
            self.assertEqual(rows['Skyrim.esm:000201']['models'], [r'Weapons\Steel\Warhammer.nif'])
            self.assertEqual(inv['excluded_zero_slot_armors'], ['Skyrim.esm:000102'])
            profile, _ = looks.profile_for(rows['Skyrim.esm:000200'], looks.family_for(rows['Skyrim.esm:000200']))
            self.assertEqual((profile['kind'], profile['subclass']), ('warhammer', 5))

    def test_legacy_bodt_clothing_and_invalid_animation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            row = esm.Record('ARMO', 1, subs=[('EDID', esm.zstring('Skin')),
                ('BODT', struct.pack('<II', 4, 0x10))])
            esm.write(root / 'Skyrim.esm', [row], [], 'test', light=False)
            inv = looks.inventory(root, plugins=['Skyrim.esm'])
            self.assertEqual(inv['forms'][0]['armor_type'], 2)
            row.set('BODT', struct.pack('<III', 4, 0, 1))
            esm.write(root / 'Skyrim.esm', [row], [], 'test', light=False)
            self.assertEqual(looks.inventory(root, plugins=['Skyrim.esm'])['forms'][0]['armor_type'], 1)
        row = gear('WEAP')
        row['animation_type'] = 100
        with self.assertRaisesRegex(ValueError, 'unknown weapon animation'):
            looks.profile_for(row, looks.family_for(row))


class MatchRulesTest(unittest.TestCase):
    def test_torso_expands_to_legs_and_hooded_robe_to_helm(self):
        row = gear()
        inv = {'forms': [row], 'plugins': ['Skyrim.esm'], 'excluded_zero_slot_armors': []}
        result = looks.build(inv, [candidate(), candidate(entry=11, display=101, inv=7)])
        self.assertEqual(result['items'][row['key']]['additional'][0]['inventory_type'], 7)
        self.assertFalse(any(w.startswith(('Skyrim torso', 'Hooded body'))
                             for w in result['items'][row['key']]['limitations']))
        row.update(armor_type=2, biped_slots=[31, 32, 42], edid='HoodedRobes', name='Robes', keywords=[])
        result = looks.build(inv, [candidate(inv=20, subclass=1),
                                  candidate(entry=12, display=102, inv=1, subclass=1,
                                            name='Brown Hood', assets=('Helm_Cloth_Hood',))])
        self.assertEqual(result['items'][row['key']]['additional'][0]['inventory_type'], 1)
        self.assertFalse(any(w.startswith(('Skyrim torso', 'Hooded body'))
                             for w in result['items'][row['key']]['limitations']))

    def test_type_constraints_beat_color_tier_and_style(self):
        row = gear()
        wrong_slot = candidate(entry=1, inv=10)
        wrong_weight = candidate(entry=2, subclass=1)
        right = candidate(entry=3, display=333, name='Plain Plate', assets=('Plate_Brown',), level=60)
        result = looks.build({'forms': [row], 'plugins': ['Skyrim.esm'], 'excluded_zero_slot_armors': []},
                             [wrong_slot, wrong_weight, right, candidate(entry=4, inv=7)])
        self.assertEqual(result['items'][row['key']]['display_id'], 333)
        self.assertIn('nearest compatible tier', result['items'][row['key']]['reason'])

    def test_material_keyword_beats_enchantment_name_and_model_color_sets_palette(self):
        row = gear()
        row.update(keywords=['ArmorMaterialGlass'], edid='EnchIronResistFire', name='Glass Armor of Iron Will',
                   models=[r'Armor\Glass\GreenCuirass.nif'], armor_type=0)
        family = looks.family_for(row)
        self.assertEqual(family[0], 'glass')
        self.assertEqual(looks.palette_for(row, family), 'green')
        self.assertEqual(looks.profile_for(row, family)[0]['subclass'], 3)

    def test_worn_geometry_beats_misleading_gameplay_material(self):
        row = gear()
        row.update(keywords=['ArmorMaterialDaedric'], models=[r'Armor\Draugr\Cuirass.nif'])
        self.assertEqual(looks.family_for(row)[0], 'draugr')
        row.update(keywords=['ArmorMaterialDragonplate'], models=[r'Armor\Dragonbone\Cuirass.nif'])
        self.assertEqual(looks.family_for(row)[0], 'dragonplate')
        row.update(keywords=['ArmorMaterialScaled'], models=[r'Armor\Hide\CuirassHeavy.nif'])
        self.assertEqual(looks.family_for(row)[0], 'scaled')
        row.update(keywords=['ArmorMaterialLeather'], models=[r'Armor\Studded\Body.nif'])
        self.assertEqual(looks.family_for(row)[0], 'leather')

    def test_explicit_asset_color_beats_misleading_item_name(self):
        family = next(f for f in looks.FAMILIES if f[0] == 'iron')
        green = candidate(name='Ironvine Gloves', assets=('Plate_Green_Glove',), level=15)
        silver = candidate(name='Plain Gloves', assets=('Plate_Silver_Glove',), level=30)
        self.assertGreater(looks.candidate_score(silver, family, 'grey')[0],
                           looks.candidate_score(green, family, 'grey')[0])

    def test_metal_helmet_does_not_become_a_color_matched_tiara(self):
        family = next(f for f in looks.FAMILIES if f[0] == 'dwarven')
        crown = candidate(name='Golden Crown', inv=1, assets=('Helm_Circlet_Gold',), level=40)
        helmet = candidate(name='Golden Helmet', inv=1, assets=('Helm_Plate_Gold',), level=40)
        self.assertGreater(looks.candidate_score(helmet, family, 'bronze', {'head_style': 'helmet'})[0],
                           looks.candidate_score(crown, family, 'bronze', {'head_style': 'helmet'})[0])

    def test_weapons_battleaxe_warhammer_crossbow_and_unarmed(self):
        row = gear('WEAP')
        row.update(keywords=[], models=[], name='', edid='Weapon')
        for anim, hints, subclass, kind in [(6, ['WeapTypeBattleaxe'], 1, 'battleaxe'),
                                           (6, ['WeapTypeWarhammer'], 5, 'warhammer'),
                                           (9, [], 18, 'crossbow'), (0, [], 13, 'unarmed')]:
            row.update(animation_type=anim, keywords=hints)
            profile, warnings = looks.profile_for(row, looks.family_for(row))
            self.assertEqual((profile['subclass'], profile['kind']), (subclass, kind))
            if anim == 0:
                self.assertTrue(warnings)

    def test_hooded_robe_jewelry_shield_and_unsupported_slots_are_explicit(self):
        row = gear()
        row.update(armor_type=2, biped_slots=[31, 32, 42], edid='HoodedRobes', name='Robes', keywords=[])
        profile, warnings = looks.profile_for(row, looks.family_for(row))
        self.assertEqual(profile['types'], (20,))
        self.assertEqual(profile['kind'], 'chest')
        self.assertEqual(len(warnings), 2)
        for slots, inv, subclass in [([35], 2, 0), ([36], 11, 0), ([39], 14, 6), ([61], 11, 0)]:
            row['biped_slots'] = slots
            profile, warnings = looks.profile_for(row, looks.family_for(row))
            self.assertEqual((profile['types'], profile['subclass']), ((inv,), subclass))
            if slots != [39]:
                self.assertTrue(warnings)

    def test_determinism_ties_and_enchanted_variants(self):
        row = gear()
        enchanted = copy.deepcopy(row)
        enchanted.update(key='Skyrim.esm:000101', local_form_id=257,
                         name='Iron Armor of Fire Resistance', edid='EnchArmorIronHealth06')
        inv = {'forms': [row, enchanted], 'plugins': ['Skyrim.esm'], 'excluded_zero_slot_armors': []}
        candidates = [candidate(entry=11, display=101), candidate(entry=10, display=100), candidate(entry=12, inv=7)]
        result = looks.build(inv, candidates)
        self.assertEqual(looks.serialize(result), looks.serialize(looks.build(inv, list(reversed(candidates)))))
        self.assertEqual([r['display_id'] for r in result['items'].values()], [100, 100])
        with self.assertRaisesRegex(ValueError, 'no compatible'):
            looks.build(inv, [])

    def test_reviewed_overrides_keep_equipment_constraints_and_cover_variants(self):
        row = gear()
        row.update(keywords=['ArmorMaterialDwarven'], models=[r'Armor\Dwarven\Helmet.nif'], biped_slots=[30])
        variant = dict(row, key='Skyrim.esm:000101', local_form_id=257, edid='EnchDwarvenHelmet')
        inv = {'forms': [row, variant], 'plugins': ['Skyrim.esm'], 'excluded_zero_slot_armors': []}
        correct = candidate(entry=16731, display=31284, inv=1, name='Helm of Valor', assets=('Helm_Plate_Gold',))
        result = looks.build(inv, [correct])
        self.assertEqual([r['display_id'] for r in result['items'].values()], [31284, 31284])
        self.assertTrue(all('reviewed entry override' in r['reason'] for r in result['items'].values()))
        with self.assertRaisesRegex(ValueError, 'incompatible/missing'):
            looks.build(inv, [dict(correct, InventoryType=5)])
        with patch.dict(looks.ENTRY_OVERRIDES, {row['key']: 99}):
            with self.assertRaisesRegex(ValueError, 'incompatible/missing'):
                looks.build(inv, [correct])


class GeneratedCatalogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.result = json.loads((looks.ROOT / 'data/skyrim_gear_looks.json').read_text())

    def test_every_row_has_valid_reference_identity_types_and_reason(self):
        result = self.result
        self.assertEqual(len(result['items']), result['counts']['total'])
        for key, row in result['items'].items():
            with self.subTest(key=key):
                self.assertEqual(key, looks.identity(row['plugin'], row['local_form_id']))
                self.assertIn(row['source_plugin'], result['plugins'])
                wow = result['wow_items'][str(row['wow_entry'])]
                for field in ('display_id', 'inventory_type', 'class', 'subclass'):
                    self.assertEqual(row[field], wow[field])
                self.assertGreater(row['display_id'], 0)
                self.assertTrue(row['reason'])
                self.assertIn(row['family'], {r[0] for r in looks.FAMILIES})
                for extra in row.get('additional', []):
                    reference = result['wow_items'][str(extra['wow_entry'])]
                    self.assertIn(extra['inventory_type'], (1, 7))
                    for field in ('display_id', 'inventory_type', 'class', 'subclass'):
                        self.assertEqual(extra[field], reference[field])

    def test_live_plugin_coverage_when_source_is_available(self):
        if not (looks.DEFAULT_DATA / 'Skyrim.esm').exists():
            self.skipTest('Installed Skyrim plugins unavailable; portable rule tests still run.')
        inv = looks.inventory(looks.DEFAULT_DATA, self.result['language'], self.result['plugins'])
        self.assertEqual({r['key'] for r in inv['forms']}, set(self.result['items']))
        self.assertEqual(inv['excluded_zero_slot_armors'], self.result['excluded_zero_slot_armors'])
        for row in inv['forms']:
            mapped = self.result['items'][row['key']]
            profile, _ = looks.profile_for(row, looks.family_for(row))
            c = {'class': mapped['class'], 'subclass': mapped['subclass'], 'InventoryType': mapped['inventory_type']}
            self.assertTrue(looks.compatible(c, profile), row['key'])


if __name__ == '__main__':
    unittest.main()
