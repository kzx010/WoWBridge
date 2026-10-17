"""data/racial_data.json: every playable class's race stats and outfits, and the Human's and Dwarf's racials (data only)."""
import json
import unittest

from data import dbc
from data.build_catalog import DBC_DIR, DEFAULT_DUMP, RACIAL_SPELLS, REPO
from data.build_paladin import CMANGOS
from data.build_racial_data import PENDING, RACIAL_LINES, TREATMENTS, build
from data.sql_dump import tables

OUT = REPO / 'data/racial_data.json'
STATS = ('str', 'agi', 'sta', 'inte', 'spi')


class RacialData(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = json.loads(OUT.read_text())
        cls.rows = tables(DEFAULT_DUMP, {'player_levelstats': None})['player_levelstats']
        cls.racials = {r['spell']: r for rows in cls.data['racials'].values() for r in rows}

    def test_checked_in_export_matches_builder(self):
        self.assertEqual(OUT.read_text(), json.dumps(build(), indent=1, sort_keys=True) + '\n')

    def test_druid_keys_are_her_rows_unchanged(self):
        # Exactly ClassicDB's class 11 rows for the Night Elf and Tauren, and the client's Druid outfits, as before.
        for race in (4, 6):
            rows = {str(r['level']): r for r in self.rows if r['class'] == 11 and r['race'] == race}
            self.assertEqual(self.data['levelstats_by_race'][str(race)], rows)
        self.assertEqual(set(self.data['levelstats_by_race']), {'4', '6'})
        outfits = [r for r in dbc.read(DBC_DIR / 'CharStartOutfit.dbc') if r['RaceID'] in (4, 6) and r['ClassID'] == 11]
        self.assertEqual(self.data['starting_outfits'], outfits)
        self.assertEqual(self.data['levelstats_by_class']['11'], self.data['levelstats_by_race'])
        self.assertEqual(self.data['starting_outfits_by_class']['11'], self.data['starting_outfits'])
        self.assertEqual([self.data['levelstats_by_race']['4']['60'][k] for k in STATS], [62, 65, 69, 100, 110])

    def test_paladin_level_stats_and_outfits(self):
        stats = self.data['levelstats_by_class']['2']
        self.assertEqual(set(stats), {'1', '3'})
        for race, levels in stats.items():
            self.assertEqual(sorted(map(int, levels)), list(range(1, 61)))
            self.assertEqual(levels, {str(r['level']): r for r in self.rows if r['class'] == 2 and r['race'] == int(race)})
        self.assertEqual([stats['1']['60'][k] for k in STATS], [105, 65, 100, 70, 78])   # Human
        self.assertEqual([stats['3']['60'][k] for k in STATS], [107, 61, 103, 69, 74])   # Dwarf
        self.assertEqual([stats['3']['1'][k] for k in STATS], [24, 16, 25, 19, 20])
        outfits = self.data['starting_outfits_by_class']['2']
        self.assertEqual(sorted((r['RaceID'], r['SexID']) for r in outfits), [(1, 0), (1, 1), (3, 0), (3, 1)])
        for row in outfits:   # the Paladin's hammer, shirt and trousers; the Dwarf's own tunic and leggings
            self.assertEqual(row['ItemID'][:3], [45, 43, 44] if row['RaceID'] == 1 else [6117, 6118, 43])
            self.assertIn(2361, row['ItemID'])                                    # Battleworn Hammer

    def test_racials_are_each_races_general_tab(self):
        manifest = json.loads((REPO / 'data/paladin_manifest.json').read_text())['class']['races']
        lines = dbc.by_id(dbc.read(DBC_DIR / 'SkillLine.dbc'))
        self.assertEqual({race: lines[line]['DisplayName_lang'] for race, line in RACIAL_LINES.items()},
                         {1: 'Racial - Human', 3: 'Dwarven Racial'})
        own = {race: {r['spell'] for r in rows} for race, rows in self.data['racials'].items()}
        self.assertEqual(own, {'1': {20597, 20598, 20599, 20600, 20864}, '3': {2481, 20594, 20595, 20596}})
        for race, spells in own.items():
            self.assertLessEqual(spells, set(manifest[race]['create_spells']))   # playercreateinfo_spell gives them
        self.assertEqual(set(self.racials), set(TREATMENTS))
        self.assertLessEqual(set(self.racials), set(RACIAL_SPELLS.values()))   # the catalog carries their rows
        for sid, r in self.racials.items():
            with self.subTest(spell=sid):
                self.assertEqual(RACIAL_SPELLS[r['key']], sid)
                # Delivered by service/racials.py and the plugin's carriers, but Find Treasure's minimap markers.
                self.assertEqual(r['service']['runtime'], 'pending' if sid == 2481 else 'delivered')
                self.assertIn(r['service']['skyrim'], ('converted', 'native', 'informational'))
                self.assertEqual(r['passive'], sid not in (20594, 20600, 2481))

    def test_treatments_read_the_client_effects(self):
        s = {sid: r['service'] for sid, r in self.racials.items()}
        self.assertEqual((s[20598]['type'], s[20598]['stat'], s[20598]['amount']), ('stat_percent', 'spi', 5))
        self.assertEqual((s[20599]['type'], s[20599]['amount'], s[20599]['skyrim']), ('reputation_gain', 10, 'informational'))
        self.assertEqual((s[20600]['amount'], s[20600]['duration_ms'], s[20600]['cooldown_ms']), (50, 20000, 180000))
        self.assertEqual([(k['skill'], k['name'], k['classes']) for k in s[20597]['skills']],
                         [(43, 'Swords', [2]), (55, 'Two-Handed Swords', [2])])
        self.assertEqual([(k['skill'], k['classes']) for k in s[20864]['skills']], [(54, [2]), (160, [2])])
        # A Paladin has no Guns skill (SkillRaceClassInfo): Gun Specialization never applies.
        self.assertEqual(([(k['skill'], k['classes']) for k in s[20595]['skills']], s[20595]['skyrim']),
                         ([(46, [])], 'informational'))
        self.assertTrue(all(k['classes'] for sid in (20597, 20864) for k in s[sid]['skills']))
        self.assertEqual((s[20596]['type'], s[20596]['amount'], s[20596]['school_mask']), ('resistance', 10, 1 << 4))
        self.assertEqual((s[2481]['type'], s[2481]['lock_type']), ('tracking', 6))
        self.assertEqual(set(PENDING), {2481})
        stone = s[20594]
        self.assertEqual((stone['armor_percent'], stone['dispel_immunity'], stone['mechanic_immunity']), (10, [3, 4], [15]))
        self.assertEqual((stone['duration_ms'], stone['cooldown_ms']), (8000, 180000))
        self.assertEqual([l['spell'] for l in self.racials[20594]['linked']], [20612])

    def test_evidence_cites_the_pinned_source(self):
        cited = self.data['provenance']['cmangos_sha256']
        refs = [e for r in self.racials.values() for e in r['evidence']] + \
            [e for v in self.data['evidence'].values() for e in v]
        for ref in refs:
            path, line = ref.rsplit(':', 1)
            self.assertIn(path, cited)
            self.assertLessEqual(int(line), len((CMANGOS / path).read_text(errors='replace').splitlines()))
        line = lambda ref: (CMANGOS / ref.rsplit(':', 1)[0]).read_text(errors='replace').splitlines()[int(ref.rsplit(':', 1)[1]) - 1]
        self.assertIn('case 20594:', line(self.racials[20594]['evidence'][3]))
        self.assertIn('GetPlayerLevelInfo', line(self.data['evidence']['levelstats'][0]))


if __name__ == '__main__':
    unittest.main()
