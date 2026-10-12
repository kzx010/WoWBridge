"""Feral catalog rows and CMaNGOS rules, using reproducible combat seeds.

Authority: StatSystem.cpp:CalculateMinMaxDamage/UpdateAttackPowerAndDamage;
SpellAuras.cpp:HandleAuraModShapeshift/HandlePeriodicDamage/PeriodicTick;
SpellEffects.cpp:EffectWeaponDmg/EffectSchoolDMG/EffectAddComboPoints;
Unit.cpp:MeleeSpellHitResult/CalculateSpellDamage; Player.cpp:RewardRage.
"""
import copy
import json
import random
import unittest

from service import character, feral, formulas as F, melee, skyrim
from service.skyrim_balance import SPELLS
from service.catalog import Catalog
from service.server import Service
from service.tests.test_service import FULL

BEAR = {9881: 15, 9908: 20, 6795: 0, 9898: 10, 8983: 10, 5229: 0, 22896: 0}
CAT = {9850: 45, 9830: 60, 9904: 40, 9896: 30, 31018: 35,
       9913: 0, 9827: 50, 9867: 60, 9846: 30, 9821: 0, 9892: 20}
SELF = {5229, 22896, 9913, 9846, 9821, 9898} | feral.FORM_SPELLS.keys()
TARGET = {'kind': 'unit', 'level': 60, 'distance_yd': 2, 'reaction': 'hostile', 'id': 7}


class Feral(unittest.TestCase):
    def setUp(self):
        self.cat = Catalog()
        self.svc = Service(self.cat, None)

    def req(self, sid, form=None, seed=1):
        form = form or ('bear' if sid in BEAR else 'cat' if sid in CAT or sid == 17392 else 'none')
        target = {'kind': 'self'} if sid in SELF else dict(TARGET)
        if sid in (9830, 9867):
            target['behind'] = True
        return {'op': 'resolve', 'spell': sid, 'seed': seed, 'target': target,
                'caster': {'form': form, 'power': {'rage': 100, 'energy': 100},
                           'combo_points': 2, 'prowling': sid in (9827, 9867)}}

    def answer(self, req):
        return self.svc.respond(json.dumps(req).encode())

    def good(self, req):
        result = self.answer(req)
        self.assertTrue(result['ok'], result)
        return result

    def test_every_form_level60_profile_and_mana_cost(self):
        # naked level60: Str62, Agi65 => humanoid AP284. Form linked
        # rows add Bear120 (capped level40), Dire180, Cat120 + Agi65.
        profiles = {'bear': (2281, 'rage', 2500, 127.5, 187.5, 404, 180, 560, 0),
                    'dire_bear': (2281, 'rage', 2500, 127.5, 187.5, 464, 360, 1240, 0),
                    'cat': (892, 'energy', 1000, 51, 75, 469, 0, 0, 0),
                    'travel': (632, 'mana', 2000, 1, 2, 284, 0, 0, 40),
                    'aquatic': (2428, 'mana', 2000, 1, 2, 284, 0, 0, 0)}
        for sid, name in feral.FORM_SPELLS.items():
            with self.subTest(form=name):
                out = self.good(self.req(sid))
                a = out['auras'][0]
                display, power, speed, low, high, ap, armor, hp, run = profiles[name]
                self.assertEqual((a['form'], a['type'], a['aura'], a['duration_ms']), (name, 'shapeshift', 36, -1))
                self.assertEqual((a['display_id'], a['power'], a['attack_speed_ms']), (display, power, speed))
                self.assertEqual(a['weapon_damage'], {'min': low, 'max': high})
                self.assertAlmostEqual(a['damage']['min'], low + ap / 14 * speed / 1000)
                self.assertAlmostEqual(a['damage']['max'], high + ap / 14 * speed / 1000)
                self.assertEqual((a['attack_power'], a['armor_percent'], a['health_bonus'],
                                  a['movement_speed_percent']), (ap, armor, hp, run))
                self.assertEqual(a['stats'], {})
                self.assertEqual((a['swim_speed_percent'], a['water_breathing']),
                                 (50, True) if name == 'aquatic' else (0, False))
                self.assertEqual(out['cost'], 684 if name in ('cat', 'bear', 'dire_bear') else 248)
                self.assertEqual(a.get('power_set'), {power: 0} if power != 'mana' else None)
                self.assertEqual(a['interrupt_flags'], self.cat.spell(sid)['AuraInterruptFlags'])

    def test_shifting_between_forms_and_leaving(self):
        for sid, name in feral.FORM_SPELLS.items():
            for old in feral.FORMS:
                req = self.req(sid, old)
                out = self.good(req)
                a = out['auras'][0]
                self.assertEqual(a['previous_form'], old)
                self.assertEqual(a['form'], 'none' if old == name else name)
                self.assertEqual(a['action'], 'leave' if old == name else 'enter')
                self.assertEqual((out['cost'], out['gcd_ms']), (0, 0) if old == name else
                                 (684 if name in ('cat', 'bear', 'dire_bear') else 248, 1500))
                self.assertTrue(a['clears_prowl'])
        req = self.req(783)
        req['ctx'] = {'indoors': True}
        self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_ONLY_OUTDOORS')
        req = self.req(1066)
        req['caster']['swimming'] = False
        self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_ONLY_UNDERWATER')
        req = self.req(1066, 'aquatic')   # out of the water, she can still leave it
        req['caster']['swimming'] = False
        self.assertEqual(self.good(req)['auras'][0]['action'], 'leave')
        for form in ('bear', 'dire_bear', 'cat', 'travel', 'aquatic'):
            for sid in (9912, 25298, 25297, 29166, 22812, 2782):
                req = self.req(sid, form)
                self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_NOT_SHAPESHIFT')

    def test_every_ability_forms_costs_and_metadata(self):
        for sid in feral.ABILITIES:
            permitted = ('bear', 'dire_bear') if sid in BEAR else ('cat', 'bear', 'dire_bear') if sid == 17392 else ('cat',)
            for form in feral.FORMS:
                with self.subTest(spell=sid, form=form):
                    req = self.req(sid, form)
                    out = self.answer(req)
                    if form not in permitted:
                        self.assertEqual(out['error'], 'SPELL_FAILED_ONLY_SHAPESHIFT')
                        continue
                    self.assertTrue(out['ok'], out)
                    self.assertEqual(out['cost'], (BEAR if sid in BEAR else CAT).get(sid, 0))
                    self.assertEqual(out['power'], 'rage' if sid in BEAR else 'energy')
                    row = self.cat.spell(sid)
                    self.assertEqual(out['gcd_ms'], row['StartRecoveryTime'])
                    self.assertEqual(out['cooldown_ms'], max(row['RecoveryTime'], row['CategoryRecoveryTime']))
                    self.assertEqual(out, self.answer(req))

    def test_power_prowl_behind_combo_and_target_validation(self):
        for sid, cost in {**BEAR, **CAT}.items():
            if not cost:
                continue
            req = self.req(sid)
            kind = 'rage' if sid in BEAR else 'energy'
            req['caster']['power'][kind] = cost - 1
            self.assertEqual(self.answer(req)['error'], f'SPELL_FAILED_NO_POWER: ERR_OUT_OF_{kind.upper()}')
            req['caster']['power'][kind] = cost
            self.good(req)
        for sid in (9827, 9867):
            req = self.req(sid)
            req['caster']['prowling'] = False
            self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_ONLY_STEALTHED')
        for sid in (9830, 9867):
            req = self.req(sid)
            req['target']['behind'] = False
            self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_NOT_BEHIND')
        self.good(self.req(9827))  # Pounce's explicit CheckCast exception: no behind requirement.
        for sid in (9896, 31018):
            req = self.req(sid)
            req['caster']['combo_points'] = 0
            self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_NO_COMBO_POINTS')
        req = self.req(9913)
        req['caster']['in_combat'] = True
        self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_AFFECTING_COMBAT')
        for sid in feral.ABILITIES:
            req = self.req(sid)
            req['target']['kind'] = 'unit' if sid in SELF else 'self'
            self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_BAD_TARGETS')
        for update, code in [({'dead': True}, 'TARGETS_DEAD'), ({'friendly': True}, 'BAD_TARGETS'),
                             ({'distance_yd': 8}, 'OUT_OF_RANGE'), ({'los': False}, 'LINE_OF_SIGHT')]:
            req = self.req(9850)
            req['target'].update(update)
            self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_' + code)

    def test_finishers_report_client_combo_error_before_empty_energy(self):
        for sid in (9896, 31018):
            req = self.req(sid)
            req['caster'].update(combo_points=0, power={'energy': 0})
            self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_NO_COMBO_POINTS')

    def test_weapon_damage_abilities_and_combo_gains(self):
        # EffectWeaponDmg multiplies BOTH the preceding flat bonus and the
        # form weapon/AP roll: Claw +115; Shred (weapon+80)*2.25;
        # Ravage (weapon+98)*3.5. Seed1's hit die succeeds and weapon roll=102.
        for sid, flat, pct in ((9850, 115, 1), (9830, 80, 2.25), (9867, 98, 3.5)):
            req = self.req(sid)
            req['target']['behind'] = True
            out = self.good(req)
            rng = random.Random(1)
            rng.randint(1, 10000)
            roll = rng.randint(84, 108)
            raw = int(flat * pct) + int(roll * pct)
            amount = int(raw * (1 - min(self.cat.creature_armor(60) /
                                       (self.cat.creature_armor(60) + 5500), .75)))
            self.assertEqual(out['events'][0]['amount'], amount)
            self.assertFalse(out['events'][0]['crit'])
            self.assertEqual(out['combo_points'], 1)
            req['caster']['combo_points'] = 5
            self.assertEqual(self.good(req)['combo_points'], 0)

    def test_yellow_miss_dodge_parry_crit_block_and_refund(self):
        for seed, kind in ((31, 'miss'), (2, 'dodge'), (32, 'parry')):
            out = self.good(self.req(9850, seed=seed))
            self.assertEqual(out['events'], [{'type': kind}])
            self.assertEqual(out['power_refund'], 36)
            self.assertNotIn('combo_points', out)
        out = self.good(self.req(9850, seed=7))
        self.assertEqual(out['events'][0]['outcome'], 'block')
        self.assertEqual(out['events'][0]['blocked'], 36)
        out = self.good(self.req(9904, seed=15))
        self.assertEqual((out['events'][0]['crit'], out['events'][0]['amount']), (True, 73))
        blocked = self.good(self.req(9904, seed=14))
        self.assertEqual(blocked['events'], [{'type': 'block'}])
        self.assertEqual(blocked['auras'], [])
        # Ravage is NO_ACTIVE_DEFENSE: the usual dodge seed still hits.
        self.assertEqual(self.good(self.req(9867, seed=2))['events'][0]['type'], 'damage')

    def test_rake_rip_pounce_bleeds_and_stuns(self):
        for sid, tick, duration, amp in ((9904, 41, 9000, 3000), (9827, 39, 18000, 3000)):
            out = self.good(self.req(sid))
            dot = next(a for a in out['auras'] if a['type'] == 'damage')
            self.assertEqual((dot['per_tick'], dot['duration_ms'], dot['amplitude_ms']), (tick, duration, amp))
            self.assertEqual(dot['ticks'], [{'t_ms': t, 'amount': tick, 'resisted': 0}
                                           for t in range(amp, duration + 1, amp)])
            self.assertEqual(out['combo_points'], 1)
        for sid, duration in ((8983, 4000), (9827, 2000)):
            stun = next(a for a in self.good(self.req(sid))['auras'] if a['type'] == 'stun')
            self.assertEqual(stun['duration_ms'], duration)
            self.assertFalse(stun['breaks_on_damage'])
        for cp in range(1, 6):
            req = self.req(9896)
            req['caster']['combo_points'] = cp
            out = self.good(req)
            # Object.cpp:CalculateSpellEffectValue + HandlePeriodicDamage.
            expected = 17 + 28 * cp + int(469 * min(cp, 4) / 100)
            self.assertEqual(out['auras'][0]['per_tick'], expected)
            self.assertEqual(len(out['auras'][0]['ticks']), 6)
            self.assertEqual(out['combo_points_spent'], cp)
            req['seed'] = 2
            missed = self.good(req)
            self.assertNotIn('combo_points_spent', missed)
            self.assertNotIn('power_refund', missed)

    def test_bite_combo_and_extra_energy_scaling(self):
        for cp in range(1, 6):
            for energy in (35, 70, 100):
                req = self.req(31018)
                req['caster'].update(combo_points=cp, power={'energy': energy})
                req['target']['behind'] = True
                out = self.good(req)
                rng = random.Random(1)
                rng.randint(1, 10000)
                base = 51 + rng.randint(1, 61) + 147 * cp
                base += int((energy - 35) * 2.7) + int(469 * .03 * cp)
                expected = int(F.armor_reduced(base, self.cat.creature_armor(60), 60))
                self.assertEqual(out['events'][0]['amount'], expected)
                self.assertEqual((out['cost'], out['extra_power_cost'], out['combo_points_spent']),
                                 (35, energy - 35, cp))
        req['seed'] = 2
        out = self.good(req)
        self.assertNotIn('extra_power_cost', out)
        self.assertNotIn('combo_points_spent', out)

    def test_maul_queue_swing_and_insufficient_power_fallback(self):
        queued = self.good(self.req(9881))
        self.assertTrue(queued['cost_deferred'])
        self.assertEqual(queued['events'], [{'type': 'next_swing', 'spell': 9881, 'bonus_damage': 128}])
        for form, ap in (('bear', 404), ('dire_bear', 464)):
            req = self.req(9881, form)
            req.update(op='melee')
            req['caster']['next_swing'] = 9881
            req['target'].update(behind=True)
            out = self.good(req)
            rng = random.Random(1)
            rng.randint(1, 10000)
            low, high = int(127.5 + ap / 14 * 2.5), int(187.5 + ap / 14 * 2.5)
            expected = int(F.armor_reduced(rng.randint(low, high) + 128, self.cat.creature_armor(60), 60))
            self.assertEqual((out['damage'], out['cost'], out['delay_ms'], out['rage_gained']), (expected, 15, 2500, 0))
            self.assertTrue(out['next_swing_consumed'])
            req['caster']['power']['rage'] = 0
            fallback = self.good(req)
            plain = copy.deepcopy(req)
            plain['caster'].pop('next_swing')
            ordinary = self.good(plain)
            self.assertEqual(fallback['damage'], ordinary['damage'])
            self.assertIn('ERR_OUT_OF_RAGE', fallback['next_swing_error'])

    def test_swipe_three_independent_targets(self):
        req = self.req(9908)
        req['targets'] = [{**TARGET, 'id': i} for i in range(4)]
        out = self.good(req)
        self.assertEqual(out['cost'], 20)
        self.assertEqual([r['id'] for r in out['results']], [0, 1, 2])
        first = out['results'][0]
        self.assertEqual(first['type'], 'damage')
        self.assertEqual(first['amount'], int(F.armor_reduced(83 + int(404 * .08), self.cat.creature_armor(60), 60)))
        req['targets'] *= 17
        self.assertIn('too many targets', self.answer(req)['error'])

    def test_utility_auras_and_events(self):
        growl = self.good(self.req(6795))
        self.assertEqual(growl['events'], [{'type': 'taunt', 'match_top_threat': True}])
        self.assertEqual((growl['auras'][0]['type'], growl['auras'][0]['duration_ms']), ('taunt', 3000))
        roar = self.good(self.req(9898))['auras'][0]
        self.assertEqual((roar['type'], roar['amount'], roar['duration_ms']), ('attack_power', -138, 30000))
        self.assertEqual(roar['area'], {'radius_yd': 10, 'hostile': True, 'channel': False})
        enrage = self.good(self.req(5229))['auras']
        self.assertEqual((enrage[0]['type'], enrage[0]['per_tick'], enrage[0]['amplitude_ms']), ('periodic_power', 2, 1000))
        self.assertEqual((enrage[1]['type'], enrage[1]['amount']), ('armor_percent', -26))
        self.assertEqual(enrage[2]['type'], 'interrupt_regen')
        fr = self.good(self.req(22896))['auras'][0]
        self.assertEqual((fr['type'], fr['max_rage_per_tick'], fr['health_per_rage']), ('rage_heal', 10, 20))
        self.assertEqual(fr['spell'], 22845)
        self.assertEqual([t['t_ms'] for t in fr['ticks']], list(range(1000, 10001, 1000)))
        self.assertEqual(fr['crit_multiplier'], 1.5)
        for sid, kind, value, duration in ((9846, 'damage_done', 40, 6000), (9821, 'movement_speed', 60, 15000)):
            a = self.good(self.req(sid))['auras'][0]
            self.assertEqual((a['type'], a['amount'], a['duration_ms']), (kind, value, duration))
        prowl = self.good(self.req(9913))['auras']
        self.assertEqual([(a['type'], a['amount']) for a in prowl], [('stealth', 300), ('movement_speed', -30)])
        self.assertTrue(prowl[0]['prowling'])
        self.assertEqual(self.good(self.req(9892))['events'], [{'type': 'threat', 'amount': -608}])
        ff = self.good(self.req(17392))['auras']
        self.assertEqual([a['type'] for a in ff], ['armor', 'dispel_immunity', 'dispel_immunity'])
        self.assertEqual(ff[0]['amount'], -505)

    def test_form_melee_staff_independence_stats_and_rage(self):
        for form in ('cat', 'bear', 'dire_bear', 'travel', 'aquatic'):
            req = {'op': 'melee', 'target': TARGET, 'seed': 1, 'caster': {'form': form}}
            out = self.good(req)
            ch = self.svc.char(req['caster'])
            rng = random.Random(1)
            table = melee.chances(ch, TARGET)
            side = melee._die(rng, list(table.items()))
            low, high = ch.weapon_damage_range()
            expected = int(F.armor_reduced(rng.randint(int(low), int(high)), self.cat.creature_armor(60), 60))
            if side == 'glance':
                expected = int(expected * rng.randint(91, 99) * .01)
            self.assertEqual(out['damage'], expected)
            self.assertEqual(out['delay_ms'], 1000 if form == 'cat' else 2500 if form in ('bear', 'dire_bear') else 2000)
            if form in ('bear', 'dire_bear'):
                conversion = .0091107836 * 60**2 + 3.225598133 * 60 + 4.2652911
                self.assertEqual(out['rage_gained'], int(expected / conversion * 75) / 10)
            naked = self.svc.char({'form': form})
            staff = self.svc.char({'form': form, 'items': [873]})
            if form in ('bear', 'dire_bear', 'cat'):
                self.assertEqual(naked.melee_weapon(), staff.melee_weapon())
        # Damage taken op stays in WoW units even when Skyrim is supplied.
        req = {'op': 'rage_from_damage_taken', 'damage': 1000, 'victim_level': 60}
        self.assertEqual(self.good(req)['rage'], 10.8)
        req['caster'] = {'skyrim': {}}
        self.assertEqual(self.good(req)['rage'], 10.8)
        prowling = {'op': 'melee', 'target': TARGET, 'seed': 31,
                    'caster': {'form': 'cat', 'prowling': True}}
        self.assertTrue(self.good(prowling)['breaks_prowl'])
        constants = self.good({'op': 'stats'})['resources']
        self.assertEqual((constants['energy_per_tick'], constants['energy_tick_ms']), (20, 2000))
        self.assertEqual((constants['rage_decay_per_tick'], constants['rage_decay_tick_ms']), (2.5, 2000))

    def test_level_scaling_equipment_and_form_cache_isolation(self):
        for level, low in ((20, 17), (60, 51), (70, 51)):
            ch = self.svc.char({'form': 'cat', 'level': level})
            self.assertEqual(ch.melee_weapon()['min'], low)
        plain = self.svc.char({'items': FULL})
        cat = self.svc.char({'items': FULL, 'form': 'cat'})
        self.assertEqual(cat.attack_power() - plain.attack_power(), plain.stats['agi'] + 120)
        self.assertEqual(cat.stats, plain.stats)
        self.assertIs(self.svc.char({'items': FULL}), plain)
        self.assertIs(self.svc.char({'items': FULL, 'form': 'cat', 'power': {'energy': 0}}), cat)
        fury = self.svc.char({'form': 'cat', 'auras': [9846]})
        self.assertEqual(fury.weapon_damage_range()[0] - self.svc.char({'form': 'cat'}).weapon_damage_range()[0], 40)

    def test_skyrim_all_forms_and_abilities_resources_unchanged(self):
        for sid in feral.ABILITIES | feral.FORM_SPELLS.keys():
            req = self.req(sid)
            req['caster']['items'] = FULL
            before = copy.deepcopy(req)
            wow = self.good(req)
            self.assertEqual(req, before)
            req['caster']['skyrim'] = {'one_handed': 80, 'alteration': 80, 'two_handed': 1}
            scaled = self.good(req)
            rule = SPELLS[sid]
            factor = rule.health_factor
            self.assertEqual(scaled['skill'], 'Alteration' if sid in feral.FORM_SPELLS else 'OneHanded')
            self.assertEqual(scaled['cost'], max(1, round(rule.base_cost * skyrim.cost_factor(80))) if sid in feral.FORM_SPELLS and wow['cost'] else wow['cost'])
            for a, b in zip(wow['events'] + wow.get('results', []), scaled['events'] + scaled.get('results', [])):
                if a['type'] == 'damage':
                    self.assertEqual(b['amount'], round(a['amount'] * factor))
                    self.assertEqual(b['blocked'], round(a['blocked'] * factor))
                elif a['type'] == 'next_swing':
                    self.assertEqual(b['bonus_damage'], round(a['bonus_damage'] * factor))
            for a, b in zip(wow['auras'], scaled['auras']):
                if a['type'] == 'damage':
                    self.assertEqual(b['per_tick'], round(a['per_tick'] * factor))
                    self.assertEqual(b['ticks'][0]['amount'], round(a['ticks'][0]['amount'] * factor))
                elif a['type'] == 'rage_heal':
                    self.assertAlmostEqual(b['health_per_rage'], 1)
                elif a['type'] == 'shapeshift':
                    self.assertEqual(b['skyrim_health_bonus'], round(a['health_bonus'] * factor))
                elif a['type'] == 'damage_done':
                    self.assertEqual(b['amount'], round(a['amount'] * factor))
                elif 'amount' in a:
                    self.assertEqual(a['amount'], b['amount'])
            for field in ('extra_power_cost', 'power_refund', 'combo_points', 'combo_points_spent'):
                self.assertEqual(wow.get(field), scaled.get(field))
        req = {'op': 'melee', 'target': TARGET, 'seed': 1, 'caster': {'form': 'bear', 'items': FULL}}
        wow = self.good(req)
        req['caster']['skyrim'] = {'one_handed': 80, 'two_handed': 1}
        scaled = self.good(req)
        self.assertEqual((scaled['damage'], scaled['rage_gained'], scaled['skill']),
                         (round(wow['damage'] * 7 / 96.5), wow['rage_gained'], 'OneHanded'))

    def test_malformed_state_and_request_independence(self):
        for fields in ({'form': 'moonkin'}, {'power': {'rage': -1}}, {'power': {'energy': 101}},
                       {'power': {'energy': 1.5}}, {'combo_points': 6}, {'combo_points': True},
                       {'prowling': 1}, {'in_combat': 'no'}):
            req = self.req(9850)
            req['caster'].update(fields)
            self.assertTrue(self.answer(req)['error'].startswith('BAD_REQUEST:'))
        req = self.req(9850)
        self.good(req)
        req['caster']['power'] = {}
        self.assertIn('ERR_OUT_OF_ENERGY', self.answer(req)['error'])


if __name__ == '__main__':
    unittest.main()
