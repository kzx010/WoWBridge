"""Paladin defence and control (service/paladin_defense.py) against CMaNGOS Classic @8ec338a and the 1.12.1 Spell.dbc.

Expected values are hand computed from the Spell.dbc rows (catalog built in memory: data.build_catalog.build) and the
cited CMaNGOS routines; the Druid's outputs are unchanged (test_druid_never_reaches_the_paladin_paths and the
before/after snapshot recorded in the worker report)."""
import json
import random
import unittest

from data.build_catalog import build
from service import character, formulas as F, melee, paladin_defense as P, skyrim, spells
from service.catalog import Catalog
from service.server import Service
from service.skyrim_balance import COUNTERPARTS, FERAL_MELEE, PALADIN_DEFENSE

HOSTILE = {'kind': 'unit', 'level': 60, 'reaction': 'hostile', 'alive': True, 'distance_yd': 5, 'los': True}
NATIVE = {'alteration': 50, 'restoration': 50}
IMPROVED_FURY = {'1421': 5, '1423': 5, '1501': 3}   # 10 Protection points open tier 3


def own(out):
    """The Paladin procs of an incoming hit (Holy Shield, Redoubt, Reckoning), apart from her gear's."""
    return [p for p in out.get('procs', []) if p['spell'] in P.HOLY_SHIELD + P.REDOUBT_BUFFS + (P.RECKONING_ATTACK,)]


class PaladinDefense(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog(raw=build())
        cls.shield = next(e for e, it in sorted(cls.cat.items.items()) if it['InventoryType'] == character.INVTYPE_SHIELD)
        cls.sword = next(e for e, it in sorted(cls.cat.items.items())
                         if it['class'] == 2 and it['InventoryType'] in character.INVTYPE_WEAPONS and it['subclass'] == 7)

    def setUp(self):
        self.svc = Service(self.cat, None)
        self.caster = {'class': 2, 'race': 1, 'level': 60, 'items': [self.shield, self.sword]}

    def ask(self, req):
        return self.svc.respond(json.dumps(req).encode())

    def cast(self, sid, target=None, seed=1, **caster):
        return self.ask({'op': 'resolve', 'spell': sid, 'seed': seed, 'caster': {**self.caster, **caster},
                         'target': target or {'kind': 'self'}})

    def hit(self, seed, damage=600, attacker=None, caster=None, **attack):
        return self.ask({'op': 'incoming_hit', 'weapon_hit': True, 'seed': seed, 'caster': {**self.caster, **(caster or {})},
                         'target': {**HOSTILE, **(attacker or {})}, 'attack': {'damage': damage, **attack}})

    def ok(self, out):
        self.assertTrue(out['ok'], out)
        return out

    # --- data ----------------------------------------------------------------------------------------------------

    def test_rank_ids_are_the_catalog_chains(self):
        for chain in (P.DIVINE_PROTECTION, P.DIVINE_SHIELD, P.BLESSING_OF_PROTECTION, P.HAMMER_OF_JUSTICE, P.HAMMER_OF_WRATH,
                      P.EXORCISM, P.HOLY_WRATH, P.TURN_UNDEAD, P.CONSECRATION, P.RIGHTEOUS_FURY, P.HOLY_SHIELD, P.REDOUBT,
                      P.RECKONING, P.LAY_ON_HANDS):
            with self.subTest(chain=chain[0]):
                self.assertEqual(sorted(chain), sorted(s for s in self.cat.ranks if self.cat.chain(s) == chain[0]))
                self.assertEqual(len({self.cat.spell(s)['SpellName'] for s in chain}), 1)
        for passive, buff in zip(P.REDOUBT, P.REDOUBT_BUFFS):
            self.assertEqual(self.cat.spell(passive)['EffectTriggerSpell1'], buff)
        self.assertEqual({self.cat.spell(s)['EffectTriggerSpell1'] for s in P.RECKONING}, {P.RECKONING_ATTACK})
        # Spell.dbc: Mechanic 25 on all three protections only; Forbearance is MECHANIC_IMMUNITY to it for 60 s.
        self.assertEqual({s for s in self.cat.ranks if self.cat.spell(s)['Mechanic'] == 25}, set(P.INVULNERABILITY))
        f = self.cat.spell(P.FORBEARANCE)
        self.assertEqual((f['EffectApplyAuraName1'], f['EffectMiscValue1'], self.cat.duration(f, 60)), (77, 25, 60000))
        self.assertTrue(f['Attributes'] & 0x20000000)   # SPELL_ATTR_NO_IMMUNITIES: lands through Divine Shield

    # --- Divine Protection, Divine Shield, Blessing of Protection, Forbearance -------------------------------------

    def test_divine_shield_all_schools_half_speed_and_forbearance(self):
        out = self.ok(self.cast(1020))
        immune = [a for a in out['auras'] if a['type'] == 'school_immunity']
        self.assertEqual([a['school_mask'] for a in immune], [1, 126])
        self.assertTrue(all(a['purges_existing'] and a['suppresses_threat'] and a['duration_ms'] == 12000 for a in immune))
        haste = next(a for a in out['auras'] if a['type'] == 'melee_haste')
        self.assertEqual((haste['amount'], haste['attack_time_multiplier']), (-100, 2.0))   # ApplyPercentModFloatVar(100)
        self.assertFalse(any(a['type'] == 'pacify' for a in out['auras']))
        forbearance = out['auras'][-1]
        self.assertEqual((forbearance['spell'], forbearance['type'], forbearance['mechanic'], forbearance['duration_ms']),
                         (P.FORBEARANCE, 'mechanic_immunity', 25, 60000))
        self.assertEqual(out['immunity'], {'school_mask': 127, 'duration_ms': 12000})
        self.assertEqual((out['cooldown_ms'], out['cooldown_category'], out['category_spells']), (300000, 37, [498, 642, 1020, 5573]))

    def test_divine_protection_is_every_school_with_pacify(self):
        # 1.12.1 Spell.dbc 5573: PACIFY, SCHOOL_IMMUNITY 1 and SCHOOL_IMMUNITY 126 - spells as well as attacks.
        out = self.ok(self.cast(5573))
        self.assertEqual(out['immunity'], {'school_mask': 127, 'duration_ms': 8000})
        self.assertIn('pacify', [a['type'] for a in out['auras']])
        self.assertEqual(out['category_spells'], [498, 642, 1020, 5573])   # shares category 37 with Divine Shield
        self.assertEqual(self.ok(self.cast(498))['immunity']['duration_ms'], 6000)

    def test_blessing_of_protection_targets_her_group_and_carries_forbearance(self):
        follower = {'kind': 'unit', 'level': 40, 'friendly': True, 'party': True, 'distance_yd': 12}
        out = self.ok(self.cast(10278, follower))
        self.assertEqual(out['immunity'], {'school_mask': 1, 'duration_ms': 10000})
        self.assertEqual([a['type'] for a in out['auras']], ['school_immunity', 'pacify', 'mechanic_immunity'])
        self.assertEqual((out['cost'], out['cooldown_category'], out['category_spells']), (105, 20, [1022, 5599, 10278]))
        self.assertNotIn('hit_chance', out)   # positive (TARGET_UNIT_RAID): no hit roll
        self.assertEqual(self.cast(10278, {**follower, 'party': False})['error'], 'SPELL_FAILED_BAD_TARGETS')
        self.assertEqual(self.cast(10278, {**HOSTILE, 'party': True})['error'], 'SPELL_FAILED_BAD_TARGETS')
        self.assertEqual(self.cast(10278, {**follower, 'distance_yd': 31})['error'], 'SPELL_FAILED_OUT_OF_RANGE')
        self.ok(self.cast(10278))   # on herself
        # Forbearance on the follower (the plugin lists its WoW auras) refuses it; on herself it refuses all three.
        self.assertEqual(self.cast(10278, {**follower, 'auras': [P.FORBEARANCE]})['error'], 'SPELL_FAILED_TARGET_AURASTATE')
        self.ok(self.cast(1020, auras=[]))
        for sid in P.INVULNERABILITY:
            self.assertEqual(self.cast(sid, auras=[P.FORBEARANCE])['error'], 'SPELL_FAILED_TARGET_AURASTATE', sid)
        self.assertEqual(self.cast(10278, {**follower, 'auras': 'x'})['error'].split(':')[0], 'BAD_REQUEST')

    def test_lay_on_hands_and_forbearance_do_not_interact_in_1_12(self):
        # Mechanic 0: Spell::prepare adds no Forbearance and Forbearance's MECHANIC_IMMUNITY 25 does not refuse it.
        for sid in P.LAY_ON_HANDS:
            self.assertEqual(self.cat.spell(sid)['Mechanic'], 0)
            out = self.ok(self.cast(sid, auras=[P.FORBEARANCE]))
            self.assertFalse(any(a.get('spell') == P.FORBEARANCE for a in out['auras']))

    def test_pacify_stops_her_swing_and_divine_shield_doubles_it(self):
        target = {**HOSTILE, 'distance_yd': 2, 'facing': True}
        plain = self.ok(self.ask({'op': 'melee', 'seed': 3, 'caster': self.caster, 'target': target}))
        for sid in (498, 5573, 1022, 10278):
            out = self.ask({'op': 'melee', 'seed': 3, 'caster': {**self.caster, 'auras': [sid]}, 'target': target})
            self.assertEqual(out['error'], 'ERR_ATTACK_PACIFIED', sid)
        shielded = self.ok(self.ask({'op': 'melee', 'seed': 3, 'caster': {**self.caster, 'auras': [1020]}, 'target': target}))
        self.assertEqual(shielded['delay_ms'], plain['delay_ms'] * 2)
        self.assertEqual(shielded['damage'], plain['damage'])
        self.assertEqual(P.attack_time_multiplier(20), 100 / 120)

    def test_immunity_on_damage_taken(self):
        req = {'op': 'damage_taken', 'seed': 3, 'damage': 100, 'victim_level': 60, 'caster': {**self.caster, 'auras': [1020]}}
        for school in range(1, 7):
            out = self.ok(self.ask({**req, 'school': school}))
            self.assertEqual((out['damage'], out['immune'], out['break']), (0, True, False))
        out = self.ok(self.ask({**req, 'school': 2, 'caster': {**self.caster, 'auras': [10278]}}))
        self.assertNotIn('immune', out)    # Blessing of Protection: physical only

    # --- Hammer of Justice, Hammer of Wrath --------------------------------------------------------------------------

    def test_hammer_of_justice_is_the_existing_stun(self):
        out = self.ok(self.cast(10308, HOSTILE))
        self.assertEqual([(a['type'], a['duration_ms'], a['breaks_on_damage'], a['single_target']) for a in out['auras']],
                         [('stun', 6000, False, False)])
        self.assertEqual((out['hit_chance'], out['cooldown_category'], out['category_cooldown_ms']), (96.0, 32, 60000))
        self.assertEqual(self.cast(853, HOSTILE)['auras'][0]['duration_ms'], 3000)
        self.assertEqual(self.cast(10308, {**HOSTILE, 'distance_yd': 11})['error'], 'SPELL_FAILED_OUT_OF_RANGE')

    def test_hammer_of_wrath_needs_the_target_below_20_percent(self):
        # Unit::Update (Unit.cpp:534): AURA_STATE_HEALTHLESS_20_PERCENT while health < max x 0.20f.
        for target, error in (({'health_pct': 19.9}, None), ({'health_pct': 20}, 'SPELL_FAILED_TARGET_AURASTATE'),
                              ({'health': 199, 'max_health': 1000}, None),
                              ({'health': 200, 'max_health': 1000}, 'SPELL_FAILED_TARGET_AURASTATE'),
                              ({}, 'SPELL_FAILED_TARGET_AURASTATE')):
            with self.subTest(target=target):
                out = self.cast(24239, {**HOSTILE, **target})
                self.assertEqual(out.get('error'), error)

    def test_hammer_of_wrath_rolls_as_a_ranged_ability(self):
        ch = self.svc.char(self.caster)
        s = self.cat.spell(24239)
        target = {**HOSTILE, 'health_pct': 10}
        crit = ch.melee_crit()
        events = []
        for seed in range(600):
            out = self.ok(self.cast(24239, target, seed=seed))
            self.assertEqual((out['hit_chance'], out['crit_chance'], out['cast_ms'], out['cooldown_ms']), (95.0, crit, 1000, 6000))
            events.append(out['events'][0])
        kinds = {e['type'] for e in events}
        self.assertEqual(kinds, {'damage', 'miss'})                       # no resist side, dodge or parry
        hits = [e for e in events if e['type'] == 'damage' and not e['crit'] and not e['blocked'] and not e['resisted']]
        self.assertGreaterEqual(min(e['amount'] for e in hits), 504)
        self.assertLessEqual(max(e['amount'] for e in hits), 556)
        crits = [e for e in events if e['type'] == 'damage' and e['crit'] and not e['blocked']]
        self.assertTrue(crits and all(1008 <= e['amount'] <= 1112 for e in crits))   # x2, not a spell's x1.5
        blocks = [e for e in events if e['type'] == 'damage' and e['blocked']]
        self.assertTrue(blocks and all(e['blocked'] == 60 // 2 + int(self.cat.creature_strength(60) / 20) for e in blocks))
        # Against a level-63 creature: 5 + 1 + 5 x 0.2 + 5 x 0.4 = 9% miss, 15 points of crit suppression x 0.2.
        far = self.ok(self.cast(24239, {**target, 'level': 63}))
        self.assertAlmostEqual(far['hit_chance'], 91.0)
        self.assertAlmostEqual(far['crit_chance'], max(0.0, crit - 3.0))
        self.assertEqual(P._ability_miss(ch, s, 300, 300, True), 5.0)

    def test_hammer_of_wrath_needs_a_hostile_unit(self):
        # TARGET_UNIT_ENEMY: herself or a point is refused as a bad target, not answered BAD_REQUEST (it read the level).
        for target in ({'kind': 'self'}, {'kind': 'self', 'health_pct': 10}, {'kind': 'ground', 'distance_yd': 5}):
            with self.subTest(target=target):
                self.assertEqual(self.cast(24239, target)['error'], 'SPELL_FAILED_BAD_TARGETS')
        self.ok(self.cast(24239, {**HOSTILE, 'health_pct': 10}))

    def test_hammer_of_wrath_rolls_with_the_ranged_attack_types_auras(self):
        # Unit::GetHitChance / GetCritChance by GetWeaponAttackType (RANGED_ATTACK): an item-neutral aura (Spell.dbc
        # EquippedItemClass -1: 15465 +2% hit, 7598 +2% crit) counts; a main-hand weapon aura does not (test_paladin_talents).
        target = {**HOSTILE, 'health_pct': 10}
        base = self.ok(self.cast(24239, target))
        hit = self.ok(self.cast(24239, target, auras=[15465]))
        crit = self.ok(self.cast(24239, target, auras=[7598]))
        self.assertEqual((hit['hit_chance'], hit['crit_chance']), (base['hit_chance'] + 2, base['crit_chance']))
        self.assertEqual((crit['hit_chance'], crit['crit_chance']), (base['hit_chance'], base['crit_chance'] + 2))

    # --- Judgement of the Crusader on her Holy damage ----------------------------------------------------------------

    def judged_bonus(self, sid, eff=1):
        """Judgement of the Crusader rank 6's 140 Holy damage taken through the spell's coefficient and level penalty
        (Unit::SpellDamageBonusTaken / MeleeDamageBonusTaken -> SpellBonusWithCoeffs)."""
        s = self.cat.spell(sid)
        return int(140 * s[f'EffectBonusCoefficient{eff}'] * F.level_penalty(s))

    def test_crusader_judgement_raises_exorcism_and_hammer_of_wrath(self):
        # After her done bonuses and before the crit; Hammer of Wrath (ranged) through MeleeDamageBonusTaken.
        self.assertEqual((self.judged_bonus(10314), self.judged_bonus(24239)), (60, 60))
        undead = {**HOSTILE, 'creature_type': 'Undead', 'health_pct': 10}
        for sid, multiplier in ((10314, 1.5), (24239, 2.0)):
            crits = set()
            for seed in range(60):
                plain = self.ok(self.cast(sid, undead, seed=seed))['events'][0]
                judged = self.ok(self.cast(sid, {**undead, 'judgements': [20303]}, seed=seed))['events'][0]
                self.assertEqual(plain['type'], judged['type'])
                if plain['type'] != 'damage' or plain['resisted'] or judged['resisted']:
                    continue
                self.assertEqual(plain['crit'], judged['crit'])
                if plain['crit']:
                    self.assertLessEqual(abs(judged['amount'] - plain['amount'] - 60 * multiplier), 1, (sid, seed))
                else:
                    self.assertEqual(judged['amount'] - plain['amount'], 60, (sid, seed))
                crits.add(plain['crit'])
            self.assertIn(False, crits)
        # Native: a lower rank's addition converts at the top rank's rate (as her gear's), not the rank's own.
        rank_1 = self.cat.balance[879]
        self.assertGreater(rank_1.health_factor, rank_1.bonus_health_factor)
        seen = 0
        for seed in range(12):
            plain = self.ok(self.cast(879, undead, seed=seed, skyrim=NATIVE))['events'][0]
            judged = self.ok(self.cast(879, {**undead, 'judgements': [20303]}, seed=seed, skyrim=NATIVE))['events'][0]
            if plain['type'] == 'damage' and not plain['crit'] and not plain['resisted'] and not judged['resisted']:
                self.assertLessEqual(abs(judged['amount'] - plain['amount'] - 60 * rank_1.bonus_health_factor), 1, seed)
                seen += 1
        self.assertTrue(seen)

    def test_crusader_judgement_on_holy_wrath_victims_and_consecration_ticks(self):
        self.assertEqual((self.judged_bonus(10318), self.judged_bonus(20924)), (26, 5))
        victim = {'id': 1, 'level': 60, 'alive': True, 'reaction': 'hostile', 'distance_yd': 4, 'creature_type': 'Undead'}
        seen = 0
        for seed in range(40):
            plain, = self.ok(self.ask({'op': 'resolve', 'spell': 10318, 'seed': seed, 'caster': self.caster,
                                       'target': {'kind': 'self'}, 'targets': [victim]}))['results']
            judged, = self.ok(self.ask({'op': 'resolve', 'spell': 10318, 'seed': seed, 'caster': self.caster,
                                        'target': {'kind': 'self'}, 'targets': [{**victim, 'judgements': [20303]}]}))['results']
            if plain['type'] == 'damage' and not plain['crit'] and not plain['resisted'] and not judged['resisted']:
                self.assertEqual(judged['amount'] - plain['amount'], 26)
                seen += 1
        self.assertTrue(seen)
        # Each tick per victim (Aura::PeriodicTick: SpellDamageBonusTaken), Consecration rank 5's 48 + 140 x .042.
        ticks = 0
        for seed in range(20):
            out = self.ok(self.ask({'op': 'tick_area', 'spell': 20924, 'seed': seed, 'caster': self.caster,
                                    'targets': [{'id': 1, 'level': 60}, {'id': 2, 'level': 60, 'judgements': [20303]}]}))
            self.assertNotIn('procs', out)     # periodic damage is outside Light's and Wisdom's proc flags
            for r in out['results']:
                if r['type'] == 'damage':
                    self.assertEqual(r['amount'] + r['resisted'], 48 + (5 if r['id'] == 2 else 0))
                    ticks += r['id'] == 2
        self.assertTrue(ticks)

    def test_crusader_judgement_on_holy_shields_block_damage(self):
        # HandleProcTriggerDamageAuraProc -> CalculateSpellDamage: SpellDamageBonusTaken with effect 2's .05: 130 + 7.
        self.assertEqual(self.judged_bonus(20928, 2), 7)
        blocks = 0
        for seed in range(200):
            out = self.ok(self.hit(seed, caster={'auras': [20928]}, attacker={'judgements': [20303]}))
            if out['defense']['outcome'] == 'block':
                proc, = own(out)
                self.assertEqual(proc['events'][0]['amount'], 137)
                blocks += 1
        self.assertTrue(blocks)

    # --- Exorcism, Holy Wrath, Turn Undead ---------------------------------------------------------------------------

    def test_exorcism_strikes_only_undead_and_demons(self):
        for kind, ok in (('Undead', True), ('Demon', True), ('Beast', False), ('Humanoid', False), ('', False)):
            out = self.cast(10314, {**HOSTILE, 'creature_type': kind})
            self.assertEqual(out['ok'], ok, kind)
            if ok:
                self.assertEqual(out['creature_types'], ['Demon', 'Undead'])
                self.assertEqual(out['category_cooldown_ms'], 15000)
            else:
                self.assertEqual(out['error'], 'SPELL_FAILED_BAD_TARGETS')

    def test_holy_wrath_filters_each_victim(self):
        targets = [{'id': i, 'level': 60, 'alive': True, 'reaction': 'hostile', 'distance_yd': d, 'creature_type': t}
                   for i, (t, d) in enumerate((('Undead', 4), ('Demon', 19), ('Beast', 3), ('Humanoid', 5), ('Undead', 21),
                                              ('', 6)))]
        targets.append({'id': 9, 'level': 60, 'dead': True, 'distance_yd': 2, 'creature_type': 'Undead'})
        bare = self.ok(self.cast(10318))
        self.assertEqual((bare['events'], bare['cast_ms'], bare['cooldown_ms']), ([], 2000, 60000))
        self.assertEqual(bare['area'], {'radius_yd': 20.0, 'hostile': True, 'channel': False, 'creature_types': ['Demon', 'Undead']})
        self.assertNotIn('results', bare)
        out = self.ok(self.ask({'op': 'resolve', 'spell': 10318, 'seed': 4, 'caster': self.caster, 'target': {'kind': 'self'},
                                'targets': targets}))
        self.assertEqual([r['id'] for r in out['results']], [0, 1])
        self.assertEqual(out['excluded'], [2, 3, 5])
        rolls = set()
        for seed in range(200):
            out = self.ok(self.ask({'op': 'resolve', 'spell': 10318, 'seed': seed, 'caster': self.caster,
                                    'target': {'kind': 'self'}, 'targets': targets[:2]}))
            for r in out['results']:
                if r['type'] == 'damage' and not r['crit'] and not r['resisted']:
                    self.assertTrue(490 <= r['amount'] <= 576)
                    rolls.add(r['amount'])
            self.assertEqual(len(out['results']), 2)
        self.assertGreater(len(rolls), 50)
        self.assertEqual(self.cast(10318, {**HOSTILE, 'creature_type': 'Undead'})['error'], 'SPELL_FAILED_OUT_OF_RANGE')

    def test_turn_undead_is_a_single_target_fear(self):
        undead = {**HOSTILE, 'creature_type': 'Undead'}
        out = self.ok(self.cast(10326, undead))
        fear, speed = out['auras']
        self.assertEqual((fear['type'], fear['mechanic'], fear['duration_ms'], fear['damage_break']),
                         ('fear', 'turn', 20000, {'victim_level': 60}))
        self.assertEqual(fear['single_target'], {'chain': 2878, 'family': 0, 'flags': 0})
        self.assertEqual((fear['heartbeat']['chance'], fear['heartbeat']['interval_ms']), (4.0, 10000))
        self.assertEqual((speed['type'], speed['amount']), ('movement_speed', 25))
        self.assertEqual(self.cast(10326, {**HOSTILE, 'creature_type': 'Demon'})['error'], 'SPELL_FAILED_BAD_TARGETS')
        # Binary: Nature resistance (Turn Undead is Nature school in Spell.dbc) resists it outright, never partially.
        resisted = sum(self.cast(10326, {**undead, 'resist': {'3': 150}}, seed=s)['events'] == [{'type': 'resist'}]
                       for s in range(400))
        pct = F.effective_resist_percent(60, 60, 150, True, True)
        self.assertAlmostEqual(resisted / 400 * 100, 4 + pct, delta=5)

    # --- Consecration, Righteous Fury, Holy Shield -------------------------------------------------------------------

    def test_consecration_is_a_ground_area_without_a_channel(self):
        out = self.ok(self.cast(20924))
        aura = out['auras'][0]
        self.assertEqual(aura['area'], {'radius_yd': 8.0, 'channel': False, 'anchor': 'caster_position', 'hostile': True,
                                        'tick_ms': 1000, 'ticks': 8})
        self.assertEqual((aura['per_tick'], aura['duration_ms'], out['cooldown_ms']), (48, 8000, 8000))
        tick = self.ok(self.ask({'op': 'tick_area', 'spell': 20924, 'seed': 2, 'caster': self.caster,
                                 'targets': [{'id': 1, 'level': 60}, {'id': 2, 'level': 60}]}))
        self.assertTrue(all(r['amount'] + r.get('resisted', 0) == 48 for r in tick['results'] if r['type'] == 'damage'))
        # Hurricane keeps its channel (AttributesEx 0x4).
        hurricane = self.ok(self.ask({'op': 'resolve', 'spell': 17402, 'seed': 1, 'caster': {'level': 60},
                                      'target': {'kind': 'ground', 'distance_yd': 20}}))
        self.assertTrue(hurricane['auras'][0]['area']['channel'])

    def test_righteous_fury_multiplies_holy_threat(self):
        out = self.ok(self.cast(25780))
        self.assertEqual([(a['type'], a['amount'], a['school_mask'], a['multiplier']) for a in out['auras']],
                         [('threat', 60, 2, 1.6)])
        undead = {**HOSTILE, 'creature_type': 'Undead'}
        self.assertNotIn('threat_multiplier', self.ok(self.cast(10314, undead)))
        self.assertEqual(self.ok(self.cast(10314, undead, auras=[25780]))['threat_multiplier'], 1.6)
        self.assertNotIn('threat_multiplier', self.ok(self.ask({'op': 'melee', 'seed': 1, 'caster': {**self.caster, 'auras': [25780]},
                                                                'target': {**HOSTILE, 'distance_yd': 2}})))
        # Improved Righteous Fury 3/3: +50% of the 60 (ALL_EFFECTS spell mod) -> x1.9.
        talented = self.ok(self.cast(25780, talents=IMPROVED_FURY))
        self.assertEqual(talented['auras'][0]['amount'], 90)
        self.assertAlmostEqual(self.ok(self.cast(10314, undead, talents=IMPROVED_FURY, auras=[25780]))['threat_multiplier'], 1.9)

    def test_holy_shield_needs_a_shield(self):
        out = self.ok(self.cast(20928))
        self.assertEqual([(a['type'], a['amount'], a['charges']) for a in out['auras']],
                         [('block_chance', 30, 4), ('block_damage', 130, 4)])
        self.assertEqual(self.cast(20928, items=[self.sword])['error'], 'SPELL_FAILED_EQUIPPED_ITEM_CLASS')
        self.assertEqual(self.ok(self.cast(20925))['auras'][1]['amount'], 65)
        self.assertEqual(self.ok(self.cast(20928))['category_cooldown_ms'], 10000)

    # --- her defences against a hit ----------------------------------------------------------------------------------

    def test_defense_table_against_a_creature(self):
        ch = self.svc.char(self.caster)
        same = P.defense_table(ch, {'level': 60})
        self.assertEqual(same, {'miss': 5.0, 'dodge': ch.dodge(), 'parry': 5.0, 'block': 5.0, 'crit': 5.0, 'crush': 0.0})
        # Three levels up: 15 points of skill over her Defense -> 0.04 a point, crushing 2 x 15 - 15 = 15%.
        high = P.defense_table(ch, {'level': 63})
        for side, own in (('miss', 5.0), ('dodge', ch.dodge()), ('parry', 5.0), ('block', 5.0)):
            self.assertAlmostEqual(high[side], own - 0.6, msg=side)
        self.assertAlmostEqual(high['crit'], 5.6)
        self.assertEqual(high['crush'], 15.0)
        # Anticipation 5/5: +10 Defense (MOD_SKILL_TALENT 95) against a level-60 creature: 0.4 off its miss and crit.
        talented = self.svc.char({**self.caster, 'talents': {'1421': 5, '1423': 5, '1629': 5}})
        t = P.defense_table(talented, {'level': 60})
        self.assertAlmostEqual(t['miss'], 5.4)
        self.assertAlmostEqual(t['crit'], 4.6)
        # From behind, casting or stunned she cannot dodge, parry or block; without a shield or a weapon she cannot
        # block or parry; a sitting player is always crit; Holy Shield adds its 30%.
        for attacker, ctx in (({'level': 60, 'in_front': False}, {}), ({'level': 60}, {'casting': True}),
                              ({'level': 60}, {'stunned': True})):
            side = P.defense_table(ch, attacker, ctx=ctx)
            self.assertEqual((side['dodge'], side['parry'], side['block']), (0.0, 0.0, 0.0))
        bare = self.svc.char({**self.caster, 'items': []})
        self.assertEqual((P.defense_table(bare, {'level': 60})['parry'], P.defense_table(bare, {'level': 60})['block']), (0.0, 0.0))
        self.assertEqual(P.defense_table(ch, {'level': 60}, ctx={'sitting': True}), {'miss': 5.0, 'crit': 100.0})
        self.assertEqual(P.defense_table(self.svc.char({**self.caster, 'auras': [20928]}), {'level': 60})['block'], 35.0)
        self.assertEqual(P.defense_table(ch, {'level': 60}, ranged=True)['parry'], 0.0)

    def test_incoming_hit_outcomes(self):
        ch = self.svc.char(self.caster)
        seen = {}
        for seed in range(400):
            out = self.ok(self.hit(seed, attacker={'level': 63}))
            d = out['defense']
            seen.setdefault(d['outcome'], d)
        self.assertEqual(set(seen), {'miss', 'dodge', 'parry', 'block', 'crit', 'crush', 'hit'})
        self.assertEqual([seen[k]['damage'] for k in ('miss', 'dodge', 'parry', 'hit', 'crit', 'crush')],
                         [0, 0, 0, 600, 1200, 900])
        block = seen['block']
        self.assertEqual((block['block_value'], block['blocked'], block['damage'], block['full_block']),
                         (ch.block_value(), ch.block_value(), 600 - ch.block_value(), False))
        full = next(d['defense'] for s in range(400) for d in [self.ok(self.hit(s, damage=20))]
                    if d['defense']['outcome'] == 'block')
        self.assertEqual((full['blocked'], full['damage'], full['full_block']), (20, 0, True))
        # Ranged: no parry, a dodge is a miss.
        outcomes = {self.ok(self.hit(s, kind='ranged'))['defense']['outcome'] for s in range(300)}
        self.assertFalse(outcomes & {'parry', 'dodge'})
        # Elemental melee is not blockable (pre-WotLK).
        self.assertEqual(self.ok(self.hit(1, school=2))['defense']['chances']['block'], 0.0)
        for bad in ({'kind': 'x'}, {'school': 9}, {'damage': -1}):
            self.assertEqual(self.ask({'op': 'incoming_hit', 'weapon_hit': True, 'caster': self.caster, 'target': HOSTILE,
                                       'attack': {'damage': 5, **bad}})['error'].split(':')[0], 'BAD_REQUEST')

    def test_immunity_and_native_conversion(self):
        out = self.ok(self.hit(1, caster={'auras': [1020]}))
        self.assertEqual(out['defense'], {'kind': 'melee', 'school': 0, 'outcome': 'immune', 'damage': 0, 'multiplier': 0})
        # Blessing of Protection: physical only - a melee blow is nothing, a fire spell lands.
        self.assertEqual(self.ok(self.hit(1, caster={'auras': [10278]}))['defense']['outcome'], 'immune')
        self.assertEqual(self.ok(self.hit(1, caster={'auras': [10278]}, school=2, kind='spell'))['defense']['outcome'], 'hit')
        self.assertEqual(self.ok(self.hit(1, caster={'auras': [5573]}, school=3, kind='spell'))['defense']['outcome'], 'immune')
        ch = self.svc.char(self.caster)
        block = next(o for s in range(400) for o in [self.ok(self.hit(s, damage=30, caster={'skyrim': NATIVE}))]
                     if o['defense']['outcome'] == 'block')['defense']
        native = ch.block_value() * COUNTERPARTS['iron_sword']['magnitude'] / 96.5
        self.assertAlmostEqual(block['blocked'], round(native, 2))
        self.assertAlmostEqual(block['damage'], round(30 - native, 2))
        self.assertEqual(P.block_health_factor(), FERAL_MELEE.health_factor)

    def test_holy_shield_redoubt_and_reckoning_procs(self):
        caster = {'auras': [20928, 20137, 20182, 25780]}
        blocks, crits = [], []
        for seed in range(300):
            out = self.ok(self.hit(seed, caster=caster))
            if out['defense']['outcome'] == 'block':
                blocks.append(out)
            elif out['defense']['outcome'] == 'crit':
                crits.append(out)
            else:
                self.assertEqual(own(out), [])
        self.assertTrue(blocks and crits)
        for out in blocks:
            proc, = own(out)
            self.assertEqual((proc['spell'], proc['target'], proc['events'][0]['amount'], proc['events'][0]['school'],
                              proc['threat_multiplier']), (20928, 'target', 130, 1, 1.6))
            self.assertEqual(out['charges_used'], {'20928': 1})
        for out in crits:
            redoubt, reckoning = own(out)
            self.assertEqual((redoubt['spell'], redoubt['target'], redoubt['auras'][0]['amount'],
                              redoubt['auras'][0]['charges'], redoubt['auras'][0]['duration_ms']), (20134, 'caster', 30, 5, 10000))
            self.assertEqual(reckoning['events'], [{'type': 'extra_attacks', 'count': 1, 'max_pending': 5}])
        # Redoubt's buff spends a charge on a block too; a spell crit raises both talents; sitting raises nothing.
        buffed = next(o for s in range(300) for o in [self.ok(self.hit(s, caster={'auras': [20134]}))]
                      if o['defense']['outcome'] == 'block')
        self.assertEqual(buffed['charges_used'], {'20134': 1})
        spell = self.ok(self.hit(1, caster=caster, kind='spell', school=2, crit=True))
        self.assertEqual([p['spell'] for p in own(spell)], [20134, 20178])
        sitting = self.ask({'op': 'incoming_hit', 'weapon_hit': True, 'seed': 1, 'caster': {**self.caster, **caster},
                            'target': HOSTILE, 'attack': {'damage': 100}, 'ctx': {'sitting': True}})
        self.assertEqual((sitting['defense']['outcome'], sitting.get('procs')), ('crit', None))
        # Spell power reaches the block's damage with Holy Shield's 0.05 coefficient (SpellDamageBonusDone at the proc).
        self.assertEqual(self.cat.spell(20928)['EffectBonusCoefficient2'], 0.05)
        # Native: the proc converts with Holy Shield's rule; the outcome keeps the native hit.
        native = next(o for s in range(300) for o in [self.ok(self.hit(s, damage=30, caster={**caster, 'skyrim': NATIVE}))]
                      if o['defense']['outcome'] == 'block')
        self.assertEqual(own(native)[0]['events'][0]['amount'], round(130 * self.cat.balance[20928].health_factor))
        self.assertEqual(own(native)[0]['skill'], 'Restoration')

    def test_reckoning_chance_by_rank(self):
        for sid, chance in zip(P.RECKONING, (20, 40, 60, 80, 100)):
            self.assertEqual(self.cat.spell(sid)['ProcChance'], chance)
        procs = 0
        for seed in range(1000):
            out = self.ok(self.ask({'op': 'incoming_hit', 'weapon_hit': False, 'seed': seed,
                                    'caster': {**self.caster, 'auras': [20177]}, 'target': HOSTILE,
                                    'attack': {'damage': 100, 'kind': 'spell', 'crit': True}}))
            procs += [p['spell'] for p in own(out)] == [P.RECKONING_ATTACK]
        self.assertAlmostEqual(procs / 10, 20, delta=4)

    def test_incoming_spell_crit_is_one_and_a_half(self):
        # Unit::GetCritMultiplier: 1.5 for a spell (damage class none or magic), 2 for an attack.
        out = self.ok(self.ask({'op': 'incoming_hit', 'weapon_hit': False, 'seed': 1, 'caster': self.caster,
                                'target': HOSTILE, 'attack': {'damage': 101, 'kind': 'spell', 'school': 2, 'crit': True}}))
        self.assertEqual((out['defense']['outcome'], out['defense']['multiplier'], out['defense']['damage']), ('crit', 1.5, 151))

    def test_talent_redoubt_procs_with_a_shield_talent(self):
        caster = {'talents': {'1421': 5}}
        crit = next(o for s in range(300) for o in [self.ok(self.hit(s, caster=caster))] if o['defense']['outcome'] == 'crit')
        self.assertEqual([p['spell'] for p in own(crit)], [20134])

    def test_landed_procs_need_a_landed_hit(self):
        # Gear procs (procEx none) want a hit or crit (SpellMgr::IsSpellProcEventCanTriggeredBy); a missed, dodged,
        # parried or fully blocked blow raises none. The fixture's shield procs when struck (Skullflame Shield).
        self.assertTrue(any(self.cat.item(self.shield)[f'spelltrigger_{n}'] == 1 for n in range(1, 6)))
        seen = {}
        for seed in range(600):
            out = self.ok(self.hit(seed, damage=20))
            d = out['defense']
            gear = [p for p in out.get('procs', []) if p not in own(out)]
            if d['outcome'] in ('miss', 'dodge', 'parry') or d.get('full_block'):
                self.assertEqual(gear, [])
            seen[d['outcome']] = seen.get(d['outcome'], False) or bool(gear)
        self.assertLessEqual({'miss', 'dodge', 'parry', 'block'}, set(seen))
        self.assertTrue(seen['hit'])

    # --- the Druid never reaches these paths -----------------------------------------------------------------------

    def test_druid_never_reaches_the_paladin_paths(self):
        druid = {'level': 60, 'auras': [9885]}
        for op in ({'op': 'incoming_hit', 'weapon_hit': True, 'seed': 4, 'caster': druid, 'target': HOSTILE},
                   {'op': 'melee', 'seed': 4, 'caster': druid, 'target': {**HOSTILE, 'distance_yd': 2}}):
            self.assertNotIn('defense', self.ok(self.ask(op)))
        wrath = self.ok(self.ask({'op': 'resolve', 'spell': 9912, 'seed': 1, 'caster': druid, 'target': HOSTILE}))
        self.assertFalse({'threat_multiplier', 'cooldown_category', 'creature_types'} & set(wrath))
        # A Paladin's judgements listed on the unit change nothing of hers (the taken bonus and the procs are a Paladin's).
        for seed in range(6):
            for sid in (9912, 25298):
                req = {'op': 'resolve', 'spell': sid, 'seed': seed, 'caster': druid, 'target': HOSTILE}
                judged = {**req, 'target': {**HOSTILE, 'judgements': [20303, 20355]}}
                self.assertEqual(self.ok(self.ask(judged)), self.ok(self.ask(req)))
        hurricane = {'op': 'tick_area', 'spell': 17402, 'seed': 3, 'caster': druid, 'targets': [{'id': 1, 'level': 60}]}
        self.assertEqual(self.ok(self.ask({**hurricane, 'targets': [{'id': 1, 'level': 60, 'judgements': [20303]}]})),
                         self.ok(self.ask(hurricane)))
        self.assertFalse(any(P.handles(sid) for sid in self.cat.class_spells if self.cat.spell(sid)['SpellFamilyName'] == 7))

    # --- balance --------------------------------------------------------------------------------------------------

    def test_balance_anchors_are_catalog_maxima_and_trainable(self):
        trainable = {'Destruction', 'Restoration', 'Alteration', 'Marksman', 'OneHanded', 'TwoHanded'}   # Character.cpp kSkills
        ch = character.build(self.cat, [], cls=2, race=1)
        for sid, rule in PALADIN_DEFENSE.items():
            s = self.cat.spell(sid)
            with self.subTest(spell=rule.name):
                self.assertEqual(s['SpellName'], rule.name)
                self.assertIn(rule.skill, trainable)
                self.assertEqual(spells._cost(s, ch), rule.wow_cost)
                self.assertEqual(max((r['rank'], r) for r in [self.cat.ranks[x] | {'id': x} for x in self.cat.ranks
                                                              if self.cat.chain(x) == self.cat.chain(sid)])[1]['id'], sid)
                budget = 0
                for e in spells._effects(s):
                    effect, aura = s[f'Effect{e}'], s[f'EffectApplyAuraName{e}']
                    value = int(F.effect_value(s, e, 60, None, maximum=True))
                    if effect == 2 or aura == 43:
                        budget += value
                    elif aura == 3:
                        budget += value * (self.cat.duration(s, 60) // s[f'EffectAmplitude{e}'])
                self.assertEqual(rule.wow_health if rule.health else 0, budget)
                for rank in (x for x in self.cat.ranks if self.cat.chain(x) == self.cat.chain(sid)):
                    self.assertEqual(self.cat.balance[rank].skill, rule.skill)

    def test_native_costs_and_health(self):
        costs = self.ok(self.ask({'op': 'skyrim_costs', 'caster': {**self.caster, 'skyrim': NATIVE}}))['costs']
        factor = skyrim.cost_factor(50)
        expected = {1020: 251 * 12 / 30, 5573: 251 * 8 / 30, 10278: 251 * 10 / 30, 10308: 450 * .6, 10314: 72,
                    24239: 72 * 425 / 345, 10318: 144, 20924: 144 * 565 / 805, 10326: 168 * 20 / 30, 25780: 39,
                    20928: 72 * 240 / 345}
        for sid, base in expected.items():
            self.assertEqual(costs[str(sid)], max(1, round(base * factor)), sid)
        for sid, health in ((10314, 75 / 563), (24239, 75 / 556), (10318, 60 / 576), (20924, 40 / 384), (20928, 50 / 3 / 130)):
            self.assertAlmostEqual(self.cat.balance[sid].health_factor, health, msg=sid)
        out = self.ok(self.cast(10314, {**HOSTILE, 'creature_type': 'Undead'}, skyrim=NATIVE))
        wow = self.ok(self.cast(10314, {**HOSTILE, 'creature_type': 'Undead'}))
        self.assertEqual(out['events'][0]['amount'], round(wow['events'][0]['amount'] * 75 / 563))
        self.assertEqual(out['skill'], 'Restoration')
        # Lower ranks keep a native floor (Exorcism I at its level is no 1-damage spark).
        low = self.ok(self.ask({'op': 'resolve', 'spell': 879, 'seed': 1, 'target': {**HOSTILE, 'level': 20, 'creature_type': 'Undead'},
                                'caster': {**self.caster, 'level': 20, 'skyrim': NATIVE}}))
        self.assertGreaterEqual(low['events'][0]['amount'], 15)


if __name__ == '__main__':
    unittest.main()
