"""All sixteen 1.12 Feral talents, every rank, through the public protocol.

Client Talent/Spell DBC and CMaNGOS spell_affect/proc rules supply the
independent expectations below. Native adaptations are checked in Skyrim by
wow_talents_feral; these tests do not claim to execute native actor effects.
"""
import unittest

from service import formulas as F
from data import dbc
from data.build_catalog import DBC_DIR
from service.catalog import Catalog
from service.server import Service
from service.talents import Talents

TARGET = {'kind': 'unit', 'level': 60, 'distance_yd': 2, 'reaction': 'hostile',
          'alive': True, 'behind': True, 'in_front': True}


class DruidFeralTalents(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()
        cls.tree = Talents().data['classes']['druid']['trees'][1]
        cls.talents = {t['id']: t for t in cls.tree['talents']}

    def setUp(self):
        self.svc = Service(self.cat, None)

    def ranks(self, tid, rank):
        talent = self.talents[tid]
        result = {t['id']: t['max_rank'] for t in self.tree['talents'] if t['tier'] < talent['tier']}
        for p in talent['prerequisites']:
            result[p['talent']] = p['rank']
        if rank:
            result[tid] = rank
        return result

    def caster(self, tid, rank, **extra):
        return {'level': 100, 'talents': self.ranks(tid, rank), 'form': 'cat',
                'power': {'energy': 100, 'rage': 100}, **extra}

    def cast(self, tid, rank, spell, *, target=None, seed=1, **extra):
        return self.svc.handle({'op': 'resolve', 'spell': spell, 'seed': seed,
                               'caster': self.caster(tid, rank, **extra),
                               'target': TARGET if target is None else target})

    def stats(self, tid, rank, **extra):
        return self.svc.handle({'op': 'stats', 'caster': self.caster(tid, rank, **extra)})

    def swing(self, tid, rank, *, seed=1, **extra):
        return self.svc.handle({'op': 'melee', 'seed': seed, 'target': TARGET,
                               'caster': self.caster(tid, rank, **extra)})

    def each(self, tid):
        return range(1, self.talents[tid]['max_rank'] + 1)

    def test_ferocity(self):
        for spell, form in ((9881, 'bear'), (9908, 'dire_bear'), (9850, 'cat'), (9904, 'cat')):
            base = self.cast(796, 0, spell, form=form)['cost']
            for rank in self.each(796):
                self.assertEqual(self.cast(796, rank, spell, form=form)['cost'], base - rank)
                if spell == 9881:
                    paid = self.swing(796, rank, form=form, next_swing=spell)
                    self.assertEqual(paid['cost'], base - rank)
                    self.assertTrue(paid['next_swing_consumed'])
        self.assertEqual(self.cast(796, 5, 9830)['cost'], self.cast(796, 0, 9830)['cost'])

    def test_feral_aggression(self):
        base = self.cast(795, 0, 9898, form='bear', target={'kind': 'self'})
        bite = self.cast(795, 0, 31018, combo_points=5)
        for rank in self.each(795):
            roar = self.cast(795, rank, 9898, form='bear', target={'kind': 'self'})
            self.assertEqual(roar['auras'][0]['amount'], int(base['auras'][0]['amount'] * (1 + .08 * rank)))
            self.assertEqual(roar['tooltip']['effects'][0][0], abs(roar['auras'][0]['amount']))
            out = self.cast(795, rank, 31018, combo_points=5)
            self.assertAlmostEqual(out['events'][0]['amount'], bite['events'][0]['amount'] * (1 + .03 * rank), delta=2)

    def test_feral_instinct(self):
        for rank in self.each(799):
            for form in ('bear', 'dire_bear'):
                profile = self.stats(799, rank, form=form)['form']
                self.assertEqual(profile['threat_percent'], 30 + 3 * rank)
                self.assertEqual(profile['feral_threat_bonus_percent'], 3 * rank)
                self.assertEqual(self.swing(799, rank, form=form)['feral_threat_bonus_percent'], 3 * rank)
            self.assertEqual(self.stats(799, rank)['form']['stealth_level_bonus'], 3 * rank)
            self.assertEqual(self.stats(799, rank)['form']['feral_threat_bonus_percent'], 0)
            self.assertEqual(self.stats(799, rank, form='none')['form']['stealth_level_bonus'], 0)

    def test_brutal_impact(self):
        for spell, form, duration in ((8983, 'bear', 4000), (9827, 'cat', 2000)):
            for rank in self.each(797):
                out = self.cast(797, rank, spell, form=form, prowling=True)
                stun = next(a for a in out['auras'] if a['type'] == 'stun')
                self.assertEqual(stun['duration_ms'], duration + 500 * rank)
                self.assertEqual(out['tooltip']['duration_ms'], stun['duration_ms'])
                if spell == 9827:
                    self.assertEqual(next(a for a in out['auras'] if a['type'] == 'damage')['duration_ms'], 18000)

    def test_thick_hide(self):
        for form, original in (('none', 0), ('cat', 0), ('bear', 180), ('dire_bear', 360)):
            for rank in self.each(794):
                armor = self.stats(794, rank, form=form)['form']['armor_percent']
                self.assertAlmostEqual(armor, ((1 + original / 100) * (1 + .02 * rank) - 1) * 100)

    def test_feline_swiftness(self):
        for rank in self.each(807):
            for indoors in (True, False):
                out = self.stats(807, rank, indoors=indoors)
                base = self.stats(807, 0, indoors=indoors)
                self.assertEqual(out['form']['movement_speed_percent'], 0 if indoors else 15 * rank)
                self.assertEqual(out['dodge_bonus'] - base['dodge_bonus'], 2 * rank)
                self.assertEqual(out['form']['dodge_bonus_percent'], 2 * rank)
            for form, speed in (('none', 0), ('bear', 0), ('travel', 40), ('aquatic', 0)):
                out = self.stats(807, rank, form=form)
                self.assertEqual(out['form']['movement_speed_percent'], speed)
                self.assertEqual(out['form']['dodge_bonus_percent'], 0)
        self.assertEqual(self.stats(807, 0)['form']['dodge_bonus_percent'], 0)  # Quickness is already native

    def test_feral_charge(self):
        for form in ('bear', 'dire_bear'):
            out = self.cast(804, 1, 16979, form=form, target={**TARGET, 'distance_yd': 10})
            self.assertEqual((out['cost'], out['power'], out['cooldown_ms']), (5, 'rage', 15000))
            self.assertEqual(out['events'][0]['type'], 'charge')
            self.assertEqual(next(e for e in out['events'] if e['type'] == 'interrupt')['school_lockout_ms'], 4000)
            root = next(a for a in out['auras'] if a['type'] == 'root')
            self.assertEqual(root['duration_ms'], 4000)
            self.assertFalse(root['single_target'])   # Feral Charge Effect: no mechanic, no family flags
        for form in ('cat', 'none'):
            with self.assertRaisesRegex(Exception, 'SHAPESHIFT'):
                self.cast(804, 1, 16979, form=form, target={**TARGET, 'distance_yd': 10})
        with self.assertRaisesRegex(Exception, 'OUT_OF_RANGE'):
            self.cast(804, 1, 16979, form='bear', target={**TARGET, 'distance_yd': 30})
        with self.assertRaisesRegex(Exception, 'NO_POWER'):
            self.cast(804, 1, 16979, form='bear', power={'rage': 4}, target={**TARGET, 'distance_yd': 10})

    def test_sharpened_claws(self):
        for rank in self.each(798):
            for form in ('cat', 'bear', 'dire_bear', 'none', 'travel'):
                actual = self.stats(798, rank, form=form)['melee_crit'] - self.stats(798, 0, form=form)['melee_crit']
                self.assertAlmostEqual(actual, 2 * rank if form in ('cat', 'bear', 'dire_bear') else 0)
        # Identical seed changes a real noncritical attack into a critical one.
        samples = [(self.swing(798, 0, seed=s), self.swing(798, 3, seed=s)) for s in range(100)]
        self.assertTrue(any(b['outcome'] == 'hit' and a['outcome'] == 'crit' and a['damage'] > b['damage'] for b, a in samples))

    def test_improved_shred(self):
        for rank in self.each(802):
            self.assertEqual(self.cast(802, rank, 9830)['cost'], 60 - 6 * rank)
            self.assertEqual(self.cast(802, rank, 9850)['cost'], self.cast(802, 0, 9850)['cost'])
        with self.assertRaisesRegex(Exception, 'NOT_BEHIND'):
            self.cast(802, 2, 9830, target={**TARGET, 'behind': False})

    def test_predatory_strikes(self):
        for rank in self.each(803):
            for form in ('cat', 'bear', 'dire_bear', 'none', 'travel'):
                base = self.stats(803, 0, form=form)['form']
                out = self.stats(803, rank, form=form)['form']
                self.assertEqual(out['attack_power'] - base['attack_power'], 50 * rank if form in ('cat', 'bear', 'dire_bear') else 0)
            self.assertGreater(self.swing(803, rank)['damage'], self.swing(803, 0)['damage'])

    def test_blood_frenzy(self):
        for rank in self.each(800):
            triggered, declined = 0, 0
            for seed in range(100):
                out = self.cast(800, rank, 9850, seed=seed)
                if not out['events'][0].get('crit'):
                    self.assertEqual(out.get('combo_points'), 1)
                    self.assertNotIn('procs', out)
                    continue
                if out.get('procs'):
                    triggered += 1
                    self.assertEqual(out['combo_points'], 2)
                    self.assertEqual(out['procs'][0]['spell'], 16953)
                else:
                    declined += 1
            self.assertGreater(triggered, 0)
            self.assertEqual(declined == 0, rank == 2)
        for spell, extra in ((9830, {}), (9904, {}), (9867, {'prowling': True}), (9827, {'prowling': True}), (31018, {'combo_points': 5})):
            samples = [self.cast(800, 2, spell, seed=seed, **extra) for seed in range(30)]
            if spell in (9830, 9904, 9867):
                self.assertTrue(any(o.get('combo_points') == 2 for o in samples))
            else:
                self.assertTrue(all(not o.get('procs') for o in samples))
        self.assertTrue(all(not self.swing(800, 2, seed=s).get('procs') for s in range(30)))
        for cp in (4, 5):
            for seed in range(30):
                self.assertLessEqual(self.cast(800, 2, 9850, seed=seed, combo_points=cp).get('combo_points', 0), 5 - cp)

    def test_primal_fury(self):
        for rank in self.each(801):
            for form in ('bear', 'dire_bear'):
                critical = [self.swing(801, rank, form=form, seed=s) for s in range(100)]
                critical = [o for o in critical if o['outcome'] == 'crit']
                procs = [o for o in critical if o.get('procs')]
                self.assertTrue(procs)
                self.assertEqual(len(procs) == len(critical), rank == 2)
                for out in procs:
                    self.assertEqual(out['procs'][0]['events'], [{'type': 'power', 'power': 'rage', 'amount': 5.0}])
                for spell in (9908, 9881):
                    replies = [self.cast(801, rank, spell, form=form, seed=s) if spell != 9881 else
                               self.swing(801, rank, form=form, next_swing=spell, seed=s) for s in range(30)]
                    self.assertTrue(any(o.get('procs') for o in replies))
        self.assertTrue(all(not self.swing(801, 2, seed=s).get('procs') for s in range(30)))

    def test_savage_fury(self):
        for spell, form in ((9850, 'cat'), (9904, 'cat'), (9908, 'bear'), (9881, 'dire_bear')):
            base = self.cast(805, 0, spell, form=form)
            for rank in self.each(805):
                out = self.cast(805, rank, spell, form=form)
                if spell == 9881:
                    # The queued flat bonus precedes Savage Fury; the actual
                    # swing below applies its damage modifier to the whole hit.
                    self.assertEqual(out['tooltip']['effects'][0], [out['events'][0]['bonus_damage']] * 2)
                    native = self.cast(805, rank, spell, form=form, skyrim={})
                    self.assertEqual(native['events'][0]['bonus_damage'],
                                     round(native['tooltip']['effects'][0][0] * native['tooltip']['health_factor']))
                    out = self.swing(805, rank, form=form, next_swing=spell)
                    original = self.swing(805, 0, form=form, next_swing=spell)
                    amount, original_amount = out['damage'], original['damage']
                else:
                    self.assertGreater(out['tooltip']['effects'][0][0], base['tooltip']['effects'][0][0])
                    amount, original_amount = out['events'][0]['amount'], base['events'][0]['amount']
                self.assertAlmostEqual(amount, original_amount * (1 + .1 * rank), delta=2)
                if spell == 9904:
                    dot = next(a for a in out['auras'] if a['type'] == 'damage')
                    plain = next(a for a in base['auras'] if a['type'] == 'damage')
                    self.assertAlmostEqual(dot['per_tick'], plain['per_tick'] * (1 + .1 * rank), delta=1)
                    self.assertEqual(out['tooltip']['effects'][1][0], dot['per_tick'])
        self.assertEqual(self.cast(805, 2, 9830)['events'], self.cast(805, 0, 9830)['events'])

    def test_faerie_fire_feral(self):
        for spell, armor in ((16857, 175), (17390, 285), (17391, 395), (17392, 505)):
            for form in ('cat', 'bear', 'dire_bear'):
                out = self.cast(1162, 1, spell, form=form)
                self.assertEqual(out['cost'], 0)
                self.assertEqual(out['cooldown_ms'], 6000)
                self.assertEqual(out['auras'][0]['amount'], -armor)
                self.assertEqual(out['auras'][0]['duration_ms'], 40000)
                self.assertEqual({a.get('dispel') for a in out['auras'][1:]}, {'Stealth', 'Invisibility'})
                self.assertEqual(out['tooltip']['effects'][0][0], armor)
                req = {'op': 'melee', 'seed': 1, 'caster': self.caster(1162, 1, form=form), 'target': TARGET}
                plain = self.svc.handle(req)
                marked = self.svc.handle({**req, 'target': {**TARGET, 'armor_reduction': armor}})
                self.assertGreater(marked['damage'], plain['damage'])
                attack = 9850 if form == 'cat' else 9908
                plain = self.cast(1162, 1, attack, form=form)
                marked = self.cast(1162, 1, attack, form=form, target={**TARGET, 'armor_reduction': armor})
                self.assertGreater(marked['events'][0]['amount'], plain['events'][0]['amount'])
        with self.assertRaisesRegex(Exception, 'NOT_KNOWN'):
            self.cast(1162, 0, 16857)

    def test_heart_of_the_wild(self):
        for rank in self.each(808):
            for form in ('none', 'cat', 'bear', 'dire_bear', 'travel'):
                base = self.stats(808, 0, form=form)['stats']
                stats = self.stats(808, rank, form=form)['stats']
                self.assertEqual(stats['int'], F.stat_total(base['int'], 0, (), [4 * rank]))
                self.assertEqual(stats['str'], F.stat_total(base['str'], 0, (), [4 * rank]) if form == 'cat' else base['str'])
                self.assertEqual(stats['sta'], F.stat_total(base['sta'], 0, (), [4 * rank]) if form in ('bear', 'dire_bear') else base['sta'])

    def test_heart_native_stat_gains_with_october_10_equipment(self):
        # The native probe must measure the mapped Skyrim values rather than
        # assume Strength becomes native Stamina. Bear health is separate.
        items = [873, 16897, 16898, 16899, 16900, 16901, 16902, 16903, 16904, 22398]
        full = {tid: t['max_rank'] for tid, t in self.talents.items()}
        def stats(form, talents):
            return self.svc.handle({'op': 'stats', 'caster': {
                'level': 100, 'items': items, 'form': form, 'talents': talents}})['stats']
        unshifted = stats('none', {})
        cat, cat_base = stats('cat', full), stats('cat', {})
        bear, bear_base = stats('bear', full), stats('bear', {})
        self.assertEqual((cat['int'], cat_base['int']), (345, 288))
        self.assertEqual((cat['str'], cat_base['str']), (74, 62))
        self.assertEqual((unshifted['sta'], bear_base['sta'], bear['sta']), (191, 191, 229))
        self.assertEqual((bear['sta'] - bear_base['sta']) * 1.5, 57)
        from service.skyrim_effects import STAT
        self.assertEqual([(mgef, (cat['str'] - cat_base['str']) * scale)
                          for mgef, scale in STAT[4]], [(0x7A0F4, 12), (0x7A0FF, 6), (0x7A106, 6)])
        # Verify the adapter's form IDs against Skyrim.esm's primary actor
        # values (disk MGEF DATA +68), independently of the native code.
        from data import esm
        from data.build_esp import MASTER
        import struct
        forms = {r.form: r for r in esm.read(MASTER, {'MGEF'})['MGEF']}
        self.assertEqual([struct.unpack_from('<i', forms[mgef].get('DATA'), 68)[0]
                          for mgef, _ in STAT[4]], [32, 96, 97])

    def test_leader_of_the_pack(self):
        for form in ('cat', 'bear', 'dire_bear', 'none', 'travel'):
            out, base = self.stats(809, 1, form=form), self.stats(809, 0, form=form)
            active = form in ('cat', 'bear', 'dire_bear')
            self.assertAlmostEqual(out['melee_crit'] - base['melee_crit'], 3 if active else 0)
            self.assertEqual(out['form']['leader_of_the_pack'], {'crit_percent': 3, 'radius_yd': 45} if active else None)

    def test_client_authority_and_book_entries(self):
        client = dbc.by_id(dbc.read(DBC_DIR / 'Talent.dbc'))
        spells = dbc.by_id(dbc.read(DBC_DIR / 'Spell.dbc'))
        full = {tid: t['max_rank'] for tid, t in self.talents.items()}
        for tid, t in self.talents.items():
            self.assertEqual(t['rank_spells'], [sid for sid in client[tid]['SpellRank'] if sid])
            # Rank spell must enter the ordinary public book, passive or active.
            for rank in self.each(tid):
                reply = self.svc.handle({'op': 'known_spells', 'class': 'druid', 'caster': self.caster(tid, rank)})
                sid = t['rank_spells'][rank - 1]
                self.assertIn(sid, reply['known_spells'])
                entry = next(s for s in reply['spells'] if s['spell'] == sid)
                self.assertEqual(entry['passive'], tid not in (804, 1162))
        for sid, expected in ((24867, 2), (24864, 4)):
            self.assertEqual(spells[sid]['EffectAura'][0], 49)
            self.assertEqual(spells[sid]['EffectBasePoints'][0] + spells[sid]['EffectBaseDice'][0], expected)
        snap = self.svc.handle({'op': 'talents', 'class': 'druid', 'caster': {'level': 100, 'talents': full}})
        self.assertEqual(snap['points_spent'], 46)


if __name__ == '__main__':
    unittest.main()
