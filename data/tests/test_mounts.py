import json
import unittest
from pathlib import Path
from data import dbc
from data.build_mounts import build, REPO, DBC_DIR


class MountCatalogTest(unittest.TestCase):
    def test_export_matches_pinned_client_and_creature_models(self):
        catalog = json.loads((REPO / 'data/mounts.json').read_text())
        self.assertEqual(build(), catalog)
        self.assertEqual(len(catalog['mounts']), 25)
        self.assertEqual(len({r['key'] for r in catalog['mounts']}), 25)
        self.assertEqual(sum(not r.get('flying') and 'class' not in r for r in catalog['mounts']), 21)
        # The Paladin's class mounts: their spells' creature models, aura 32's speed, mana, the level that teaches them.
        warhorse, charger = (next(r for r in catalog['mounts'] if r['spell'] == s) for s in (13819, 23214))
        self.assertEqual((warhorse['key'], warhorse['creature'], warhorse['display'], warhorse['speed_percent'],
                          warhorse['class'], warhorse['level'], warhorse['mana'], warhorse['model']),
                         ('summon_warhorse', 9158, 8469, 60, 2, 40, 100, 'Creature\\WarHorse\\WarHorse.mdx'))
        self.assertEqual((charger['key'], charger['creature'], charger['speed_percent'], charger['level'], charger['mana']),
                         ('summon_charger', 14565, 100, 60, 150))
        saber = next(r for r in catalog['mounts'] if r['spell'] == 23221)
        self.assertEqual(saber['display'], 14331)  # spell misc is creature 14556, not display 14556
        self.assertEqual(saber['creature'], 14556)

    def test_flyers_are_synthetic_spells_on_classic_models(self):
        catalog = json.loads((REPO / 'data/mounts.json').read_text())
        spells = dbc.by_id(dbc.read(DBC_DIR / 'Spell.dbc'))
        flyers = {r['key']: r for r in catalog['mounts'] if r.get('flying')}
        self.assertEqual(set(flyers), {'swift_golden_gryphon', 'swift_green_wind_rider'})
        gryphon, rider = flyers['swift_golden_gryphon'], flyers['swift_green_wind_rider']
        self.assertEqual((gryphon['spell'], gryphon['creature'], gryphon['display']), (32235, 541, 6852))  # taxi Riding Gryphon
        self.assertEqual(gryphon['model'], 'Creature\\Gryphon\\Gryphon.mdx')
        self.assertEqual(gryphon['icon'], 'Interface\\Icons\\Ability_Hunter_Pet_Owl')
        self.assertEqual((rider['spell'], rider['display']), (32295, 11011))  # WyvernSkin01Green
        self.assertEqual(rider['model'], 'Creature\\Wyvern\\Wyvern.mdx')
        self.assertEqual(rider['icon'], 'Interface\\Icons\\Ability_Hunter_Pet_Bat')
        for row in flyers.values():
            self.assertNotIn(row['spell'], spells)  # the UI supplies their spell info
            # Rows remain valid for skse/src/Mounts.cpp Init's epic mount checks.
            self.assertRegex(row['key'], r'^[a-z_]+$')
            self.assertEqual((row['speed_percent'], row['cast_ms'], row['flight_speed_percent']), (100, 3000, 280))
            self.assertTrue(row['description'].startswith('Summons and dismisses a rideable '))
            self.assertIn('fly outdoors', row['description'])
