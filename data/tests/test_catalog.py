"""Catalog fixtures cross-checked against CMaNGOS spell_template and the 1.12.1 client DBCs."""
import json
import unittest
from pathlib import Path

from data import dbc, build_catalog
from data.sql_dump import tables

CATALOG = Path(__file__).resolve().parents[2] / 'build/catalog.json'


class Catalog(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.c = json.loads(CATALOG.read_text())

    def spell(self, name):
        return self.c['spells'][str(self.c['slice'][name])]

    def test_wrath(self):
        s = self.spell('wrath')
        self.assertEqual((s['EffectBasePoints1'], s['EffectDieSides1'], s['ManaCost'], s['School']), (235, 29, 180, 3))
        self.assertAlmostEqual(s['EffectBonusCoefficient1'], 0.571, places=3)
        self.assertEqual(self.c['cast_times'][str(s['CastingTimeIndex'])]['Base'], 2000)
        self.assertEqual(self.c['ranges'][str(s['RangeIndex'])]['RangeMax'], 30.0)

    def test_moonfire_direct_and_periodic(self):
        s = self.spell('moonfire')
        self.assertEqual((s['ManaCost'], s['School']), (375, 6))
        self.assertEqual((s['EffectBasePoints1'], s['EffectAmplitude1']), (95, 3000))   # periodic tick
        self.assertEqual((s['EffectBasePoints2'], s['EffectDieSides2']), (188, 33))     # direct hit
        self.assertEqual(self.c['durations'][str(s['DurationIndex'])]['Duration'], 12000)

    def test_rejuvenation_and_healing_touch(self):
        r, h = self.spell('rejuvenation'), self.spell('healing_touch')
        self.assertEqual((r['EffectBasePoints1'], r['EffectAmplitude1'], r['ManaCost']), (221, 3000, 360))
        self.assertEqual((h['EffectBasePoints1'], h['EffectDieSides1'], h['ManaCost']), (2266, 411, 800))
        self.assertEqual(self.c['cast_times'][str(h['CastingTimeIndex'])]['Base'], 3500)

    def test_entangling_roots(self):
        s = self.spell('entangling_roots')
        self.assertEqual((s['ManaCost'], s['Attributes'] & 0x8000), (125, 0x8000))   # SPELL_ATTR_OUTDOORS_ONLY
        self.assertEqual(self.c['durations'][str(s['DurationIndex'])]['Duration'], 27000)

    def test_hurricane_rank3(self):
        s = self.spell('hurricane')
        self.assertEqual((s['Id'], s['ManaCost']), (17402, 1495))
        self.assertEqual(self.c['durations'][str(s['DurationIndex'])]['Duration'], 10000)

    def test_stormrage_set_and_idol(self):
        sets = self.c['item_sets']['214']
        self.assertEqual([x for x in sets['SetSpellID'] if x], [21894, 21872, 21871])
        self.assertEqual([x for x in sets['SetThreshold'] if x], [3, 5, 8])
        self.assertEqual(self.c['items']['22398']['spellid_1'], 27853)
        self.assertIn({'entry': 27853, 'effectId': 0, 'SpellFamilyMask': 16}, self.c['spell_affect'])

    def test_character(self):
        ls, cs = self.c['character']['levelstats'], self.c['character']['classlevelstats']
        self.assertEqual((ls['inte'], ls['spi'], cs['basemana']), (100, 110, 1244))

    def test_equipment_matches_pinned_dump_and_client_sets(self):
        sets = dbc.by_id(dbc.read(CATALOG.parent / 'dbc/DBFilesClient/ItemSet.dbc'))
        required = set(range(201, 210)) | {214}
        self.assertTrue(required <= {s['id'] for s in self.c['equipment_sets'].values()})
        for group in self.c['equipment_sets'].values():
            source = sets[group['id']]
            self.assertEqual(group['name'], source['Name_lang'])
            self.assertEqual(group['entries'], [e for e in source['ItemID'] if e])
            if group['id'] in required:
                self.assertEqual(len(group['entries']), 8)
            self.assertEqual(self.c['item_sets'][str(group['id'])], source)
        entries = {int(e) for e in self.c['items']}
        source = tables(build_catalog.DEFAULT_DUMP, {'item_template': None},
                        filters={'item_template': lambda r: r['entry'] in entries})['item_template']
        self.assertEqual({str(r['entry']): {**r, 'RequiredLevel': 0} for r in source}, self.c['items'])
        for row in source:
            for n in range(1, 6):
                if row[f'spellid_{n}']:
                    self.assertIn(str(row[f'spellid_{n}']), self.c['spells'])

    def test_equipment_selection_is_data_driven(self):
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'equipment.json'
            path.write_text(json.dumps({'sets': {'druid': 205}, 'items': [19019]}))
            client_sets = dbc.read(CATALOG.parent / 'dbc/DBFilesClient/ItemSet.dbc')
            rows = {int(e): row for e, row in self.c['items'].items()}
            items, sets, groups = build_catalog.equipment(path, client_sets, rows)
            self.assertEqual({i['entry'] for i in items}, set(groups['druid']['entries']) | {19019})
            self.assertEqual([s['ID'] for s in sets], [205])
            self.assertTrue(all(i['RequiredLevel'] == 0 for i in items))


if __name__ == '__main__':
    unittest.main()
