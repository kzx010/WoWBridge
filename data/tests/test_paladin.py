"""The Paladin manifest: every rank with its Spell.dbc row and provenance, and its link closure in the catalog."""
import json
import unittest

from data import dbc
from data.build_catalog import DBC_DIR, RACIAL_LINKED_SPELLS, RACIAL_SPELLS, build as build_catalog, character_stats
from data.build_paladin import CMANGOS, DEFAULT_OUT, build

LINES = {'594', '267', '184'}          # Holy, Protection, Retribution


def contained(old, new):
    """Whether everything in old is in new unchanged (dicts by key, row lists by row)."""
    if isinstance(old, dict):
        return isinstance(new, dict) and all(k in new and contained(v, new[k]) for k, v in old.items())
    if isinstance(old, list) and isinstance(new, list) and len(old) != len(new):
        return all(row in new for row in old)
    return old == new


class PaladinManifest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = json.loads(DEFAULT_OUT.read_text())
        cls.built = build()
        cls.catalog = build_catalog()
        cls.ranks = {r['spell']: r for r in cls.m['ranks']}
        cls.spells = {int(k): v for k, v in cls.m['spells'].items()}
        cls.links = {(l['from'], l['to'], l['kind']) for l in cls.m['links']}
        cls.progression = json.loads((DEFAULT_OUT.parent / 'progression.json').read_text())['classes']
        cls.talents = json.loads((DEFAULT_OUT.parent / 'talents.json').read_text())['classes']

    def test_checked_in_manifest_matches_builder_and_inputs(self):
        self.assertEqual(self.built, self.m)
        self.assertEqual(DEFAULT_OUT.read_text(), json.dumps(self.built, indent=1, sort_keys=True) + '\n')

    def test_every_rank_and_talent_rank_with_effects_and_provenance(self):
        book = self.progression['paladin']
        self.assertEqual(set(self.ranks), {r['spell'] for r in book['spells'] + book['excluded']})
        trees = self.talents['paladin']['trees']
        talents = {t['id']: t for tree in trees for t in tree['talents']}
        talent_ranks = {sid for t in talents.values() for sid in t['rank_spells']}
        self.assertEqual((len(self.ranks), len(talents), len(talent_ranks)), (284, 44, 140))
        self.assertLessEqual(talent_ranks | {int(s) for s in self.talents['paladin']['trainer_options']}, set(self.ranks))
        sources = {}
        for sid, r in self.ranks.items():
            with self.subTest(spell=sid):
                sources[r['source']] = sources.get(r['source'], 0) + 1
                self.assertTrue(self.spells[sid]['effects'])
                p = r['provenance']
                if r['source'] in ('trainer', 'talent_trainer'):
                    self.assertTrue(p['trainer'] and all(o['trainer_spell'] for o in p['trainer']))
                if r['source'] == 'trainer':
                    self.assertEqual(min(o['required_level'] for o in p['trainer']), r['required_level'])
                if r['source'] == 'starting':
                    self.assertEqual(p['starting_races'], [1, 3])
                if r['source'] == 'quest':
                    self.assertTrue(all(str(q['quest']) in self.m['quests'] for q in p['quests']))
                if r['source'] == 'item':
                    self.assertTrue(p['items'] and all(i['item'] in self.m['items'] for i in p['items']))
                if r['source'] == 'talent':
                    t = talents[p['talent']['id']]
                    self.assertEqual(t['rank_spells'][p['talent']['rank'] - 1], sid)
                if r['source'] == 'talent_trainer':
                    self.assertEqual(talents[p['talent_chain']]['rank_spells'][0], r['chain'])
        self.assertEqual(sources, {'trainer': 120, 'starting': 2, 'quest': 4, 'item': 3, 'talent': 140,
                                   'talent_trainer': 15})
        # Spot checks against the client rows.
        self.assertEqual([(e['effect'], e['base_points']) for e in self.spells[635]['effects']], [(77, 38)])
        self.assertEqual(self.spells[635]['power']['cost'], 35)
        self.assertEqual([e['effect'] for e in self.spells[10310]['effects']], [67, 30])
        self.assertEqual(self.spells[25898]['reagents'], [{'item': 21177, 'count': 1}])     # Symbol of Kings
        self.assertEqual(self.spells[19752]['reagents'], [{'item': 17033, 'count': 1}])     # Symbol of Divinity
        self.assertEqual(self.spells[20473]['category_cooldown_ms'], 30000)
        # Auras sit on the stance bar (AttributesExB 0x10, SpellDefines.h USE_SHAPESHIFT_BAR); seals do not.
        auras = [s for s, r in self.ranks.items() if any(e['effect'] == 35 for e in self.spells[s]['effects'])]
        self.assertTrue(auras and all(self.spells[s]['attributes'][2] & 0x10 for s in auras))
        self.assertFalse(self.spells[20271]['attributes'][2] & 0x10 or self.spells[21084]['attributes'][2] & 0x10)
        self.assertEqual((self.spells[7294]['stance_bar_order'], self.spells[19746]['stance_bar_order']), (1, 2))

    def test_named_code_links(self):
        has = lambda source, to, kind: (source, to, kind) in self.links
        for sid in (498, 5573, 642, 1020, 1022, 5599, 10278):           # Divine Protection/Shield, Protection
            self.assertTrue(has(sid, 25771, 'precast'), sid)
        for sid, (harm, heal) in {20473: (25912, 25914), 20929: (25911, 25913), 20930: (25902, 25903)}.items():
            self.assertTrue(has(sid, harm, 'dummy') and has(sid, heal, 'dummy'))
        holy_light = [s for s, r in self.ranks.items() if r['chain'] == 635]
        flash = [s for s, r in self.ranks.items() if r['chain'] == 19750]
        self.assertEqual((len(holy_light), len(flash)), (9, 6))
        self.assertTrue(all(has(s, 19968, 'script_effect') for s in holy_light))
        self.assertTrue(all(has(s, 19993, 'script_effect') for s in flash))
        seals = [s for s in self.ranks if self.spells[s]['family'] == 10 and self.spells[s]['family_flags'] & 0x0A000200]
        self.assertEqual(len(seals), 28)
        judged = {l['from']: l['to'] for l in self.m['links'] if l['kind'] == 'judgement'}
        self.assertEqual(set(judged), set(seals) - {20154})
        self.assertEqual(sum(l['kind'] == 'dummy_proc' and l['from'] in seals for l in self.m['links']), 9)   # SoR
        follow = {l['from']: l['to'] for l in self.m['links'] if l['kind'] in ('dummy', 'script')}
        payloads = {self.spells[s]['name']: [] for s in seals}
        for seal, judgement in judged.items():
            payloads[self.spells[seal]['name']].append(follow.get(judgement, judgement))
        self.assertEqual(sorted(payloads['Seal of Command']), [20467, 20963, 20964, 20965, 20966])   # damage
        self.assertEqual(sorted(payloads['Seal of Light']), [20267, 20341, 20342, 20343])              # heal
        self.assertEqual(sorted(payloads['Seal of Wisdom']), [20268, 20352, 20353])                    # mana
        self.assertEqual(payloads['Seal of Justice'], [20184])
        self.assertEqual(sorted(payloads['Seal of the Crusader']), [20188, 20300, 20301, 20302, 20303, 21183])
        self.assertEqual(len(payloads['Seal of Righteousness']), 8)
        for source, to, kind in ((20210, 20272, 'proc_override'), (9799, 25997, 'dummy_proc'), (25988, 25997, 'dummy_proc'),
                                 (13819, 31726, 'db_script'), (23214, 31726, 'db_script'), (13819, 33388, 'learn'),
                                 (23214, 33391, 'learn'), (20177, 20178, 'trigger'), (20049, 20050, 'trigger'),
                                 (20127, 20128, 'trigger'), (9452, 67, 'trigger'), (20234, 20233, 'trigger'),
                                 (19752, 19753, 'trigger'), (20185, 5373, 'trigger')):
            self.assertTrue(has(source, to, kind), (source, to, kind))
        # Mechanics with no payload spell are recorded as handlers with their code.
        shaped = {s for h in self.m['handlers'] for s in h['spells']}
        self.assertLessEqual({6940, 20729, 20925, 20927, 20928, 633, 2800, 10310, 2878, 5627, 10326, 20178}, shaped)
        self.assertEqual({m['spell']: (m['creature'], m['speed_percent']) for m in self.m['mounts']},
                         {13819: (9158, 60), 23214: (14565, 100)})

    def test_code_links_cite_the_pinned_source(self):
        cited = self.m['provenance']['cmangos_sha256']
        for link in self.m['links']:
            evidence = link['evidence']
            if link['source'] == 'dbc':
                self.assertEqual(evidence, [e for e in evidence if e.startswith(f'Spell.dbc {link["from"]} ')])
                continue
            refs = [e for e in evidence if e.startswith(('src/', 'sql/'))]
            self.assertTrue(refs, link)
            for ref in refs:
                path, line = ref.rsplit(':', 1)
                self.assertIn(path, cited)
                self.assertLessEqual(int(line), len((CMANGOS / path).read_text(errors='replace').splitlines()))
        self.assertIn('src/game/Spells/Spell.cpp:3218', next(l for l in self.m['links'] if l['from'] == 642)['evidence'])
        self.assertIn('src/game/Spells/SpellEffects.cpp:1441', next(l for l in self.m['links'] if l['to'] == 25912)['evidence'])
        self.assertEqual({u['ref'] for u in self.m['unresolved']}, {2689, 20154, 38320, 43743, 27158, 34976})

    def test_every_reached_spell_is_in_the_catalog_built_in_memory(self):
        spells = self.catalog['spells']
        self.assertLessEqual(set(self.spells), set(spells))
        for link in self.m['links']:
            self.assertIn(link['to'], spells)
        self.assertLessEqual(set(self.ranks), set(self.catalog['ranks']))

    def test_no_other_class_leaks_in(self):
        abilities = {}
        for a in dbc.read(DBC_DIR / 'SkillLineAbility.dbc'):
            abilities.setdefault(a['Spell'], []).append(a)
        for sid, r in self.ranks.items():
            with self.subTest(spell=sid):
                self.assertIn(str(r['skill_line']), LINES)
                self.assertTrue(all(not a['ClassMask'] or a['ClassMask'] & 2 for a in abilities[sid]))
        others = {row['spell'] for key, c in self.progression.items() if key != 'paladin'
                  for row in c['spells'] + c['excluded']}
        others |= {sid for key, c in self.talents.items() if key != 'paladin'
                   for tree in c['trees'] for t in tree['talents'] for sid in t['rank_spells']}
        self.assertFalse(set(self.ranks) & others)
        self.assertLessEqual({s['family'] for s in self.spells.values()}, {0, 10})

    def test_human_and_dwarf_only(self):
        races = self.m['class']['races']
        self.assertEqual({k: v['name'] for k, v in races.items()}, {'1': 'Human', '3': 'Dwarf'})
        self.assertEqual(self.m['class']['race_mask'], 0b101)
        for sid, r in self.ranks.items():
            with self.subTest(spell=sid):
                self.assertNotIn('race_mask', r)
                quests = [self.m['quests'][str(q['quest'])] for q in r['provenance'].get('quests', [])]
                if quests:
                    covered = 0
                    for q in quests:
                        self.assertEqual(q['races'] & ~0b101, 0)
                        covered |= q['races']
                    self.assertEqual(covered, 0b101)   # Redemption: one Tome of Divinity a race
                for item in r['provenance'].get('items', []):
                    self.assertEqual(item['allowable_race'], -1)
                    self.assertTrue(item['allowable_class'] & 2)

    def test_catalog_additions_leave_the_druid_catalog_unchanged(self):
        before = build_catalog(manifests=())
        self.assertTrue(contained(before, self.catalog))
        added = set(self.catalog['spells']) - set(before['spells'])
        self.assertLessEqual(added, set(self.spells))
        self.assertIn(25771, added)                  # Forbearance came only from the manifest
        self.assertEqual(before['ranks'], self.catalog['ranks'])

    def test_level_stats_for_every_playable_class(self):
        c = self.catalog['character']
        by_class, base = c['levelstats_by_class'], c['classlevelstats_by_class']
        self.assertEqual({cls: sorted(races) for cls, races in by_class.items()}, {11: [4, 6], 2: [1, 3]})
        for cls, races in by_class.items():
            self.assertEqual(sorted(base[cls]), list(range(1, 61)))
            self.assertTrue(all(r['class'] == cls and r['level'] == n for n, r in base[cls].items()))
            for race, levels in races.items():
                self.assertEqual(sorted(levels), list(range(1, 61)))
                self.assertTrue(all((r['class'], r['race'], r['level']) == (cls, race, n) for n, r in levels.items()))
        # The Druid's keys are her class's tables (the Night Elf's level 60 as before), beside the class-aware ones.
        self.assertEqual(c['levelstats_by_race'], by_class[11])
        self.assertEqual(c['levelstats_by_level'], by_class[11][4])
        self.assertEqual(c['classlevelstats_by_level'], base[11])
        self.assertEqual((c['levelstats']['inte'], c['levelstats']['spi'], c['classlevelstats']['basemana']), (100, 110, 1244))
        # player_classlevelstats / player_levelstats for the Paladin (Player::InitStatsForLevel reads them).
        self.assertEqual([(base[2][n]['basehp'], base[2][n]['basemana']) for n in (1, 60)], [(28, 59), (1381, 1512)])
        stats = ('str', 'agi', 'sta', 'inte', 'spi')
        self.assertEqual([by_class[2][1][60][k] for k in stats], [105, 65, 100, 70, 78])   # Human
        self.assertEqual([by_class[2][3][60][k] for k in stats], [107, 61, 103, 69, 74])   # Dwarf
        rows = [r for races in by_class.values() for levels in races.values() for r in levels.values()]
        missing = [r for r in rows if not (r['class'], r['race'], r['level']) == (2, 3, 30)]
        with self.assertRaises(ValueError):
            character_stats(missing, list(base[2].values()) + list(base[11].values()))

    def test_human_and_dwarf_racials_are_in_the_catalog(self):
        self.assertEqual(self.catalog['racials'], RACIAL_SPELLS)
        racials = {r['spell'] for rows in racial_data()['racials'].values() for r in rows}
        self.assertLessEqual(racials | set(RACIAL_LINKED_SPELLS), set(self.catalog['spells']))
        self.assertEqual(self.catalog['spells'][20612]['EffectApplyAuraName1'], 77)   # Stoneform's bleed immunity


def racial_data():
    return json.loads((DEFAULT_OUT.parent / 'racial_data.json').read_text())


if __name__ == '__main__':
    unittest.main()
