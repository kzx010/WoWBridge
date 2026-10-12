"""Talent allocation, DBC provenance and the plugin's stateless combat cycle."""
import json
import unittest

from data.build_talents import DEFAULT_OUT, build
from service.catalog import Catalog
from service.server import Service
from service.talents import Talents, points

BALANCE = {762: 5, 763: 5, 791: 5, 781: 3, 782: 2, 789: 1, 784: 4, 790: 5, 793: 1}
FERAL = {796: 5, 794: 5, 807: 2, 798: 3, 803: 3, 800: 2, 805: 2, 808: 5, 1162: 1, 799: 2, 809: 1}
RESTORATION = {821: 1, 822: 5, 824: 5, 829: 3, 827: 1, 830: 3, 843: 5, 828: 5, 831: 1, 825: 1, 844: 1}
FULL_BUILD = {**BALANCE, 796: 5, 794: 5, 807: 2, 798: 3, 822: 5}
TARGET = {'kind': 'unit', 'level': 60, 'distance_yd': 20, 'reaction': 'hostile', 'alive': True, 'id': 7}


class TalentRules(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.book = Talents()
        cls.cat = Catalog()

    def setUp(self):
        self.svc = Service(self.cat, None)

    def snapshot(self, ranks=None, level=60, **extra):
        return self.svc.handle({'op': 'talents', 'class': 'druid',
                                'caster': {'level': level, 'talents': ranks or {}}, **extra})

    def cast(self, spell=9912, ranks=None, seed=1, **caster):
        return self.svc.handle({'op': 'resolve', 'spell': spell, 'seed': seed,
                                'caster': {'level': 60, 'talents': ranks or {}, **caster},
                                'target': {'kind': 'self'} if spell in (24858, 768, 5487, 17116, 16864) else TARGET})

    def talent(self, snap, tid):
        return next(t for tree in snap['trees'] for t in tree['talents'] if t['id'] == tid)

    def test_points_follow_character_level_without_skill_curve_or_cap(self):
        for level, expected in ((1, 0), (9, 0), (10, 1), (11, 2), (60, 51), (70, 61)):
            self.assertEqual(points(level), expected)
            self.assertEqual(self.snapshot(level=level)['points_available'], expected)
        for value in (True, 1.5, '60', float('inf'), 0):
            with self.assertRaises(ValueError):
                points(value)

    def test_level_changes_keep_spent_points_and_teach_active_and_passive_spells(self):
        ranks = {821: 5, 823: 5, 827: 1}
        for level in (20, 21, 60, 70):
            snap = self.snapshot(ranks, level=level)
            self.assertEqual(snap['points_available'], level - 9 - 11)
        caster = {'level': 60, 'talents': ranks}
        out = self.svc.handle({'op': 'known_spells', 'class': 'druid', 'caster': caster})
        self.assertIn(5570, out['known_spells'])
        self.assertIn(17055, out['known_spells'])
        rows = {row['spell']: row for row in out['spells']}
        self.assertFalse(rows[5570]['passive'])
        self.assertTrue(rows[17055]['passive'])
        self.assertTrue(self.cast(5570, ranks)['auras'])
        refused = self.svc.respond(json.dumps({'op': 'resolve', 'spell': 17055, 'caster': caster,
                                               'target': {'kind': 'self'}}).encode())
        self.assertFalse(refused['ok'])
        self.assertEqual(refused['error'], 'SPELL_FAILED_SPELL_UNAVAILABLE')
        reset = self.svc.handle({'op': 'known_spells', 'class': 'druid',
                                 'caster': {**caster, 'talents': {}}, 'since': out['known_spells']})
        self.assertNotIn(5570, reset['known_spells'])
        self.assertNotIn(17055, reset['known_spells'])

    def test_mark_tooltips_match_each_rank_effect_and_native_carrier(self):
        for spell in (1126, 5232, 6756, 5234, 8907, 9884, 9885, 21849, 21850):
            reagents = {str(self.cat.spell(spell)[f'Reagent{e}']): 100 for e in range(1, 9)
                        if self.cat.spell(spell).get(f'Reagent{e}', 0)}
            base = self.svc.handle({'op': 'resolve', 'spell': spell, 'caster': {'level': 60, 'talents': {}, 'reagents': reagents},
                                    'target': {'kind': 'self'}})
            for rank in range(1, 6):
                caster = {'level': 60, 'talents': {821: rank}, 'skyrim': {'alteration': 100}, 'reagents': reagents}
                out = self.svc.handle({'op': 'resolve', 'spell': spell, 'caster': caster, 'target': {'kind': 'self'}})
                for index, aura in enumerate(out['auras']):
                    amount = int(base['auras'][index]['amount'] * (100 + 7 * rank) / 100)
                    self.assertEqual(aura['amount'], amount)
                    self.assertEqual(out['tooltip']['effects'][index], [amount, amount])
                    self.assertTrue(aura['skyrim'])
                if spell == 9885 and rank == 5:
                    self.assertEqual([a['amount'] for a in out['auras']], [384, 16, 27])
                    native = {a['type']: a['skyrim'] for a in out['auras']}
                    self.assertAlmostEqual(native['armor'][0]['magnitude'], 384 * 40 / 285)
                    self.assertAlmostEqual(native['stat'][0]['magnitude'], 16 * 20 / 12)
                    profile = self.svc.handle({'op': 'skyrim_costs', 'caster': caster})['spell_tooltips']['9885']
                    self.assertEqual(profile, out['tooltip'])
                    book = self.svc.handle({'op': 'known_spells', 'class': 'druid',
                                           'caster': {**caster, 'skyrim': {'alteration': 100}}})
                    self.assertEqual(book['spell_tooltips']['9885'], profile)
        plain = self.svc.char({'level': 60, 'talents': {}, 'auras': [9885]})
        improved = self.svc.char({'level': 60, 'talents': {821: 5}, 'auras': [9885]})
        self.assertEqual(improved.stats['sta'] - plain.stats['sta'], 4)

    def test_mark_native_passive_baseline_does_not_duplicate_cast_strength(self):
        # Forms keeps Improved Mark in the passive carrier's comparison profile:
        # the cast carrier already owns its gain, using 20/12 rather than STAT's 1.5.
        allocation = {795: 5, 799: 5, 807: 2, 821: 5, 823: 5, 827: 1}
        for spell in (9885, 21850):
            for rank in range(6):
                with self.subTest(spell=spell, rank=rank):
                    ranks = allocation if rank == 5 else {821: rank}
                    caster = {'class': 11, 'level': 40, 'talents': ranks, 'auras': [spell]}
                    profile = self.svc.handle({'op': 'stats', 'caster': caster})
                    baseline = self.svc.handle({'op': 'stats', 'caster': {**caster, 'talents': {821: rank}}})
                    self.assertEqual(profile['stats'], baseline['stats'])
                    untalented = self.svc.handle({'op': 'stats', 'caster': {**caster, 'talents': {}}})
                    if rank == 5:
                        for stat in ('sta', 'int'):
                            self.assertEqual((profile['stats'][stat] - untalented['stats'][stat]) * 1.5, 6)
                    # A genuine passive stat increase must still reach its carrier,
                    # including its interaction with Mark's strengthened intellect.
                    hotw = self.svc.handle({'op': 'stats', 'caster': {**caster, 'level': 60,
                                                                    'talents': {**FERAL, 821: rank}}})
                    baseline60 = self.svc.handle({'op': 'stats', 'caster': {**caster, 'level': 60,
                                                                          'talents': {821: rank}}})
                    self.assertEqual(hotw['stats']['int'], int(baseline60['stats']['int'] * 1.2))
                    self.assertGreater(hotw['stats']['int'] - baseline60['stats']['int'], 0)

    def test_tooltip_modifiers_follow_cast_time_and_periodic_healing(self):
        wrath = self.cast(ranks={762: 5})
        self.assertEqual(wrath['tooltip']['cast_ms'], wrath['cast_ms'])
        charged = self.cast(ranks=BALANCE, auras=[16886])
        self.assertEqual(charged['tooltip']['cast_ms'], charged['cast_ms'])
        hot = self.svc.handle({'op': 'resolve', 'spell': 9839, 'caster': {'level': 60, 'talents': RESTORATION},
                               'target': {'kind': 'self'}})
        eff = hot['auras'][0]['eff']
        self.assertEqual(hot['tooltip']['effects'][eff - 1], [hot['auras'][0]['per_tick']] * 2)
        reach = self.cast(ranks={762: 5, 763: 5, 764: 2})
        self.assertEqual(reach['tooltip']['range_max'], 36)
        req = {'op': 'resolve', 'spell': 9912, 'caster': {'level': 60, 'talents': {762: 5, 763: 5, 764: 2}},
               'target': {**TARGET, 'distance_yd': 35}}
        self.assertTrue(self.svc.respond(json.dumps(req).encode())['ok'])

    def test_tier_and_prerequisite_and_one_rank_learning(self):
        snap = self.snapshot()
        self.assertIn('tier_points', self.talent(snap, 763)['reasons'])
        self.assertIn('prerequisite', self.talent(snap, 921)['reasons'])
        snap = self.snapshot({762: 5})
        self.assertTrue(self.talent(snap, 763)['learnable'])
        learned = self.snapshot({}, level=10, learn={'talent': 762, 'rank': 1})
        self.assertEqual(learned['talents'], {'762': 1})
        self.assertEqual(learned['learned'], {'talent': 762, 'rank': 1, 'spell': 16814})
        self.assertEqual(learned['points_available'], 0)
        self.assertIn('no_points', self.talent(learned, 762)['reasons'])
        for learn in ({'talent': 762, 'rank': 3}, {'talent': 763}, {'talent': 921}, {'talent': 762.0}):
            with self.assertRaises(ValueError):
                self.snapshot(learn=learn)
        with self.assertRaises(ValueError):
            self.snapshot({762: 5}, learn={'talent': 762})

    def test_saved_allocations_cannot_bypass_gates(self):
        for ranks in ({763: 5}, {793: 1}, {796: 5, 794: 5, 798: 3, 800: 1}, {26: 1}, {762: True}, {'762': 6}):
            with self.subTest(ranks=ranks), self.assertRaises(ValueError):
                self.snapshot(ranks)
        with self.assertRaises(ValueError):
            self.snapshot({762: 2}, level=10)

    def test_full_51_point_build_and_request_independence(self):
        snap = self.snapshot(FULL_BUILD)
        self.assertEqual((snap['points_spent'], snap['points_available']), (51, 0))
        self.assertEqual([t['points_spent'] for t in snap['trees']], [31, 15, 5])
        self.assertFalse(any(t['learnable'] for tree in snap['trees'] for t in tree['talents']))
        self.assertEqual(self.snapshot()['points_spent'], 0)
        self.assertEqual(self.snapshot(FERAL)['points_spent'], 31)
        self.assertEqual(self.snapshot(RESTORATION)['points_spent'], 31)

    def test_all_classes_client_layout_icons_backgrounds(self):
        self.assertEqual(len(self.book.data['classes']), 9)
        for key, definition in self.book.data['classes'].items():
            snap = self.book.handle({'class': definition['id'], 'caster': {'level': 10}})
            self.assertEqual(snap['class'], key)
            self.assertEqual(len(snap['trees']), 3)
            for tree in snap['trees']:
                self.assertTrue(tree['icon'])
                self.assertEqual(set(tree['background_textures']), {'TopLeft', 'TopRight', 'BottomLeft', 'BottomRight'})
                for t in tree['talents']:
                    self.assertIn(t['column'], range(4))
                    self.assertIn(t['tier'], range(7))
                    self.assertEqual(t['max_rank'], len(t['rank_spells']))
                    self.assertTrue(t['icon'])
                    for sid in t['rank_spells']:
                        self.assertIn(sid, self.cat.spells)

    def test_checked_in_data_is_deterministic_and_matches_authority(self):
        generated = build()
        encoded = json.dumps(generated, indent=2, sort_keys=True) + '\n'
        self.assertEqual(DEFAULT_OUT.read_text(), encoded)
        self.assertEqual(self.book.data['provenance'], generated['provenance'])

    def test_feline_swiftness_outdoor_speed_and_cache(self):
        ranks = {796: 5, 794: 5, 807: 2}
        def profile(form, **extra):
            return self.svc.handle({'op': 'stats', 'caster': {'talents': ranks, 'form': form, **extra}})['form']
        self.assertEqual(profile('cat')['movement_speed_percent'], 30)
        self.assertEqual(profile('cat', indoors=True)['movement_speed_percent'], 0)
        self.assertEqual(profile('travel')['movement_speed_percent'], 40)
        self.assertEqual(profile('bear')['movement_speed_percent'], 0)
        self.assertEqual(self.cast(768, ranks)['auras'][0]['movement_speed_percent'], 30)
        self.assertEqual(self.svc.handle({'op': 'stats', 'caster': {'form': 'cat'}})['form']['movement_speed_percent'], 0)

    def test_improved_wrath_and_natures_grace_proc_consumption(self):
        self.assertEqual(self.cast(ranks={762: 5})['cast_ms'], 1500)
        critical = next(self.cast(ranks=BALANCE, seed=seed) for seed in range(300)
                        if self.cast(ranks=BALANCE, seed=seed)['events'][0].get('crit'))
        proc = next(p for p in critical['procs'] if p['spell'] == 16886)
        self.assertEqual(proc['auras'][0]['amount'], -500)
        self.assertEqual(proc['target'], 'caster')
        following = self.cast(ranks=BALANCE, auras=[16886])
        self.assertEqual(following['cast_ms'], 1000)
        self.assertEqual(following['consumed_auras'], [16886])
        self.assertEqual(self.cast(9835, BALANCE, auras=[16886]).get('consumed_auras'), None)
        self.assertEqual(self.cast(ranks=BALANCE)['cast_ms'], 1500)

    def test_moonkin_learning_and_casting(self):
        with self.assertRaisesRegex(Exception, 'NOT_KNOWN'):
            self.cast(24858)
        out = self.cast(24858, BALANCE)
        aura = out['auras'][0]
        self.assertEqual((aura['form'], aura['armor_percent'], aura['display_id']), ('moonkin', 360, 15374))
        self.assertIn(24858, self.svc.handle({'op': 'known_spells', 'class': 'druid',
                                            'caster': {'level': 60, 'talents': BALANCE}})['known_spells'])
        plain = self.svc.char({'talents': BALANCE})
        shifted = self.svc.char({'talents': BALANCE, 'form': 'moonkin'})
        self.assertAlmostEqual(shifted.spell_crit(3) - plain.spell_crit(3), 3)
        self.assertIn('cost', self.cast(ranks=BALANCE, form='moonkin'))
        with self.assertRaisesRegex(Exception, 'NOT_SHAPESHIFT'):
            self.cast(9889, BALANCE, form='moonkin')

    def test_trainer_upgrades_require_talent_and_skill_progression(self):
        ranks = {**FERAL, 761: 1, 822: 5, 824: 5, 827: 1}
        def known(skills):
            return self.svc.handle({'op': 'known_spells', 'class': 'druid',
                                    'caster': {'level': 60, 'skyrim': skills, 'talents': ranks}})
        novice = known({})
        self.assertTrue({16689, 16857, 5570} <= set(novice['known_spells']))
        self.assertFalse({17392, 24977, 17329} & set(novice['known_spells']))
        master = known({'destruction': 100, 'one_handed': 100, 'restoration': 100})
        self.assertTrue({16689, 17329, 16857, 17392, 5570, 24977} <= set(master['known_spells']))
        self.assertEqual(master['highest_ranks']['16857'], 17392)
        without = self.svc.handle({'op': 'known_spells', 'class': 'druid', 'caster': {'level': 60}})
        self.assertFalse({17392, 24977, 17329} & set(without['known_spells']))

    def test_required_spell_learning_and_saved_talent_retention(self):
        snap = self.snapshot(known_spells=[])
        self.assertIn('required_spell', self.talent(snap, 761)['reasons'])
        with self.assertRaisesRegex(ValueError, 'required_spell'):
            self.snapshot(known_spells=[], learn={'talent': 761})
        learned = self.snapshot(level=10, known_spells=[339], learn={'talent': 761})
        self.assertEqual(learned['talents'], {'761': 1})
        self.assertEqual(self.snapshot({761: 1}, known_spells=[])['points_spent'], 1)

    def test_feral_charge_resources_range_and_triggered_effects(self):
        req = {'op': 'resolve', 'spell': 16979, 'seed': 1, 'target': {**TARGET, 'distance_yd': 10},
               'caster': {'talents': {796: 5, 794: 5, 804: 1}, 'form': 'bear', 'power': {'rage': 100}}}
        out = self.svc.handle(req)
        self.assertEqual((out['cost'], out['power']), (5, 'rage'))
        self.assertEqual(out['events'][0]['type'], 'charge')
        self.assertTrue(any(a['type'] == 'root' for a in out['auras']))
        self.assertTrue(any(e['type'] == 'interrupt' for e in out['events']))
        req['target']['distance_yd'] = 30
        with self.assertRaisesRegex(Exception, 'OUT_OF_RANGE'):
            self.svc.handle(req)
        with self.assertRaisesRegex(Exception, 'NOT_KNOWN'):
            self.cast(17392, form='cat', power={'energy': 100})

    def test_shaman_granted_proficiencies_are_known(self):
        definition = self.book.data['classes']['shaman']
        talent = next(t for tree in definition['trees'] for t in tree['talents']
                      if t['name'] == 'Two-Handed Axes and Maces')
        ranks = {}
        tree = next(tree for tree in definition['trees'] if talent in tree['talents'])
        for t in sorted(tree['talents'], key=lambda t: (t['tier'], t['id'])):
            if t['tier'] < talent['tier'] and not t['prerequisites'] and sum(ranks.values()) < 10:
                ranks[t['id']] = t['max_rank']
        ranks[talent['id']] = 1
        out = self.svc.handle({'op': 'known_spells', 'class': 'shaman',
                               'caster': {'level': 60, 'talents': ranks}})
        self.assertTrue({16269, 197, 199} <= set(out['known_spells']))

    def test_hotw_predatory_strikes_furor_and_leader(self):
        ranks = {**FERAL, 822: 5}
        cat = self.svc.char({'talents': ranks, 'form': 'cat'})
        bear = self.svc.char({'talents': ranks, 'form': 'bear'})
        self.assertEqual(cat.stats['int'], 120)
        self.assertEqual(cat.stats['str'], 74)
        self.assertEqual(bear.stats['sta'], 82)  # base 69 * 1.20, core truncation
        self.assertEqual(cat.attack_power(), 60 * 3 + 74 * 2 - 20 + 120 + 90 + 65)
        plain = self.svc.char({'form': 'cat'})
        self.assertAlmostEqual(cat.melee_crit() - plain.melee_crit(), 9)
        self.assertEqual(self.cast(768, ranks)['auras'][0]['power_set'], {'energy': 40})
        self.assertEqual(self.cast(5487, ranks)['auras'][0]['power_set'], {'rage': 10})

    def test_blood_frenzy_only_on_eligible_cat_critical_builders(self):
        for seed in range(300):
            out = self.svc.handle({'op': 'resolve', 'spell': 9850, 'seed': seed, 'target': {**TARGET, 'distance_yd': 2},
                                   'caster': {'talents': FERAL, 'form': 'cat', 'power': {'energy': 100}}})
            if out['events'][0].get('crit'):
                self.assertEqual(out['combo_points'], 2)
                self.assertTrue(any(p['spell'] == 16953 for p in out['procs']))
                break
        else:
            self.fail('no critical builder in deterministic sample')

    def test_omen_buff_ppm_cooldown_and_clearcasting(self):
        ranks = {762: 5, 791: 5, 788: 1}
        self.assertEqual(self.cast(16864, ranks)['auras'][0]['type'], 'proc')
        req = {'op': 'melee', 'target': {**TARGET, 'distance_yd': 2},
               'caster': {'talents': ranks, 'auras': [16864], 'items': [873]}}
        procs = [self.svc.handle({**req, 'seed': seed}) for seed in range(200)]
        triggered = next(out for out in procs if out.get('procs'))
        proc = triggered['procs'][0]
        self.assertEqual((proc['spell'], proc['cooldown_ms']), (16870, 3000))
        self.assertEqual(proc['auras'][0]['amount'], -100)
        blocked = {**req, 'caster': {**req['caster'], 'proc_cooldowns': {'16864': 3000}}}
        self.assertTrue(all(not self.svc.handle({**blocked, 'seed': seed}).get('procs') for seed in range(30)))
        free = self.cast(ranks=ranks, auras=[16870])
        self.assertEqual((free['cost'], free['consumed_auras']), (0, [16870]))

    def test_natures_swiftness_charge_and_swiftmend_consumes_shortest_hot(self):
        self.assertEqual(self.cast(17116, RESTORATION)['auras'][0]['amount'], -100)
        heal = self.svc.handle({'op': 'resolve', 'spell': 9889, 'caster': {'talents': RESTORATION, 'auras': [17116]},
                                'target': {'kind': 'self'}})
        self.assertEqual((heal['cast_ms'], heal['consumed_auras']), (0, [17116]))
        req = {'op': 'resolve', 'spell': 18562, 'seed': 1, 'caster': {'talents': RESTORATION},
               'target': {'kind': 'self', 'hots': [{'spell': 9858, 'per_tick': 50, 'remaining_ms': 10000},
                                                 {'spell': 9841, 'per_tick': 100, 'remaining_ms': 1000}]}}
        out = self.svc.handle(req)
        self.assertEqual(out['consumed_target_hot'], 9841)
        self.assertGreaterEqual(out['events'][0]['amount'], 401)
        req['target']['hots'] = []
        with self.assertRaisesRegex(Exception, 'TARGET_AURASTATE'):
            self.svc.handle(req)

    def test_flat_pct_crit_hit_stat_regen_and_damage_auras(self):
        naked = self.svc.char({})
        with_reflection = self.svc.char({'talents': {822: 5, 824: 5, 829: 3}})
        self.assertGreater(with_reflection.regen()['per_sec_casting'], naked.regen()['per_sec_casting'])
        plain = self.cast(seed=1)
        boosted = self.cast(ranks=BALANCE, seed=1)
        self.assertGreater(boosted['events'][0]['amount'], plain['events'][0]['amount'])
        priest = self.svc.handle({'op': 'talents', 'class': 'priest', 'caster': {'level': 60}})
        focused = next(t['id'] for tr in priest['trees'] for t in tr['talents'] if t['name'] == 'Holy Specialization')
        ch = self.svc.char({'class': 'priest', 'talents': {focused: 5}})
        base = self.svc.char({'class': 'priest', 'talents': {}})
        from service import formulas as F
        smite = self.cat.spell(585)
        self.assertAlmostEqual(F.apply_spell_mod(smite, F.SPELLMOD_CRITICAL_CHANCE,
                                                ch.spell_crit(1), ch.mods) - base.spell_crit(1), 5)


if __name__ == '__main__':
    unittest.main()
