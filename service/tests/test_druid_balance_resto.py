"""Every 1.12 Balance/Restoration talent, through the public service protocol.

Each test checks every rank (including non-linear 40/70/100 Roots protection
and 15/30/45/65 Grasp chance), with a legal allocation and an untalented control.
Expected values are independent client/CMaNGOS rules, not generated assertions.
"""
import random
import unittest
from service import formulas as F
from unittest.mock import patch

from service.catalog import Catalog
from service.server import Service
from service.talents import Talents
from service.spells import is_positive

TARGET = {'kind': 'unit', 'level': 60, 'distance_yd': 2, 'reaction': 'hostile', 'alive': True}


class DruidBalanceRestoration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()
        cls.trees = [t for t in Talents().data['classes']['druid']['trees'] if t['name'] != 'Feral Combat']
        cls.talents = {t['id']: (tree, t) for tree in cls.trees for t in tree['talents']}

    def setUp(self):
        self.svc = Service(self.cat, None)

    def ranks(self, tid, rank):
        tree, talent = self.talents[tid]
        ranks = {t['id']: t['max_rank'] for t in tree['talents'] if t['tier'] < talent['tier']}
        for p in talent['prerequisites']:
            ranks[p['talent']] = p['rank']
        if rank:
            ranks[tid] = rank
        return ranks

    def caster(self, tid, rank, **extra):
        return {'level': 100, 'talents': self.ranks(tid, rank), **extra}

    def cast(self, tid, rank, spell, *, seed=1, target=None, **extra):
        if target is None:
            target = {'kind': 'self'} if is_positive(self.cat.spell(spell)) else TARGET
            if spell == 16914:
                target = {'kind': 'ground', 'distance_yd': 2}
        return self.svc.handle({'op': 'resolve', 'spell': spell, 'seed': seed,
                               'caster': self.caster(tid, rank, **extra),
                               'target': target})

    def each(self, tid):
        return range(1, self.talents[tid][1]['max_rank'] + 1)

    def effects(self, tid, spell, percent, index=0, additive_base=100, **extra):
        base = self.cast(tid, 0, spell, **extra)['tooltip']['effects'][index]
        for rank in self.each(tid):
            actual = self.cast(tid, rank, spell, **extra)['tooltip']['effects'][index]
            for b, a in zip(base, actual):
                self.assertAlmostEqual(a, int(b * (additive_base + rank * percent) / additive_base), delta=1)
                self.assertGreaterEqual(a, b)

    def times(self, tid, spell):
        base = self.cast(tid, 0, spell)['cast_ms']
        for rank in self.each(tid):
            out = self.cast(tid, rank, spell)
            self.assertEqual(out['cast_ms'], base - rank * 100)
            self.assertEqual(out['tooltip']['cast_ms'], out['cast_ms'])

    def costs(self, tid, spells, percent, **extra):
        for spell in spells:
            base = self.cast(tid, 0, spell, **extra)['cost']
            ch = self.svc.char(self.caster(tid, 0, **extra))
            from service import formulas as F
            # Other filler talents can contribute additive percent cost mods.
            initial = F.apply_spell_mod(self.cat.spell(spell), 14, 100, ch.mods)
            for rank in self.each(tid):
                value = self.cast(tid, rank, spell, **extra)['cost']
                self.assertAlmostEqual(value, base * (initial - percent * rank) / initial, delta=1)
                self.assertLess(value, base)

    def test_improved_wrath(self):
        self.times(762, 9912)

    def test_natures_grasp(self):
        out = self.cast(761, 1, 16689, target={'kind': 'self'})
        self.assertEqual(out['auras'][0]['duration_ms'], 45000)
        self.assertEqual(out['tooltip']['proc_chance'], 35)
        with self.assertRaisesRegex(Exception, 'ONLY_OUTDOORS'):
            self.svc.handle({'op': 'resolve', 'spell': 16689, 'caster': self.caster(761, 1),
                             'target': {'kind': 'self'}, 'ctx': {'indoors': True}})
        req = {'op': 'incoming_hit', 'weapon_hit': True, 'caster': self.caster(761, 1, auras=[16689]), 'target': TARGET}
        replies = [self.svc.handle({**req, 'seed': seed}) for seed in range(30)]
        self.assertTrue(any(not r.get('procs') for r in replies))
        hit = next(r for r in replies if r.get('procs'))
        self.assertEqual(hit['consumed_auras'], [16689])
        root = next(a for a in hit['procs'][0]['auras'] if a['type'] == 'root')
        self.assertEqual(root['duration_ms'], 12000)
        # Its Entangling Roots shares the cast spell's family flags: IsSingleTargetSpells ends either on the last unit.
        self.assertEqual((root['single_target']['family'], root['single_target']['flags']), (7, 0x200))
        self.assertEqual(hit['procs'][0]['target'], 'target')
        self.assertNotIn('procs', self.svc.handle({**req, 'weapon_hit': False}))
        self.assertNotIn('procs', self.svc.handle({**req, 'caster': self.caster(761, 1)}))

    def test_improved_natures_grasp(self):
        for rank, chance in enumerate((50, 65, 80, 100), 1):
            out = self.cast(921, rank, 16689, target={'kind': 'self'})
            self.assertEqual(out['tooltip']['proc_chance'], chance)
            hit = self.svc.handle({'op': 'incoming_hit', 'weapon_hit': True, 'seed': 1,
                                  'caster': self.caster(921, rank, auras=[16689]), 'target': TARGET})
            self.assertEqual(hit['proc_chance'], chance)
            if rank == 4:
                self.assertEqual(hit['consumed_auras'], [16689])
                for spell in (16689, 16810, 16811, 16812, 16813, 17329):
                    req = {'op': 'incoming_hit', 'weapon_hit': True, 'seed': 1,
                           'caster': self.caster(921, rank, auras=[spell]), 'target': TARGET}
                    self.assertEqual(self.svc.handle(req)['consumed_auras'], [spell])
                for form in ('cat', 'bear', 'dire_bear', 'travel', 'aquatic', 'moonkin'):
                    ranks = self.ranks(921, rank)
                    if form == 'moonkin':
                        ranks = {t['id']: t['max_rank'] for t in self.trees[0]['talents']}
                    req = {'op': 'incoming_hit', 'weapon_hit': True, 'seed': 1, 'target': TARGET,
                           'caster': {'level': 100, 'talents': ranks, 'form': form, 'auras': [16689]}}
                    out = self.svc.handle(req)
                    self.assertEqual(out['consumed_auras'], [16689])
                    self.assertTrue(any(a['type'] == 'root' for a in out['procs'][0]['auras']))

    def test_improved_entangling_roots(self):
        for rank, chance in enumerate((40, 70, 100), 1):
            self.assertEqual(self.cast(787, rank, 9853)['pushback_resist_pct'], chance)
            self.assertEqual(self.cast(787, rank, 9912)['pushback_resist_pct'], 0)

    def test_improved_moonfire(self):
        self.effects(763, 9835, 2)
        self.effects(763, 9835, 2, index=1)
        base = self.cast(763, 0, 9835)['crit_chance']
        for r in self.each(763):
            self.assertEqual(self.cast(763, r, 9835)['crit_chance'], base + 2 * r)

    def test_natural_weapons(self):
        for form in ('none', 'cat', 'bear', 'dire_bear', 'travel', 'aquatic'):
            base = self.svc.char(self.caster(791, 0, form=form)).weapon_damage_range()
            for r in self.each(791):
                value = self.svc.char(self.caster(791, r, form=form)).weapon_damage_range()
                for b, a in zip(base, value):
                    self.assertAlmostEqual(a, b * (1 + r * .02))
        # Moonkin must be legally learned; omit dependent Omen while isolating
        # Natural Weapons. Its armor/spell aura must not hide physical damage.
        ranks = {t['id']: t['max_rank'] for t in self.trees[0]['talents'] if t['id'] not in (791, 788)}
        base = self.svc.char({'level': 100, 'talents': ranks, 'form': 'moonkin'}).weapon_damage_range()
        for r in self.each(791):
            value = self.svc.char({'level': 100, 'talents': {**ranks, 791: r}, 'form': 'moonkin'}).weapon_damage_range()
            for b, a in zip(base, value):
                self.assertAlmostEqual(a, b * (1 + r * .02))

    def test_natural_shapeshifter(self):
        self.costs(781, (768, 5487, 9634, 783, 1066), 10, target={'kind': 'self'}, swimming=True)
        ranks = {t['id']: t['max_rank'] for t in self.trees[0]['talents'] if t['id'] != 781}
        req = {'op': 'resolve', 'spell': 24858, 'target': {'kind': 'self'}, 'caster': {'level': 100, 'talents': ranks}}
        base = self.svc.handle(req)['cost']
        for r in self.each(781):
            out = self.svc.handle({**req, 'caster': {**req['caster'], 'talents': {**ranks, 781: r}}})
            self.assertAlmostEqual(out['cost'], base * (1 - .1 * r), delta=1)

    def test_natural_weapons_reaches_native_white_swings(self):
        # The sheet already scaled damage; white swings used a separate range
        # and omitted aura 79. Fix the die at its upper end to compare equal
        # weapon rolls: resizing randint's range can change a seeded sample.
        for form in ('none', 'cat', 'bear', 'dire_bear', 'travel', 'aquatic', 'moonkin'):
            ranks = (self.ranks(791, 0) if form != 'moonkin' else
                     {t['id']: t['max_rank'] for t in self.trees[0]['talents'] if t['id'] not in (791, 788)})
            for native in (None, {'two_handed': 15, 'one_handed': 15}, {'two_handed': 100, 'one_handed': 100}):
                caster = {'level': 100, 'talents': ranks, 'form': form, 'items': [873]}
                if native is not None:
                    caster['skyrim'] = native
                def swing(rank):
                    req = {'op': 'melee', 'caster': {**caster, 'talents': {**ranks, 791: rank}} if rank else caster,
                           'target': TARGET, 'seed': 1}
                    with patch('service.melee.random.Random') as rng:
                        rng.return_value.randint.side_effect = lambda low, high: high
                        return self.svc.handle(req)
                base = swing(0)
                self.assertEqual(base['outcome'], 'hit')
                for rank in self.each(791):
                    with self.subTest(form=form, native=native, rank=rank):
                        out = swing(rank)
                        self.assertEqual(out['outcome'], base['outcome'])
                        self.assertAlmostEqual(out['damage'], base['damage'] * (1 + .02 * rank), delta=2)
                # The 10% gain can round to the same native health integer.
                self.assertGreaterEqual(swing(5)['damage'], base['damage'])

    def test_improved_thorns(self):
        self.effects(782, 9910, 25, target={'kind': 'self'})
        base = self.cast(782, 0, 9910, target={'kind': 'self'})['auras'][0]['amount']
        for r in self.each(782):
            out = self.cast(782, r, 9910, target={'kind': 'self'}, skyrim={'alteration': 100})
            self.assertEqual(out['auras'][0]['amount'], round(int(base * (1 + .25 * r)) * self.cat.balance[9910].health_factor))

    def test_omen_of_clarity(self):
        ranks = self.ranks(788, 1)
        self.assertEqual(self.cast(788, 1, 16864, target={'kind': 'self'})['auras'][0]['duration_ms'], 600000)
        req = {'op': 'melee', 'target': TARGET, 'caster': {'level': 100, 'talents': ranks, 'auras': [16864]}}
        proc = next(self.svc.handle({**req, 'seed': s}) for s in range(500) if self.svc.handle({**req, 'seed': s}).get('procs'))
        self.assertEqual(proc['procs'][0]['spell'], 16870)
        self.assertEqual(proc['procs'][0]['cooldown_ms'], 3000)
        out = self.cast(788, 1, 9912, auras=[16870])
        self.assertEqual((out['cost'], out['consumed_auras']), (0, [16870]))
        cat = self.cast(788, 1, 9850, form='cat', power={'energy': 100}, auras=[16870])
        self.assertEqual((cat['cost'], cat['consumed_auras']), (0, [16870]))

    def test_natures_reach(self):
        for spell in (9912, 9853, 9907, 9835, 9876, 16914):
            base = self.cast(764, 0, spell)['tooltip']['range_max']
            for rank in self.each(764):
                self.assertAlmostEqual(self.cast(764, rank, spell)['tooltip']['range_max'], base * (1 + .1 * rank))
                if spell != 16914:
                    self.cast(764, rank, spell, target={**TARGET, 'distance_yd': base * (1 + .1 * rank)})

    def test_vengeance(self):
        for spell in (9912, 9835, 9876):
            for r in self.each(792):
                seed = next(s for s in range(500) if self.cast(792, r, spell, seed=s)['events'][0].get('crit'))
                base = self.cast(792, 0, spell, seed=seed)['events'][0]['amount']
                value = self.cast(792, r, spell, seed=seed)['events'][0]['amount']
                self.assertAlmostEqual(value / base, (1.5 + .1 * r) / 1.5, delta=.015)

    def test_improved_starfire(self):
        self.times(784, 9876)
        for r in self.each(784):
            outputs = [self.cast(784, r, 9876, seed=s) for s in range(200)]
            observed = [s for s, out in enumerate(outputs) if any(p['spell'] == 16922 for p in out.get('procs', []))]
            expected = [s for s in range(200) if random.Random(f'{s}:talent_procs').random() * 100 < 3 * r]
            self.assertEqual(observed, expected)
            proc = next(p for out in outputs for p in out.get('procs', []) if p['spell'] == 16922)
            self.assertEqual(proc['auras'][0]['type'], 'stun')
            self.assertEqual(proc['auras'][0]['duration_ms'], 3000)
            self.assertFalse(any(p['spell'] == 16922 for p in self.cast(784, r, 9912).get('procs', [])))

    def test_natures_grace(self):
        out = next(self.cast(789, 1, 9912, seed=s) for s in range(500) if self.cast(789, 1, 9912, seed=s)['events'][0].get('crit'))
        self.assertTrue(any(p['spell'] == 16886 for p in out['procs']))
        base = self.cast(789, 1, 9912)['cast_ms']
        out = self.cast(789, 1, 9912, auras=[16886])
        self.assertEqual((out['cast_ms'], out['consumed_auras']), (base - 500, [16886]))

    def test_moonglow(self):
        self.costs(783, (9835, 9876, 9912, 9889, 9858, 9841), 3)

    def test_moonglow_native_healing_touch_requires_friendly_target(self):
        for skill in (15, 100):
            self.costs(783, (9889,), 3, target={'kind': 'self'}, skyrim={'restoration': skill})
            with self.assertRaisesRegex(Exception, 'BAD_TARGETS'):
                self.cast(783, 3, 9889, target=TARGET, skyrim={'restoration': skill})

    def test_moonfury(self):
        for spell in (9876, 9835, 9912):
            self.effects(790, spell, 2, additive_base=110 if spell == 9835 else 100)
        self.effects(790, 9835, 2, index=1, additive_base=110)

    def test_moonkin_form(self):
        out = self.cast(793, 1, 24858, target={'kind': 'self'}, skyrim={})
        self.assertEqual(out['auras'][0]['armor_percent'], 360)
        self.assertEqual(out['auras'][0]['display_id'], 15374)
        base = self.svc.char(self.caster(793, 1))
        moon = self.svc.char(self.caster(793, 1, form='moonkin'))
        for school in range(7):
            self.assertEqual(moon.spell_crit(school) - base.spell_crit(school), 3)
        self.assertEqual(self.cat.radius_yd(self.cat.spell(24907), 1), 30)
        # Native passive refresh must use an unshifted untalented control:
        # stripping the allocation while retaining Moonkin rejects the profile.
        profile = self.svc.handle({'op': 'stats', 'caster': self.caster(793, 1, form='moonkin')})
        control = self.svc.handle({'op': 'stats', 'caster': {'level': 100, 'talents': {}, 'form': 'none'}})
        self.assertEqual(profile['stats'], control['stats'])
        with self.assertRaisesRegex(Exception, 'NOT_SHAPESHIFT'):
            self.cast(793, 1, 9889, form='moonkin')

    def test_improved_mark_of_the_wild(self):
        self.effects(821, 9885, 7, target={'kind': 'self'})
        self.effects(821, 9885, 7, index=1, target={'kind': 'self'})
        self.effects(821, 9885, 7, index=2, target={'kind': 'self'})

    def test_furor(self):
        for rank in self.each(822):
            for spell, form, resource, expected in ((768, 'cat', 'energy', 40), (5487, 'bear', 'rage', 10), (9634, 'dire_bear', 'rage', 10)):
                gains = [self.cast(822, rank, spell, seed=s, target={'kind': 'self'})['auras'][0]['power_set'][resource] for s in range(100)]
                self.assertEqual(gains, [expected if random.Random(s).random() * 100 < rank * 20 else 0 for s in range(100)])
                self.assertIn(expected, gains)
                self.assertEqual(set(gains), {expected} if rank == 5 else {0, expected})

    def test_improved_healing_touch(self):
        self.times(824, 9889)

    def test_natures_focus(self):
        for r in self.each(823):
            for spell in (9889, 9858, 9863):
                self.assertEqual(self.cast(823, r, spell)['pushback_resist_pct'], 14 * r)
            self.assertEqual(self.cast(823, r, 9841)['pushback_resist_pct'], 0)

    def test_improved_enrage(self):
        for rank, amount in ((1, 5), (2, 10)):
            out = self.cast(826, rank, 5229, target={'kind': 'self'}, form='bear', power={'rage': 0})
            proc = next(p for p in out['procs'] if p['spell'] == (17080 if rank == 1 else 17081))
            self.assertEqual(proc['events'], [{'type': 'power', 'power': 'rage', 'amount': amount}])
            self.assertEqual(out['auras'][0]['per_tick'], 2)

    def test_reflection(self):
        base = self.svc.char(self.caster(829, 0)).regen()
        for r in self.each(829):
            regen = self.svc.char(self.caster(829, r)).regen()
            self.assertEqual(regen['per_sec'], base['per_sec'])
            self.assertAlmostEqual(regen['per_sec_casting'], base['per_sec'] * r * .05)

    def test_insect_swarm(self):
        out = self.cast(827, 1, 5570, skyrim={'destruction': 100})
        self.assertEqual(next(a for a in out['auras'] if a['type'] == 'hit_chance')['amount'], -2)
        dot = next(a for a in out['auras'] if a['type'] == 'damage')
        self.assertEqual((dot['duration_ms'], len(dot['ticks'])), (12000, 6))
        self.assertGreater(dot['per_tick'], 0)
        book = self.svc.handle({'op': 'known_spells', 'class': 'druid', 'caster': self.caster(827, 1)})
        self.assertIn(5570, book['known_spells'])

    def test_subtlety(self):
        for r in self.each(841):
            for spell in (9889, 9858, 9841, 9863):
                self.assertAlmostEqual(self.cast(841, r, spell)['threat_multiplier'], 1 - .04 * r)
            self.assertEqual(self.cast(841, r, 9912)['threat_multiplier'], 1)

    def test_tranquil_spirit(self):
        self.costs(843, (9889, 9863), 2)

    def test_improved_rejuvenation(self):
        self.effects(830, 9841, 5)
        for items in ([], [16897, 16898, 16899, 16900, 16901, 16902, 16903, 16904]):
            for rank in self.each(830):
                wow_tick = self.cast(830, rank, 9841, items=items)['auras'][0]['per_tick']
                for skill in (15, 100):
                    hot = self.cast(830, rank, 9841, items=items, skyrim={'restoration': skill})['auras'][0]
                    # The rank floor applies to base power; all added healing
                    # uses the chain's shared top-rank conversion.
                    spell = self.cat.spell(9841)
                    base = int(F.effect_value(spell, 1, 60, None, maximum=True))
                    expected = round(base * self.cat.balance[9841].health_factor +
                                     (wow_tick-base) * self.cat.balance[25299].health_factor)
                    self.assertEqual(hot['per_tick'], expected)
                    self.assertEqual(hot['amplitude_ms'], 3000)
                    self.assertEqual(hot['ticks'], [{'t_ms': ms, 'amount': expected}
                                                   for ms in range(3000, 15001 if items else 12001, 3000)])

    def test_natures_swiftness(self):
        self.cast(831, 1, 17116, target={'kind': 'self'})
        out = self.cast(831, 1, 9889, auras=[17116])
        self.assertEqual((out['cast_ms'], out['consumed_auras']), (0, [17116]))
        self.assertGreater(self.cast(831, 1, 9876, auras=[17116])['cast_ms'], 0)
        self.assertNotIn('consumed_auras', self.cast(831, 1, 9841, auras=[17116]))

    def test_gift_of_nature(self):
        for spell in (9889, 9858, 9841, 9863):
            self.effects(828, spell, 2, additive_base=115 if spell == 9841 else 100)
        self.effects(828, 9858, 2, index=1)
        base = self.svc.handle({'op': 'tick_area', 'spell': 9863, 'caster': self.caster(828, 0), 'targets': [{'id': 1, 'friendly': True}]})['results'][0]['amount']
        value = self.svc.handle({'op': 'tick_area', 'spell': 9863, 'caster': self.caster(828, 5), 'targets': [{'id': 1, 'friendly': True}]})['results'][0]['amount']
        self.assertAlmostEqual(value, base * 1.1, delta=1)

    def test_improved_tranquility(self):
        # Lower-tier filler includes Subtlety 5: modifiers add, then clamp at zero.
        for rank, expected in ((1, .3), (2, 0)):
            self.assertAlmostEqual(self.cast(842, rank, 9863)['threat_multiplier'], expected)
            out = self.svc.handle({'op': 'tick_area', 'spell': 9863, 'caster': self.caster(842, rank), 'targets': [{'id': 1, 'friendly': True}]})
            self.assertAlmostEqual(out['threat_multiplier'], expected)
            self.assertAlmostEqual(self.cast(842, rank, 9889)['threat_multiplier'], .8)

    def test_improved_regrowth(self):
        base = self.cast(825, 0, 9858)['crit_chance']
        for r in self.each(825):
            self.assertAlmostEqual(self.cast(825, r, 9858)['crit_chance'], base + 10 * r)

    def test_swiftmend(self):
        target = {'kind': 'self', 'hots': [{'spell': 9858, 'per_tick': 50, 'remaining_ms': 10000}, {'spell': 9841, 'per_tick': 100, 'remaining_ms': 1000}]}
        out = self.cast(844, 1, 18562, target=target, skyrim={})
        self.assertEqual((out['cast_ms'], out['consumed_target_hot']), (0, 9841))
        self.assertGreater(out['events'][0]['amount'], 0)
        # The client's explicit 12/18-second windows differ from CMaNGOS's
        # full-duration Regrowth implementation (21 seconds = seven ticks).
        for spell, ticks in ((9841, 4), (9858, 6)):
            amounts = []
            for tick in (0, 100):
                hot = {'kind': 'self', 'hots': [{'spell': spell, 'per_tick': tick, 'remaining_ms': 1000}]}
                heal = self.cast(844, 1, 18562, target=hot)
                self.assertFalse(heal['events'][0]['crit'])
                amounts.append(heal['events'][0]['amount'])
            self.assertEqual(amounts[1] - amounts[0], ticks * 100)
        with self.assertRaisesRegex(Exception, 'TARGET_AURASTATE'):
            self.cast(844, 1, 18562, target={'kind': 'self'})

    def test_swiftmend_native_budget_comes_from_consumed_hot(self):
        # Real HoT magnitudes, including talents/gear, must reach the bridge
        # before conversion. Swiftmend's one-point DBC placeholder previously
        # multiplied these by 50 (61,250 health in the October 9 game run).
        from service.tests.test_service import FULL
        for spell, ticks in ((774, 4), (9841, 4), (25299, 4), (8936, 6), (9858, 6)):
            factor = self.cat.balance[spell].health_factor
            for items in ([], FULL):
                for level in (15, 100):
                    with self.subTest(spell=spell, items=items, skill=level):
                        caster = self.caster(844, 1, items=items)
                        hot_request = {'op': 'resolve', 'spell': spell, 'seed': 1,
                                       'caster': caster, 'target': {'kind': 'self'}}
                        wow_hot = next(a for a in self.svc.handle(hot_request)['auras'] if a['type'] == 'heal')
                        target = {'kind': 'self', 'hots': [{'spell': spell,
                                  'per_tick': wow_hot['per_tick'], 'remaining_ms': 1000}]}
                        request = {'op': 'resolve', 'spell': 18562, 'seed': 1,
                                   'caster': caster, 'target': target}
                        wow = self.svc.handle(request)
                        native = self.svc.handle({**request, 'caster': {
                            **caster, 'skyrim': {'restoration': level}}})
                        self.assertEqual(native['consumed_target_hot'], spell)
                        amount = native['events'][0]['amount']
                        base_tick = int(F.effect_value(self.cat.spell(spell), 1 if spell in (774, 9841, 25299) else 2,
                                                     60, None, maximum=True))
                        base = 1 + ticks * base_tick
                        bonus_factor = self.cat.balance[spell].bonus_health_factor or factor
                        expected = round(base * factor + (wow['events'][0]['amount'] - base) * bonus_factor)
                        self.assertLessEqual(abs(amount - max(1, expected)), 1)
                        # Within rounding and Swiftmend's flat/bonus healing,
                        # it releases four/six native ticks, not 50x WoW ticks.
                        native_hot = self.svc.handle({**hot_request, 'caster': {
                            **caster, 'skyrim': {'restoration': level}}})
                        tick = next(a for a in native_hot['auras'] if a['type'] == 'heal')['per_tick']
                        self.assertLessEqual(abs(amount - ticks * tick), 8)
                        self.assertLess(amount, 200)
