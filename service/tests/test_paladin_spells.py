"""The first Paladin spells: Holy Light / Flash of Light's code-linked heals, Lay on Hands, the seals on her swing and
Judgement, with their Skyrim rules; the Druid's spell-mod gate unchanged.

Expected values are hand computed from the spell rows and the cited CMaNGOS code (float steps as CMaNGOS rounds them);
the catalog is built in memory (data.build_catalog.build) because build/ may predate the Paladin manifest."""
import json
import random
import unittest

from data.build_catalog import build
from service import character, formulas as F, melee, paladin, skyrim, spells
from service.catalog import Catalog
from service.server import Service
from service.skyrim_balance import CRUSADER_NATIVE, HEALING_TOUCH, WRATH, FERAL_MELEE, MIGHT_NATIVE, PALADIN

PALADIN_60 = {'class': 2, 'race': 1, 'level': 60}
HOSTILE = {'kind': 'unit', 'level': 60, 'reaction': 'hostile', 'distance_yd': 3, 'in_front': True, 'facing': True}
PERDITIONS_BLADE, ASHKANDI = 18816, 19364        # one-hander 1.8 s 73-137; two-hander 3.5 s 229-344
SOR_1, SOR_8, CRUSADER_6, COMMAND_5, LIGHT_4, WISDOM_3, JUSTICE = 20154, 20293, 20308, 20920, 20349, 20357, 20164
HOLY_LIGHT_9, FLASH_6, LOH_1, LOH_3 = 25292, 19943, 633, 10310
JOTC_6, JOL_4, JOW_3 = 20303, 20346, 20355


def aura(kind, amount, misc=0):
    return {'aura': kind, 'amount': amount, 'misc': misc, 'spell': 0, 'eff': 1}


class PaladinSpells(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog(raw=build())

    def setUp(self):
        self.svc = Service(self.cat, None)

    def answer(self, req):
        out = self.svc.respond(json.dumps(req).encode())
        return out

    def ok(self, req):
        out = self.answer(req)
        self.assertTrue(out['ok'], out)
        return out

    def ch(self, items=(), auras=(), extra=()):
        ch = character.build(self.cat, list(items), cls=2, race=1, auras=list(auras))
        ch.auras.extend(extra)
        return ch

    # --- spell modifiers by class family ----------------------------------------------------------------------

    def test_spell_mods_follow_the_casters_class_family(self):
        # Lawbringer Armor's 23564: SPELLMOD_CHANCE_OF_SUCCESS +10 for family 10 mask 0x80000 (Judgement of Light).
        jol = self.cat.spell(JOL_4)
        paladin_ch = self.ch()
        character._apply_spell(paladin_ch, self.cat, 23564)
        self.assertEqual(paladin_ch.mods.family, 10)
        self.assertEqual(F.apply_spell_mod(jol, 18, 50, paladin_ch.mods), 60)
        # The Druid keeps the legacy gate: a family-10 mod on her list leaves a family-10 spell alone.
        druid = character.build(self.cat, [])
        character._apply_spell(druid, self.cat, 23564)
        self.assertEqual(druid.mods.family, 7)
        self.assertEqual(F.apply_spell_mod(jol, 18, 50, druid.mods), 50)
        self.assertEqual(F.apply_spell_mod(jol, 18, 50, list(paladin_ch.mods)), 50)   # a plain list is the Druid's
        # Perks keep the family on the augmented character.
        perked = self.svc.char({**PALADIN_60, 'perks': {}, 'skyrim': {}})
        self.assertEqual(getattr(perked.mods, 'family', None), 10)

    # --- heals ----------------------------------------------------------------------------------------------

    def heal(self, sid, ch, seed, target=None):
        req = {'op': 'resolve', 'spell': sid, 'seed': seed, 'caster': PALADIN_60, 'target': target or {'kind': 'self'}}
        return spells.resolve(self.cat, ch, req)

    def test_holy_light_heals_through_19968_with_its_level_one_penalty(self):
        ch = self.ch(extra=[aura(135, 100, 2)])             # +100 healing (SPELL_AURA_MOD_HEALING_DONE, holy)
        for seed in range(8):
            out = self.heal(HOLY_LIGHT_9, ch, seed)
            rng = random.Random(f'{seed}:paladin')
            base = 1589 + rng.randint(1, 181)                # EffectBasePoints 1589, DieSides 181 (level 60, no growth)
            crit = rng.random() * 100 < ch.spell_crit(1)
            # 19968: coefficient .714, SpellLevel 1 -> CalculateLevelPenalty 1 - 19 x .0375 = .2875; int(20.5275) = 20.
            amount = base + 20
            self.assertEqual(out['events'], [{'type': 'heal', 'amount': int(amount * 1.5) if crit else amount,
                                              'crit': crit, 'school': 1, 'spell': 19968}])
            self.assertEqual((out['cost'], out['cast_ms'], out['gcd_ms'], out['linked_spell']), (660, 2500, 1500, 19968))
            self.assertAlmostEqual(out['crit_chance'], ch.spell_crit(1))

    def test_flash_of_light_heals_through_19993(self):
        ch = self.ch(extra=[aura(135, 100, 2)])
        out = self.heal(FLASH_6, ch, 3)
        rng = random.Random('3:paladin')
        # Rank 6: 342 + 2.6 x (60 - 58) = 347.2, + 1..41; 19993: coefficient .429, level 20 (no penalty) -> +42.
        base = int(342 + 5.2 + rng.randint(1, 41))
        crit = rng.random() * 100 < ch.spell_crit(1)
        self.assertEqual(out['events'][0]['amount'], int((base + 42) * 1.5) if crit else base + 42)
        self.assertEqual((out['cost'], out['cast_ms'], out['events'][0]['spell']), (140, 1500, 19993))

    def test_heals_crit_at_the_heal_multiplier(self):
        ch = self.ch(extra=[aura(57, 200)])                  # always crits
        out = self.heal(HOLY_LIGHT_9, ch, 5)
        rng = random.Random('5:paladin')
        base = 1589 + rng.randint(1, 181)
        self.assertEqual(out['events'][0], {'type': 'heal', 'amount': int(base * 1.5), 'crit': True, 'school': 1,
                                            'spell': 19968})

    def test_lay_on_hands(self):
        ch = self.ch()
        for sid, mana in ((LOH_1, None), (2800, 250), (LOH_3, 550)):
            out = self.heal(sid, ch, 1, dict(HOSTILE, reaction='friendly', distance_yd=5))
            self.assertEqual(out['events'][0], {'type': 'heal', 'amount': 2201, 'crit': False, 'school': 1,
                                                'caster_max_health': 1.0})
            self.assertEqual(out['events'][1:], [{'type': 'power', 'power': 'mana', 'amount': mana}] if mana else [])
            self.assertEqual((out['cost'], out['use_all_mana'], out['cooldown_ms'], out['auras']), (0, True, 3600000, []))
        # Only ranks 2-3 can crit (their mana energize: IsSpellEffectAbleToCrit); the heal crit is x1.5.
        crit = self.ch(extra=[aura(57, 200)])
        self.assertEqual(self.heal(LOH_1, crit, 1)['crit_chance'], 0)
        self.assertFalse(self.heal(LOH_1, crit, 1)['events'][0]['crit'])
        out = self.heal(LOH_3, crit, 1)
        self.assertEqual(out['events'][0], {'type': 'heal', 'amount': 3301, 'crit': True, 'school': 1,
                                            'caster_max_health': 1.5})
        # No Forbearance in 1.12 (Spell.cpp:3215-3245 adds 25771 only for MECHANIC_INVULNERABILITY spells).
        self.assertEqual(self.cat.spell(LOH_3)['Mechanic'], 0)

    def test_heal_tooltips_show_the_linked_heal(self):
        from service.talent_effects import tooltip
        ch = self.ch(extra=[aura(135, 100, 2)])
        self.assertEqual(tooltip(self.cat, ch, self.cat.spell(HOLY_LIGHT_9))['effects'][0], [1610, 1790])
        self.assertEqual(tooltip(self.cat, ch, self.cat.spell(FLASH_6))['effects'][0], [390, 430])
        self.assertEqual(tooltip(self.cat, self.ch(), self.cat.spell(LOH_3))['effects'][:2], [[2201, 2201], [550, 550]])

    # --- seals ----------------------------------------------------------------------------------------------

    def cast(self, sid, caster=None, target=None, seed=1):
        return self.ok({'op': 'resolve', 'spell': sid, 'seed': seed, 'caster': caster or PALADIN_60,
                        'target': target or {'kind': 'self'}})

    def test_seal_casts(self):
        out = self.cast(SOR_8)
        self.assertEqual((out['stack'], out['stack_rule'], out['cost'], out['gcd_ms']), ('Seal', 'unique', 200, 1500))
        self.assertNotIn('stack_amounts', out)
        self.assertEqual(out['seal'], {'chain': 20154, 'kind': 'righteousness', 'judgement': 20286})
        self.assertEqual([(a['type'], a.get('amount'), a.get('payload'), a.get('judgement'), a['duration_ms'])
                          for a in out['auras']],
                         [('seal_proc', 1880, 25713, None, 30000), ('seal_judgement', None, None, 20286, 30000)])
        self.assertEqual(self.cast(SOR_1)['seal']['judgement'], None)     # the starting seal has no effect 2
        out = self.cast(CRUSADER_6)
        self.assertEqual([(a['type'], a.get('amount')) for a in out['auras']],
                         [('attack_power', 325), ('melee_haste', 40), ('seal_judgement', None)])
        out = self.cast(COMMAND_5)
        proc = out['auras'][0]
        self.assertEqual((proc['type'], proc['payload'], proc['ppm'], proc['proc_cooldown_ms'], proc['proc_flags']),
                         ('seal_proc', 20424, 7, 3000, 4))
        for sid, payload, ppm in ((LIGHT_4, 20340, 20), (WISDOM_3, 20351, 20), (JUSTICE, 20170, 5)):
            proc = self.cast(sid)['auras'][0]
            self.assertEqual((proc['payload'], proc['ppm'], proc['proc_flags']), (payload, ppm, 20))
        self.assertEqual(self.cast(JUSTICE)['cost'], 13 * 1512 // 100)

    def test_seal_of_righteousness_damage(self):
        t = dict(HOSTILE)
        # Bare hands (2 s, 1-2): 0.85 x ceil(1880 x 1.2 x 1.03 x 2 / 100 = 46.47) - 1 = 38.95, + .03 x 3 / 2 -> 39.
        self.assertEqual(paladin.righteousness_damage(self.cat, self.ch(), SOR_8, t), 39)
        # Perdition's Blade (one-hander, 1.8 s): 0.85 x ceil(41.83) - 1 = 34.7, + .03 x 210 / 2 = 37.85 -> 38.
        self.assertEqual(paladin.righteousness_damage(self.cat, self.ch([PERDITIONS_BLADE]), SOR_8, t), 38)
        # Ashkandi (two-hander, 3.5 s): 1.2 x 1880 x 1.2 x 1.03 x 3.5 / 100 + 1 = 98.59, + .03 x 573 / 2 -> 108.
        self.assertEqual(paladin.righteousness_damage(self.cat, self.ch([ASHKANDI]), SOR_8, t), 108)
        # Rank 1 (216 at 60, level 1 seal): 5; +1000 holy spell damage adds 1000 x .092 x 2 x .2875 = 52.9 -> 57.
        self.assertEqual(paladin.righteousness_damage(self.cat, self.ch(), SOR_1, t), 5)
        powered = self.ch(extra=[aura(13, 1000, 2)])
        self.assertEqual(paladin.righteousness_damage(self.cat, powered, SOR_1, t), 57)
        # Judgement of the Crusader's 140 holy damage taken joins the same bonus (SpellBaseDamageBonusTaken).
        self.assertEqual(paladin.righteousness_damage(self.cat, self.ch(), SOR_1, {**t, 'judgements': [JOTC_6]}),
                         int(5 + 140 * F.f32(.092 * 2) * F.level_penalty(self.cat.spell(SOR_1))))

    def swing(self, caster, target=None, seed=0):
        return self.ok({'op': 'melee', 'seed': seed, 'caster': caster, 'target': target or HOSTILE})

    def landed(self, caster, target=None, seeds=range(60)):
        for seed in seeds:
            out = self.swing(caster, target, seed)
            if out['outcome'] in ('hit', 'crit', 'glance', 'block') and out['damage'] > 0:
                yield seed, out

    def test_seal_of_righteousness_procs_on_each_landed_swing(self):
        seen = 0
        for seed in range(40):
            out = self.swing({**PALADIN_60, 'auras': [SOR_1]}, seed=seed)
            procs = [p for p in out.get('procs', []) if p['source_spell'] == SOR_1]
            if out['outcome'] in ('miss', 'dodge', 'parry') or out['damage'] == 0:   # a full block lands nothing
                self.assertEqual(procs, [])
                self.assertNotIn('refresh_judgements', out)
                continue
            seen += 1
            # Rank 1 bare-handed: 5, then 25742's own .029 x .2875 coefficient on zero spell damage; holy, no armor,
            # SPELL_ATTR_EX2_CANT_CRIT, always hits.
            self.assertEqual(procs, [{'source_spell': SOR_1, 'spell': 25742, 'target': 'target', 'cooldown_ms': 0,
                                      'school': 1, 'auras': [],
                                      'events': [{'type': 'damage', 'amount': 5, 'resisted': 0, 'crit': False,
                                                  'school': 1, 'spell': 25742}]}])
            self.assertTrue(out['refresh_judgements'])
        self.assertGreater(seen, 20)

    def test_seal_of_the_crusader_on_the_swing(self):
        ch = self.ch([PERDITIONS_BLADE], auras=[CRUSADER_6])
        plain = self.ch([PERDITIONS_BLADE])
        # Aura::HandleModAttackSpeed: 1800 / 1.4; GetAttackTime (AP, PPM) keeps 1800.
        self.assertEqual(melee.attack_time(ch, ch.melee_weapon()), 1285)
        base = F.f32(100 / 140.0)
        total = F.f32(F.f32(100.0 + F.f32(F.f32(-4000.0) / 140.0)) / 100.0)
        bonus = ch.attack_power() / 14.0 * 1.8
        self.assertEqual(ch.attack_power(), plain.attack_power() + 325)
        low, high, mult = melee.roll_range(ch, ch.melee_weapon())
        self.assertAlmostEqual(low, (73 * base + bonus) * total)
        self.assertAlmostEqual(high, (137 * base + bonus) * total)
        self.assertAlmostEqual(mult, total * base)
        out = self.swing({**PALADIN_60, 'items': [PERDITIONS_BLADE], 'auras': [CRUSADER_6]})
        self.assertEqual(out['delay_ms'], 1285)
        self.assertEqual(self.swing({**PALADIN_60, 'items': [PERDITIONS_BLADE]})['delay_ms'], 1800)

    def test_seal_of_the_crusader_attack_power_natively(self):
        # Every rank's attack power has its native counterpart at the gear rate (Blessing of Might's MIGHT_NATIVE: 0.1%
        # One- and Two-Handed damage a point); its haste and damage factors reach her native swing through the factor.
        native = {**PALADIN_60, 'items': [PERDITIONS_BLADE], 'skyrim': {'one_handed': 50}}
        ranks = sorted(s for s in self.cat.ranks if self.cat.chain(s) == self.cat.chain(CRUSADER_6))
        self.assertEqual(len(ranks), 6)
        for sid in ranks:
            self.assertEqual(self.cat.balance[sid].native, {'attack_power': CRUSADER_NATIVE})
            ap, = [a for a in self.cast(sid, native)['auras'] if a['type'] == 'attack_power']
            self.assertEqual([e['mgef'] for e in ap['skyrim']], [0x7A0FF, 0x7A106])
            self.assertTrue(all(abs(e['magnitude'] - ap['amount'] * 1.4 / 10) < 1e-9 for e in ap['skyrim']))
        fortify = next(a for a in self.cast(CRUSADER_6, native)['auras'] if a['type'] == 'attack_power')['skyrim'][0]
        self.assertAlmostEqual(fortify['magnitude'], 45.5)
        # The reviewer's setup (Perdition's Blade at native 10, One-Handed 50): a plain hit's native damage per second.
        caster = {**native, 'equipment_modifiers': {'weapon_entry': PERDITIONS_BLADE, 'weapon_skill': 'OneHanded',
                                                    'weapon_damage': 10}}

        def per_second(auras, fortified):
            ch = self.svc.char({**caster, 'auras': auras})
            low, high, mult = melee.roll_range(ch, ch.melee_weapon())
            hit = melee.native_factor(ch, low, high, mult) * (low + high) / 2
            return hit * (1 + fortified / 100) / melee.attack_time(ch, ch.melee_weapon()) * 1000
        plain = per_second([], 0)
        unmapped, sealed = per_second([CRUSADER_6], 0), per_second([CRUSADER_6], fortify['magnitude'])
        # Without the counterpart the seal's two cuts (base x 100/140, total x 100/140) outweigh its +40% speed: -29%.
        self.assertAlmostEqual(unmapped / plain, 1800 / 1285 * F.f32(100 / 140.0) ** 2, places=2)
        # With it, x 1.455: the gear rate restored by 140/100, since the native Fortify multiplies the cut hit while WoW's
        # attack power adds to it uncut. The seal then pays as in WoW (here +4% over her unsealed white damage).
        self.assertAlmostEqual(sealed / plain, unmapped / plain * 1.455)
        self.assertGreater(sealed / plain, 1)

    def test_proc_seals(self):
        caster = {**PALADIN_60, 'items': [PERDITIONS_BLADE], 'talents': {}}   # talent mode: finish skips the seal
        hits = 0
        for seed, out in self.landed({**caster, 'auras': [COMMAND_5]}):
            # 7 PPM x 1800 / 600 = 21%, its 3 s proc cooldown (spell_proc_event Cooldown, seconds in this dump).
            self.assertAlmostEqual(out['seal_proc_chance'], 21.0)
            procs = [p for p in out.get('procs', []) if p['source_spell'] == COMMAND_5]
            self.assertLessEqual(len(procs), 1)                 # talent_effects.finish leaves the seal alone
            for p in procs:
                hits += 1
                self.assertEqual((p['spell'], p['cooldown_ms'], p['target']), (20424, 3000, 'target'))
                self.assertIn(p['events'][0]['type'], ('damage', 'miss', 'dodge', 'parry'))
        self.assertGreater(hits, 3)
        cooling = [o for _, o in self.landed({**caster, 'auras': [COMMAND_5], 'proc_cooldowns': {str(COMMAND_5): 500}})]
        self.assertTrue(cooling)
        self.assertFalse(any(p['source_spell'] == COMMAND_5 for o in cooling for p in o.get('procs', [])))
        # Light and Wisdom: 20 PPM; their payloads are DmgClass none, cannot crit, take no healing bonus.
        for sid, event in ((LIGHT_4, {'type': 'heal', 'amount': 94, 'crit': False, 'school': 1, 'spell': 20340}),
                           (WISDOM_3, {'type': 'power', 'power': 'mana', 'amount': 90})):
            procs = [p for _, o in self.landed({**PALADIN_60, 'items': [PERDITIONS_BLADE], 'auras': [sid]})
                     for p in o.get('procs', []) if p['source_spell'] == sid]
            self.assertTrue(procs)
            self.assertTrue(all(p['events'] == [event] and p['target'] == 'caster' for p in procs))
        # Justice: 5 PPM, a 2 s stun with its own magic hit roll.
        procs = [p for _, o in self.landed({**PALADIN_60, 'items': [ASHKANDI], 'auras': [JUSTICE]}, seeds=range(200))
                 for p in o.get('procs', []) if p['source_spell'] == JUSTICE]
        self.assertTrue(procs)
        for p in procs:
            self.assertTrue(p['auras'] and p['auras'][0]['type'] == 'stun' and p['auras'][0]['duration_ms'] == 2000
                            or p['events'][0]['type'] in ('miss', 'resist'))

    # --- Judgement ------------------------------------------------------------------------------------------

    def judge(self, seal, target=None, seed=1, **caster):
        return self.answer({'op': 'resolve', 'spell': 20271, 'seed': seed,
                            'caster': {**PALADIN_60, 'auras': [seal] if seal else [], **caster},
                            'target': target or HOSTILE})

    def test_judgement_needs_a_seal_and_consumes_it(self):
        self.assertEqual(self.judge(None)['error'], 'SPELL_FAILED_CASTER_AURASTATE')
        two = self.answer({'op': 'resolve', 'spell': 20271, 'caster': {**PALADIN_60, 'auras': [SOR_8, CRUSADER_6]},
                           'target': HOSTILE})
        self.assertEqual(two['error'], 'BAD_REQUEST: caster.auras: one seal at a time')
        # In talent mode too the payload and the consumed seal survive talent_effects.finish.
        out = self.judge(SOR_8, talents={})
        self.assertEqual((out['consumed_auras'], out['procs'][0]['spell']), ([SOR_8], 20286))
        self.assertEqual(self.judge(SOR_8, dict(HOSTILE, distance_yd=11))['error'], 'SPELL_FAILED_OUT_OF_RANGE')
        self.assertEqual(self.judge(SOR_8, dict(HOSTILE, reaction='friendly'))['error'], 'SPELL_FAILED_BAD_TARGETS')
        out = self.judge(SOR_1)               # nothing to judge: cast, cost and cooldown, the seal stays
        self.assertTrue(out['ok'], out)
        self.assertEqual((out['cost'], out['cooldown_ms'], out['gcd_ms']), (90, 10000, 0))
        self.assertNotIn('procs', out)
        self.assertNotIn('consumed_auras', out)

    def test_judgement_of_righteousness(self):
        # A level 70 target: Judgement never misses (DmgClass none; the judgement ignores the hit roll), though its
        # damage still meets the level-based partial resist (CalculateEffectiveMagicResistancePercent).
        for seed in range(20):
            self.assertEqual(self.judge(SOR_8, dict(HOSTILE, level=70), seed)['procs'][0]['events'][0]['type'], 'damage')
        for seed in range(6):
            out = self.judge(SOR_8, seed=seed)
            self.assertEqual(out['consumed_auras'], [SOR_8])
            (p,) = out['procs']
            self.assertEqual((p['spell'], p['source_spell'], p['judgement'], p['target'], p['stack'], p['stack_rule']),
                             (20286, SOR_8, True, 'target', 'Judgement', 'unique_per_caster'))
            event = p['events'][0]
            self.assertEqual(event['type'], 'damage')
            amount = event['amount'] / (1.5 if event['crit'] else 1)
            self.assertTrue(170 <= amount <= 186, event)        # 161 + 4.1 x 2 = 169.2, + 1..17
        # Crusader's 140 holy damage taken through the rank's .5 coefficient: +70.
        plain = self.judge(SOR_8, seed=2)['procs'][0]['events'][0]
        judged = self.judge(SOR_8, dict(HOSTILE, judgements=[JOTC_6]), seed=2)['procs'][0]['events'][0]
        self.assertFalse(plain['crit'] or judged['crit'])
        self.assertEqual(judged['amount'] - plain['amount'], 70)

    def test_judgement_debuffs_and_command(self):
        p = self.judge(CRUSADER_6)['procs'][0]
        self.assertEqual((p['spell'], p['events']), (JOTC_6, []))
        self.assertEqual([(a['type'], a['amount'], a['school_mask'], a['duration_ms']) for a in p['auras']],
                         [('damage_taken', 140, 2, 10000)])
        self.assertEqual(p['stack_amounts'], {'6:14:+': 140})
        p = self.judge(JUSTICE)['procs'][0]
        self.assertEqual([(a['type'], a['duration_ms']) for a in p['auras']], [('prevent_fleeing', 10000)])
        for seal, judgement, payload, kind, amount in ((LIGHT_4, JOL_4, 20343, 'heal', 61),
                                                         (WISDOM_3, JOW_3, 20353, 'mana', 59)):
            (a,) = self.judge(seal)['procs'][0]['auras']
            self.assertEqual((a['type'], a['payload'], a['payload_type'], a['payload_amount'], a['proc_chance']),
                             ('judgement_proc', payload, kind, amount, 50))
        # Command: 20966's 339-373, halved unless the target is stunned (Paladin.cpp:157-164); a melee crit is x2.
        for seed in range(6):
            for stunned in (False, True):
                p = self.judge(COMMAND_5, dict(HOSTILE, stunned=stunned), seed)['procs'][0]
                self.assertEqual(p['spell'], 20968)
                event = p['events'][0]
                amount = event['amount'] / (2 if event['crit'] else 1)
                low, high = (169, 186) if not stunned else (339, 373)
                self.assertTrue(low <= amount <= high, (stunned, event))
                self.assertEqual(event['halved'], not stunned)

    def test_judged_targets_light_and_wisdom_procs(self):
        target = dict(HOSTILE, judgements=[JOL_4])
        procs = [p for _, o in self.landed(PALADIN_60, target, range(80)) for p in o.get('procs', [])]
        self.assertTrue(procs)
        self.assertTrue(all(p == {'source_spell': JOL_4, 'spell': 20343, 'target': 'caster', 'cooldown_ms': 0,
                                  'school': 1, 'auras': [],
                                  'events': [{'type': 'heal', 'amount': 61, 'crit': False, 'school': 1, 'spell': 20343}]}
                            for p in procs))
        landed = sum(1 for _ in self.landed(PALADIN_60, target, range(80)))
        self.assertTrue(.25 < len(procs) / landed < .75)       # ProcChance 50
        procs = [p for _, o in self.landed(PALADIN_60, dict(HOSTILE, judgements=[JOW_3])) for p in o.get('procs', [])]
        self.assertTrue(procs and all(p['events'] == [{'type': 'power', 'power': 'mana', 'amount': 59}] for p in procs))
        self.assertEqual(self.answer({'op': 'melee', 'caster': PALADIN_60, 'target': dict(HOSTILE, judgements=[20343])})
                         ['error'], 'BAD_REQUEST: target.judgements must be an array of judgement spell IDs')

    def test_judged_targets_procs_from_her_spells_and_strikes(self):
        # Every landed damaging hit of hers raises the judged unit's procs by the hit's victim flag (Spell::
        # PrepareMasksForProcSystem): Light 0x28 (melee swings and abilities), Wisdom 0x222a8 (also ranged attacks and
        # abilities, harmful spells and abilities); a triggered hit only with SPELL_ATTR_EX3_NOT_A_PROC.
        light = {'source_spell': JOL_4, 'spell': 20343, 'target': 'caster', 'cooldown_ms': 0, 'school': 1, 'auras': [],
                 'events': [{'type': 'heal', 'amount': 61, 'crit': False, 'school': 1, 'spell': 20343}]}
        wisdom = {'source_spell': JOW_3, 'spell': 20353, 'target': 'caster', 'cooldown_ms': 0, 'school': 1, 'auras': [],
                  'events': [{'type': 'power', 'power': 'mana', 'amount': 59}]}
        undead = dict(HOSTILE, creature_type='Undead', health_pct=10, distance_yd=5)
        # Exorcism (magic: TAKE_HARMFUL_SPELL) and Hammer of Wrath (TAKE_RANGED_ABILITY): Wisdom only, 50%.
        for sid in (10314, 24239):
            procs, landed = [], 0
            for seed in range(60):
                out = self.ok({'op': 'resolve', 'spell': sid, 'seed': seed, 'caster': PALADIN_60,
                               'target': dict(undead, judgements=[JOW_3])})
                mine = [p for p in out.get('procs', []) if p.get('from_spell') == sid]
                if any(e['type'] == 'damage' and e['amount'] > 0 for e in out['events']):
                    landed += 1
                else:
                    self.assertEqual(mine, [])           # a miss or a full resist raises nothing
                procs += mine
            self.assertTrue(.25 < len(procs) / landed < .75, sid)
            self.assertTrue(all(p == {**wisdom, 'from_spell': sid} for p in procs))
            for seed in range(30):
                self.assertNotIn('procs', self.ok({'op': 'resolve', 'spell': sid, 'seed': seed, 'caster': PALADIN_60,
                                                   'target': dict(undead, judgements=[JOL_4])}))
        # Judgement's strikes: Righteousness's (a spell, not triggered) raises Wisdom; Command's damage rank (a melee
        # ability triggered by its dummy, NOT_A_PROC) raises Light and Wisdom.
        for seal, judgement, hit, payload in ((SOR_8, JOW_3, 20286, wisdom), (COMMAND_5, JOL_4, 20966, light),
                                              (COMMAND_5, JOW_3, 20966, wisdom)):
            procs = [p for seed in range(30) for p in self.judge(seal, dict(HOSTILE, judgements=[judgement]), seed)['procs']
                     if p.get('from_spell') == hit]
            self.assertTrue(procs and all(p == {**payload, 'from_spell': hit} for p in procs), (seal, judgement))
        for seed in range(30):
            self.assertEqual(len(self.judge(SOR_8, dict(HOSTILE, judgements=[JOL_4]), seed)['procs']), 1)
        # Holy Wrath: each struck victim under her judgement, named.
        victims = [{'id': 1, 'level': 60, 'alive': True, 'reaction': 'hostile', 'distance_yd': 4, 'creature_type': 'Undead',
                    'judgements': [JOW_3]},
                   {'id': 2, 'level': 60, 'alive': True, 'reaction': 'hostile', 'distance_yd': 4, 'creature_type': 'Undead'}]
        procs = [p for seed in range(30) for p in self.ok({'op': 'resolve', 'spell': 10318, 'seed': seed, 'caster': PALADIN_60,
                                                           'target': {'kind': 'self'}, 'targets': victims}).get('procs', [])]
        self.assertTrue(procs and all(p == {**wisdom, 'from_spell': 10318, 'victim': 1} for p in procs))
        # On her swing: Seal of Command's strike (20424, triggered, NOT_A_PROC) rolls its own; Righteousness's payload
        # (triggered, without it) does not; the white swing's own roll carries no from_spell.
        caster = {**PALADIN_60, 'items': [PERDITIONS_BLADE], 'auras': [COMMAND_5]}
        strikes = []
        for _, out in self.landed(caster, dict(HOSTILE, judgements=[JOL_4]), range(200)):
            strike = [p for p in out.get('procs', []) if p['source_spell'] == COMMAND_5]
            mine = [p for p in out.get('procs', []) if p.get('from_spell') == 20424]
            if not (strike and any(e['type'] == 'damage' and e['amount'] > 0 for e in strike[0]['events'])):
                self.assertEqual(mine, [])
            strikes += mine
        self.assertTrue(strikes and all(p == {**light, 'from_spell': 20424} for p in strikes))
        for _, out in self.landed({**PALADIN_60, 'auras': [SOR_8]}, dict(HOSTILE, judgements=[JOL_4]), range(40)):
            self.assertFalse([p for p in out.get('procs', []) if 'from_spell' in p])

    # --- Skyrim rules -----------------------------------------------------------------------------------------

    def test_every_rank_and_payload_has_a_paladin_rule(self):
        chains = {635, 19750, 633, 20271, 20154, 21082, 20375, 20165, 20166, 20164}
        ranks = [sid for sid, row in self.cat.ranks.items() if row['chain'] in chains]
        self.assertEqual(len(ranks), 9 + 6 + 3 + 1 + 9 + 6 + 5 + 4 + 3 + 1)
        payloads = {to for kind, rows in paladin.links().items() for source, tos in rows.items() for to in tos
                    if kind in ('dummy_proc', 'trigger', 'judgement') and paladin.is_seal(source)
                    or kind == 'script' and source in paladin.judgements()}
        self.assertEqual(len(payloads), 9 + 1 + 4 + 3 + 1 + 8 + 6 + 5 + 4 + 3 + 1 + 4 + 3)
        for sid in ranks + sorted(payloads):
            rule = self.cat.balance[sid]
            with self.subTest(spell=sid):
                self.assertNotIn('fallback', rule.reason)
                self.assertIn(rule.skill, skyrim.SKILLS)
        self.assertAlmostEqual(self.cat.balance[HOLY_LIGHT_9].health_factor, HEALING_TOUCH.health_factor)
        self.assertAlmostEqual(self.cat.balance[20286].health_factor, WRATH.health_factor)
        self.assertEqual(self.cat.balance[25713].health_factor, FERAL_MELEE.health_factor)
        for sid, rule in PALADIN.items():
            s = self.cat.spell(sid)
            self.assertEqual(rule.wow_cost, max(1, s['ManaCost'] + s['ManaCostPercentage'] * 1512 // 100), sid)

    def test_skyrim_costs_skills_and_health(self):
        skills = {'restoration': 50, 'destruction': 30, 'one_handed': 20, 'two_handed': 80}
        caster = {**PALADIN_60, 'skyrim': skills}
        out = self.cast(HOLY_LIGHT_9, caster)
        self.assertEqual((out['skill'], out['cost']), ('Restoration', round(126 * 660 / 800 * skyrim.cost_factor(50))))
        self.assertEqual(out['events'][0]['amount'], round(self.cast(HOLY_LIGHT_9)['events'][0]['amount'] *
                                                           HEALING_TOUCH.health_factor))
        # A seal trains the weapon she swings and costs Wrath's magicka per WoW mana.
        for items, skill in (([], 'OneHanded'), ([PERDITIONS_BLADE], 'OneHanded'), ([ASHKANDI], 'TwoHanded')):
            out = self.cast(SOR_8, {**caster, 'items': items})
            level = skills[skyrim.SKILLS[skill]]
            self.assertEqual((out['skill'], out['cost']), (skill, round(41 * 200 / 180 * skyrim.cost_factor(level))))
            costs = self.ok({'op': 'skyrim_costs', 'caster': {**caster, 'items': items}})['costs']
            self.assertEqual(costs[str(SOR_8)], out['cost'])
        out = self.ok({'op': 'resolve', 'spell': 20271, 'seed': 1, 'caster': {**caster, 'auras': [SOR_8]},
                       'target': HOSTILE})
        self.assertEqual((out['skill'], out['cost']), ('Destruction', round(41 * 90 / 180 * skyrim.cost_factor(30))))
        wow = self.judge(SOR_8)['procs'][0]['events'][0]['amount']
        self.assertEqual(out['procs'][0]['events'][0]['amount'], max(1, round(wow * WRATH.health_factor)))
        # Lay on Hands: no price but all her magicka; her maximum health and the mana as her heals spend it.
        out = self.cast(LOH_3, caster, dict(HOSTILE, reaction='friendly', distance_yd=5))
        self.assertEqual((out['cost'], out['use_all_mana']), (0, True))
        self.assertEqual(out['events'], [{'type': 'heal', 'amount': round(2201 * HEALING_TOUCH.health_factor),
                                          'crit': False, 'school': 1, 'caster_max_health': 1.0},
                                         {'type': 'power', 'power': 'mana', 'amount': round(550 * 126 / 800)}])

    def test_seal_procs_take_the_swings_native_weapon_factor(self):
        caster = {**PALADIN_60, 'items': [PERDITIONS_BLADE], 'auras': [SOR_8], 'skyrim': {'one_handed': 40},
                  'equipment_modifiers': {'weapon_entry': PERDITIONS_BLADE, 'weapon_skill': 'OneHanded',
                                          'weapon_damage': 11}}
        wow_caster = {k: v for k, v in caster.items() if k not in ('skyrim', 'equipment_modifiers')}
        for seed, out in self.landed(caster, seeds=range(10)):
            wow = self.swing(wow_caster, seed=seed)
            ch = self.svc.char(caster)
            low, high, mult = melee.roll_range(ch, ch.melee_weapon())
            factor = melee.native_factor(ch, low, high, mult)
            proc = next(p for p in out['procs'] if p['source_spell'] == SOR_8)
            wow_proc = next(p for p in wow['procs'] if p['source_spell'] == SOR_8)
            self.assertEqual(proc['events'][0]['amount'], max(1, round(wow_proc['events'][0]['amount'] * factor)))
        # Without a native weapon: the white-hit budget, Iron Sword per 96.5 at +0.5% a One-Handed point.
        plain = {**PALADIN_60, 'auras': [SOR_8], 'skyrim': {'one_handed': 40}}
        for seed, out in self.landed(plain, seeds=range(10)):
            proc = next(p for p in out['procs'] if p['source_spell'] == SOR_8)
            self.assertEqual(proc['events'][0]['amount'], round(39 * 7 / 96.5 * 1.2))

    def test_a_catalog_without_the_linked_spells(self):
        # build/catalog.json may predate the manifest: the ranks without 19968, the payloads and the judgements.
        linked = {to for rows in paladin.links().values() for tos in rows.values() for to in tos} - set(self.cat.ranks)
        raw = {**self.cat.raw, 'spells': {k: v for k, v in self.cat.raw['spells'].items() if int(k) not in linked}}
        svc = Service(Catalog(raw=raw), None)
        ask = lambda req: svc.respond(json.dumps(req).encode())
        self.assertEqual(ask({'op': 'resolve', 'spell': HOLY_LIGHT_9, 'caster': PALADIN_60, 'target': {'kind': 'self'}})
                         ['error'], 'SPELL_FAILED_SPELL_UNAVAILABLE')
        self.assertEqual(ask({'op': 'resolve', 'spell': 20271, 'caster': {**PALADIN_60, 'auras': [SOR_8]},
                              'target': HOSTILE})['error'], 'SPELL_FAILED_SPELL_UNAVAILABLE')
        self.assertTrue(ask({'op': 'melee', 'caster': {**PALADIN_60, 'auras': [SOR_8]}, 'target': HOSTILE})['ok'])
        self.assertTrue(ask({'op': 'skyrim_costs', 'caster': {**PALADIN_60, 'talents': {}, 'skyrim': {}}})['ok'])


if __name__ == '__main__':
    unittest.main()
