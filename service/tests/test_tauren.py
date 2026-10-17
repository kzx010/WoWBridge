"""Tauren Classic client/CMaNGOS values, including old-catalog supplementation."""
import json
import unittest

from service import character, feral, racials
from service.catalog import Catalog
from service.server import Service


class Tauren(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()

    def setUp(self):
        self.svc = Service(self.cat, None)

    def answer(self, spell=20549, race=6, **fields):
        req = {'op': 'resolve', 'spell': spell, 'caster': {'race': race}, 'target': {'kind': 'self'}, **fields}
        return self.svc.respond(json.dumps(req).encode())

    def test_stomp_timings_radius_cap_and_damage_does_not_break(self):
        out = self.answer()
        self.assertTrue(out['ok'], out)
        self.assertEqual((out['cost'], out['cast_ms'], out['gcd_ms'], out['cooldown_ms']), (0, 500, 0, 120000))
        aura = out['auras'][0]
        self.assertEqual((aura['type'], aura['duration_ms'], aura['breaks_on_damage']), ('stun', 2000, False))
        self.assertEqual(aura['area'], {'radius_yd': 8, 'hostile': True, 'max_targets': 5, 'channel': False})
        for race in (1, 4):
            self.assertEqual(self.answer(race=race)['error'], 'SPELL_FAILED_NOT_KNOWN')
        self.assertEqual(self.answer(target={'kind': 'unit'})['error'], 'SPELL_FAILED_BAD_TARGETS')
        for form in ('cat', 'bear', 'dire_bear', 'travel', 'aquatic'):
            self.assertEqual(self.answer(caster={'race': 6, 'form': form})['error'], 'SPELL_FAILED_NOT_SHAPESHIFT')

    def test_stomp_each_victim_rolls_at_impact(self):
        targets = [{'id': i, 'level': 60, 'distance_yd': 8} for i in range(8)]
        out = self.answer(targets=targets, seed=4)
        self.assertTrue(out['ok'], out)
        self.assertEqual(len(out['results']), 5)
        self.assertEqual(out['auras'], [])
        for result in out['results']:
            self.assertTrue(result['auras'] or result['events'])
        # Unequal rolls for equal victims: the seed is advanced, not reused per target.
        seen = set()
        for seed in range(50):
            hit = self.answer(targets=targets[:5], seed=seed)['results']
            seen.add(tuple(bool(r['auras']) for r in hit))
        self.assertTrue(any(any(row) and not all(row) for row in seen))
        invalid = [{'id': 1, 'level': 60, 'distance_yd': 8.01},
                   {'id': 2, 'level': 60, 'friendly': True}, {'id': 3, 'level': 60, 'dead': True},
                   {'id': 4, 'level': 60, 'los': False}, {'id': 5, 'level': 60, 'attackable': False}]
        self.assertEqual(self.answer(targets=invalid)['results'], [])
        eligible = self.answer(targets=invalid + targets, seed=4)['results']
        self.assertEqual([r['id'] for r in eligible], list(range(5)))

    def test_racial_passives_isolated_deduplicated_and_cannot_cast(self):
        foreign = [20582, 20583, 20585, 20550, 20551, 20552] * 2
        for race, own in racials.PASSIVES_BY_RACE.items():
            for form in ('none', 'cat', 'bear', 'dire_bear', 'travel', 'aquatic'):
                ch = character.build(self.cat, [], race=race, form=form, auras=foreign)
                self.assertEqual([a['spell'] for a in ch.auras if a['spell'] in racials.ALL_PASSIVES], [sid for sid in own if sid != 20585])
                self.assertEqual(ch.total(49), 1 if race == 4 else 0)
                self.assertEqual(ch.resistance(3), 10)
            for sid in own:
                self.assertEqual(self.answer(spell=sid, race=race)['error'], 'SPELL_FAILED_NOT_KNOWN')
        self.assertEqual(self.answer(spell=20580)['error'], 'SPELL_FAILED_NOT_KNOWN')

    def test_endurance_is_five_percent_of_total_health(self):
        for items in ([], [16897, 16898, 16899, 16900, 16901, 16902, 16903, 16904]):
            for form in ('none', 'bear', 'dire_bear'):
                ch = character.build(self.cat, items, race=6, form=form)
                self.assertEqual(racials.health_multiplier(ch), 1.05)
                bare = ch.base_health + ch.item_health + 20 + (ch.stats['sta'] - 20) * 10 + ch.total(34)
                self.assertEqual(ch.max_health, int(bare * 1.05 + .5))
        self.assertEqual(character.build(self.cat, [], race=6).max_health, 2124)
        self.assertEqual(character.build(self.cat, [], race=4).max_health, 1993)

    def test_cultivation_and_nature_resistance_metadata(self):
        out = self.svc.respond(json.dumps({'op': 'stats', 'caster': {'race': 6, 'skyrim': {}}}).encode())
        self.assertTrue(out['ok'], out)
        ps = {a['spell']: a for a in out['racials']}
        self.assertEqual(set(ps), {20550, 20551, 20552})
        self.assertEqual(ps[20550]['native_max_health_percent'], 5)
        self.assertEqual((ps[20552]['type'], ps[20552]['skill'], ps[20552]['amount']), ('skill', 182, 15))
        self.assertTrue(ps[20552]['informational'])
        self.assertNotIn('skyrim', ps[20552])
        self.assertAlmostEqual(ps[20551]['skyrim'][0]['magnitude'], 10 / 3)
        self.assertEqual((out['resist']['3'], out['dodge_bonus']), (10, 0))
        ch = character.build(self.cat, [], race=6, auras=[9885, 20551])
        self.assertEqual(ch.resistance(3), 30)

    def test_race_stats_and_form_displays(self):
        ch = character.build(self.cat, [], race=6)
        self.assertEqual(ch.stats, {'str': 70, 'agi': 55, 'sta': 72, 'int': 95, 'spi': 112})
        for race, displays in ((4, feral.DISPLAY), (6, feral.TAUREN_DISPLAY)):
            for form in ('cat', 'bear', 'dire_bear', 'travel', 'aquatic'):
                ch = character.build(self.cat, [], race=race, form=form)
                self.assertEqual(feral.profile(self.cat, ch)['display_id'], displays[form])
            from service.talents import Talents
            tree = Talents().data['classes']['druid']['trees'][0]
            ranks = {t['id']: t['max_rank'] for t in tree['talents'] if t['tier'] < 6}
            ranks[793] = 1
            ch = character.build(self.cat, [], race=race, level=100, form='moonkin', talents=ranks)
            self.assertEqual(feral.profile(self.cat, ch)['display_id'], displays['moonkin'])

    def test_starting_outfits_are_the_client_rows(self):
        rows = racials.data()['starting_outfits']
        self.assertEqual(len(rows), 4)
        for race, entries, displays in ((4, [3661, 6123, 6124], [18530, 12683, 9987]),
                                        (6, [35, 6139, 6124], [472, 12684, 9987])):
            for sex in (0, 1):
                row = next(r for r in rows if r['RaceID'] == race and r['SexID'] == sex)
                self.assertEqual(row['ItemID'][:3], entries)
                self.assertEqual(row['DisplayItemID'][:3], displays)

    def test_war_stomp_stuns_a_target(self):
        # With a talent allocation the talent pass reads the cast's per-target results, which carry no 'type'.
        out = self.answer(caster={'race': 6, 'class': 11, 'level': 60, 'talents': {}}, seed=1,
                          targets=[{'id': 1, 'kind': 'unit', 'level': 1, 'distance_yd': 3, 'is_player': False}])
        self.assertTrue(out['ok'], out)
        self.assertTrue(any(a['type'] == 'stun' and a['duration_ms'] == 2000 for a in out['results'][0]['auras']), out)

    def test_war_stomp_resolves_in_the_games_request_shape(self):
        # The plugin sends her Skyrim skills (so the Skyrim conversion runs) and the units it finds around her, which
        # carry no kind: the stun lands on each as a result without a type.
        out = self.answer(caster={'race': 6, 'class': 11, 'level': 2, 'talents': {},
                                  'skyrim': {'alteration': 15.0, 'destruction': 15.0, 'restoration': 15.0}},
                          seed=1, targets=[{'id': 4278192384, 'level': 2, 'distance_yd': 3.1, 'is_player': False, 'armor_reduction': 0.0}])
        self.assertTrue(out['ok'], out)
        self.assertTrue(any(a['type'] == 'stun' and a['duration_ms'] == 2000 for a in out['results'][0]['auras']), out)
