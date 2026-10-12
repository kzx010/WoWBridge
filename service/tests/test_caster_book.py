"""Caster book expectations from realm rows and cited CMaNGOS formulas."""
import copy
import json
import random
import unittest

from service import character, formulas as F, skyrim
from service.catalog import Catalog
from service.server import Service
from service.skyrim_effects import RESISTS, ARMOR_BUFF, ARMOR_DEBUFF
from service.skyrim_balance import SPELLS
from service.tests.test_service import FULL, HOSTILE

BOOK = {'starfire': 25298, 'regrowth': 9858, 'faerie_fire': 9907, 'hibernate': 18658,
        'barkskin': 22812, 'innervate': 29166, 'mark_of_the_wild': 9885,
        'gift_of_the_wild': 21850, 'thorns': 9910, 'tranquility': 9863,
        'rebirth': 20748, 'remove_curse': 2782, 'abolish_poison': 2893,
        'cure_poison': 8946, 'soothe_animal': 9901}
HARMFUL = {'starfire', 'faerie_fire', 'hibernate', 'soothe_animal'}


class CasterBook(unittest.TestCase):
    def setUp(self):
        self.cat = Catalog()
        self.svc = Service(self.cat, None)

    def request(self, name, items=FULL, seed=1):
        target = ({**HOSTILE, 'creature_type': 'Beast', 'friendly': False} if name in HARMFUL
                  else {**HOSTILE, 'friendly': True})
        if name in ('barkskin', 'tranquility'):
            target = {'kind': 'self'}
        if name == 'rebirth':
            target['dead'] = True
        return {'op': 'resolve', 'spell': BOOK[name], 'seed': seed, 'target': target,
                'caster': {'items': items, 'reagents': {'17026': 1, '17038': 1}}}

    def answer(self, req):
        return self.svc.respond(json.dumps(req).encode())

    def test_every_spell_timing_cost_duration_and_reagents(self):
        for name, sid in BOOK.items():
            for items in ([], FULL):
                with self.subTest(name=name, items=items):
                    req = self.request(name, items)
                    out = self.answer(req)
                    self.assertTrue(out['ok'], out)
                    s, ch = self.cat.spell(sid), character.build(self.cat, items)
                    # Spell::CalculatePowerCost: truncate percentage before spell mods.
                    cost = s['ManaCost'] + s['ManaCostPercentage'] * ch.base_mana // 100
                    self.assertEqual(out['cost'], int(F.apply_spell_mod(s, F.SPELLMOD_COST, cost, ch.mods)))
                    self.assertEqual(out['cast_ms'], int(F.apply_spell_mod(
                        s, F.SPELLMOD_CASTING_TIME, self.cat.cast_time(s), ch.mods)))
                    cd = max(int(F.apply_spell_mod(s, F.SPELLMOD_COOLDOWN, s['RecoveryTime'], ch.mods)),
                             int(F.apply_spell_mod(s, F.SPELLMOD_COOLDOWN,
                                                  s['CategoryRecoveryTime'] if s['Category'] else 0, ch.mods)))
                    self.assertEqual(out['cooldown_ms'], cd)
                    self.assertEqual(out['gcd_ms'], s['StartRecoveryTime'])
                    self.assertEqual(out['interrupt_flags'], s['InterruptFlags'])
                    self.assertEqual(out['channel_interrupt_flags'], s['ChannelInterruptFlags'])
                    duration = int(F.apply_spell_mod(s, F.SPELLMOD_DURATION, self.cat.duration(s, 60), ch.mods))
                    for a in out['auras']:
                        self.assertEqual(a['duration_ms'], duration)
                        self.assertEqual(a['amplitude_ms'], s[f"EffectAmplitude{a['eff']}"])
                    expected = [{'item': s[f'Reagent{i}'], 'count': s[f'ReagentCount{i}']}
                                for i in range(1, 9) if s[f'Reagent{i}'] > 0]
                    self.assertEqual(out.get('reagents', []), expected)
                    self.assertEqual(out, self.answer(req))
        for name, ms in (('rebirth', 1800000), ('barkskin', 60000), ('innervate', 360000), ('tranquility', 300000)):
            self.assertEqual(self.answer(self.request(name))['cooldown_ms'], ms)

    def test_starfire_and_regrowth_bonus_coefficients_and_crits(self):
        seen = set()
        for items in ([], FULL):
            ch = character.build(self.cat, items)
            for name in ('starfire', 'regrowth'):
                s = self.cat.spell(BOOK[name])
                for seed in (1, 2, 3, 11, 31):
                    rng = random.Random(seed)
                    out = self.answer(self.request(name, items, seed))
                    if name == 'starfire':
                        miss = F.roll_chance(rng, F.spell_miss_chance(60, 60, False, ch.spell_hit()))
                        if miss:
                            self.assertEqual(out['events'][0]['type'], 'miss')
                            continue
                    crit = F.roll_chance(rng, ch.spell_crit(s['School']))
                    base = int(F.effect_value(s, 1, 60, rng, mods=ch.mods))
                    bonus = ch.spell_damage(1 << s['School']) if name == 'starfire' else ch.healing_done(1 << s['School'])
                    expected = int(F.apply_spell_mod(s, F.SPELLMOD_DAMAGE,
                        base + F.bonus_with_coeffs(s, 1, 0, bonus, ch.mods), ch.mods))
                    if crit:
                        expected = int(expected * F.crit_multiplier(s['DmgClass']))
                    event = out['events'][0]
                    self.assertEqual((event['amount'], event['crit']), (expected, crit))
                    self.assertEqual(event['type'], 'damage' if name == 'starfire' else 'heal')
                    seen.add(crit)
                    if name == 'regrowth':
                        base = int(F.effect_value(s, 2, 60, rng, mods=ch.mods))
                        tick = int(F.apply_spell_mod(s, F.SPELLMOD_DOT,
                            base + F.bonus_with_coeffs(s, 2, 0, bonus, ch.mods), ch.mods))
                        aura = out['auras'][0]
                        self.assertEqual(aura['per_tick'], tick)
                        self.assertEqual(aura['ticks'], [{'t_ms': t, 'amount': tick} for t in range(3000, 21001, 3000)])
        self.assertEqual(seen, {False, True})

    def test_buffs_debuffs_and_shield_values(self):
        expected_types = {'faerie_fire': ['armor', 'dispel_immunity', 'dispel_immunity'],
                          'mark_of_the_wild': ['armor', 'stat', 'resistance'],
                          'gift_of_the_wild': ['armor', 'stat', 'resistance'],
                          'innervate': ['mana_regen_interrupt', 'mana_regen_percent'],
                          'barkskin': ['pushback_immunity', 'spell_mod'],
                          'thorns': ['damage_shield'], 'soothe_animal': ['detect_range']}
        for name, kinds in expected_types.items():
            s = self.cat.spell(BOOK[name])
            out = self.answer(self.request(name))
            self.assertEqual([a['type'] for a in out['auras']], kinds)
            for aura in out['auras']:
                if 'amount' in aura:
                    self.assertEqual(aura['amount'], int(F.effect_value(s, aura['eff'], 60, None, maximum=True)))
        ff = self.answer(self.request('faerie_fire'))['auras']
        self.assertEqual([a['dispel'] for a in ff[1:]], ['Invisibility', 'Stealth'])
        self.assertEqual(ff[0]['school_mask'], 1)
        gift = self.answer(self.request('gift_of_the_wild'))['auras']
        for a in gift:
            self.assertEqual(a['area'], {'radius_yd': self.cat.radius_yd(self.cat.spell(21850), a['eff']),
                                         'party': True, 'channel': False})
        self.assertEqual(gift[1]['stat'], -1)
        self.assertEqual((gift[2]['school_mask'], gift[2]['exclusive']), (126, True))
        bark = self.answer(self.request('barkskin'))['auras']
        self.assertEqual(bark[0]['school_mask'], 127)
        self.assertEqual((bark[1]['op'], bark[1]['mask'], bark[1]['modifier']), (10, 16777829, 'flat'))
        self.assertNotIn('damage_taken_percent', [a['type'] for a in bark])
        self.assertEqual(self.answer(self.request('thorns'))['auras'][0]['school'], 3)

    def test_hibernate_types_single_target_damage_and_heartbeat(self):
        for creature in ('Beast', 'Dragonkin'):
            req = self.request('hibernate')
            req['target']['creature_type'] = creature
            out = self.answer(req)
            aura = out['auras'][0]
            self.assertEqual((aura['type'], aura['breaks_on_damage'], aura['single_target']), ('stun', True, True))
            self.assertEqual(aura['heartbeat']['chance'], F.spell_miss_chance(60, 60, False, 0))
            self.assertEqual(aura['heartbeat']['interval_ms'], self.cat.duration(self.cat.spell(18658), 60) // 2)
        for name in ('hibernate', 'soothe_animal'):
            for creature in ('Humanoid', 'Undead', None, 'Dragonkin'):
                if name == 'hibernate' and creature == 'Dragonkin':
                    continue
                req = self.request(name)
                req['target']['creature_type'] = creature
                self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_BAD_TARGETS')
            req = self.request(name)
            req['target']['resist'] = {'3': 200}
            out = self.answer(req)
            pct = F.effective_resist_percent(60, 60, 200, True, True)
            self.assertEqual(out['hit_chance'], 100 - 4 - pct)
            self.assertEqual(out['events'][0]['type'], 'resist')

    def test_dispels_and_rebirth_are_not_healing(self):
        for name, dispel in (('remove_curse', 'Curse'), ('cure_poison', 'Poison'), ('abolish_poison', 'Poison')):
            s = self.cat.spell(BOOK[name])
            eff = 2 if name == 'abolish_poison' else 1
            out = self.answer(self.request(name))
            self.assertEqual(out['events'], [{'type': 'dispel', 'dispel': dispel,
                                            'count': int(F.effect_value(s, eff, 60, None, maximum=True))}])
        aura = self.answer(self.request('abolish_poison'))['auras'][0]
        trigger = self.cat.spell(self.cat.spell(2893)['EffectTriggerSpell1'])
        self.assertEqual((aura['type'], aura['spell'], aura['dispel'], aura['count']),
                         ('periodic_dispel', trigger['Id'], 'Poison', int(F.effect_value(trigger, 1, 60, None, maximum=True))))
        self.assertEqual((aura['duration_ms'], aura['amplitude_ms']), (8000, 2000))
        s = self.cat.spell(20748)
        for items in ([], FULL):
            for seed in (1, 31):
                out = self.answer(self.request('rebirth', items, seed))
                self.assertEqual(out['events'], [{'type': 'resurrect',
                    'health': int(F.effect_value(s, 1, 60, None, maximum=True)), 'mana': s['EffectMiscValue1']}])

    def test_target_and_reagent_validation(self):
        for name in BOOK:
            req = self.request(name)
            req['target']['dead'] = name != 'rebirth'
            self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_TARGET_NOT_DEAD' if name == 'rebirth'
                             else 'SPELL_FAILED_TARGETS_DEAD')
        for name in ('regrowth', 'mark_of_the_wild', 'innervate', 'thorns', 'remove_curse', 'cure_poison'):
            req = self.request(name)
            req['target']['friendly'] = False
            self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_BAD_TARGETS')
        for name, entry in (('rebirth', '17038'), ('gift_of_the_wild', '17026')):
            for inventory in ({}, {entry: 0}, {'123': 99}):
                req = self.request(name)
                req['caster']['reagents'] = inventory
                self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_REAGENTS')
            req = self.request(name)
            req['caster'].pop('reagents')
            self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_REAGENTS')
            req['caster']['reagents'] = {entry: 2}
            before = copy.deepcopy(req)
            self.assertTrue(self.answer(req)['ok'])
            self.assertEqual(req, before)

    def test_rebirth_revives_hostile_corpses_without_relaxing_other_checks(self):
        for converted in (False, True):
            req = self.request('rebirth')
            req['target'].update(friendly=False, healable=False, attackable=True, creature_type='Beast')
            if converted:
                req['caster']['skyrim'] = {'alteration': 100, 'restoration': 100}
            before = copy.deepcopy(req)
            out = self.answer(req)
            self.assertTrue(out['ok'], out)
            self.assertEqual([e['type'] for e in out['events']], ['resurrect'])
            self.assertEqual(out['reagents'], [{'item': 17038, 'count': 1}])
            self.assertEqual(req, before)
            for change, error in (({'dead': False}, 'SPELL_FAILED_TARGET_NOT_DEAD'),
                                  ({'distance_yd': 1000}, 'SPELL_FAILED_OUT_OF_RANGE'),
                                  ({'los': False}, 'SPELL_FAILED_LINE_OF_SIGHT')):
                bad = copy.deepcopy(req)
                bad['target'].update(change)
                self.assertEqual(self.answer(bad)['error'], error)
            req['caster']['reagents'] = {}
            self.assertEqual(self.answer(req)['error'], 'SPELL_FAILED_REAGENTS')

    def test_tranquility_party_channel_and_friendly_ticks(self):
        s = self.cat.spell(9863)
        for items in ([], FULL):
            ch = character.build(self.cat, items)
            base = int(F.effect_value(s, 1, 60, None, maximum=True, mods=ch.mods))
            amount = base + F.bonus_with_coeffs(s, 1, 0, ch.healing_done(1 << s['School']), ch.mods)
            out = self.answer(self.request('tranquility', items))
            aura = out['auras'][0]
            self.assertEqual(aura['type'], 'heal')
            self.assertEqual(aura['area'], {'radius_yd': self.cat.radius_yd(s, 1), 'party': True, 'channel': True})
            self.assertEqual(aura['per_tick'], amount)
            self.assertNotIn('ticks', aura)  # party membership is found anew at each tick
            for seed in (1, 31, 50):
                req = {'op': 'tick_area', 'spell': 9863, 'seed': seed, 'tick': 3, 'caster': {'items': items},
                       'targets': [{'id': 1, 'level': 90, 'friendly': True, 'resist': {'3': 10000}},
                                   {'id': 2, 'friendly': True}, {'id': 3, 'friendly': False},
                                   {'id': 4, 'friendly': True, 'dead': True}]}
                out = self.answer(req)
                self.assertEqual(out['tick'], 3)
                self.assertEqual(out['results'], [{'id': i, 'type': 'heal', 'amount': amount, 'school': s['School']}
                                                 for i in (1, 2)])

    def test_active_auras_apply_mods_stats_and_regen_without_cache_leaks(self):
        caster = {'items': FULL}
        plain = self.svc.char(caster)
        bark = self.svc.char({**caster, 'auras': [22812]})
        for name in ('wrath', 'starfire', 'regrowth', 'hibernate', 'soothe_animal', 'healing_touch'):
            sid = self.cat.slice[name]
            s = self.cat.spell(sid)
            expected = self.cat.cast_time(s)
            expected = F.apply_spell_mod(s, F.SPELLMOD_CASTING_TIME, expected, bark.mods)
            req = self.request(name) if name in BOOK else {
                'op': 'resolve', 'spell': sid, 'target': HOSTILE if name == 'wrath' else {'kind': 'self'}}
            req['caster'] = {**caster, 'auras': [22812]}
            self.assertEqual(self.answer(req)['cast_ms'], int(expected))
        req = self.request('starfire')
        old = self.answer(req)['cast_ms']
        req['caster']['auras'] = [22812]
        self.assertEqual(self.answer(req)['cast_ms'], old + 1000)
        mark = self.svc.char({**caster, 'auras': [9885]})
        self.assertEqual(mark.stats, {k: v + 12 for k, v in plain.stats.items()})
        inn = self.svc.char({**caster, 'auras': [29166]})
        expected = F.mana_regen_per_second(11, plain.stats['spi']) * 5 + plain.total(character.A_MOD_POWER_REGEN) / 5
        self.assertEqual(inn.regen()['per_sec'], expected)
        self.assertEqual(inn.regen()['per_sec_casting'], expected)
        self.assertIs(self.svc.char(caster), plain)
        self.assertIs(self.svc.char({**caster, 'auras': [22812, 22812]}), bark)

    def test_damage_taken_percent_outcome_keeps_school_and_percent(self):
        # Protocol support for aura 87 is independent of Barkskin: the core
        # never applies the tooltip's 22839, which is absent from this catalog.
        spell = {**self.cat.spell(22812), 'Id': -1, 'EffectApplyAuraName1': 87,
                 'EffectBasePoints1': -21, 'EffectMiscValue1': 1, 'Effect3': 0}
        self.cat.spells[-1] = spell
        req = {'op': 'resolve', 'spell': -1, 'target': {'kind': 'self'}, 'caster': {'skyrim': {}}}
        aura = self.answer(req)['auras'][0]
        self.assertEqual((aura['type'], aura['amount'], aura['school_mask']), ('damage_taken_percent', -20, 1))

    def test_skyrim_amounts_for_entire_book(self):
        for name in BOOK:
            req = self.request(name)
            wow = self.answer(req)
            req['caster']['skyrim'] = {k: 80 for k in ('destruction', 'restoration', 'alteration')}
            scaled = self.answer(req)
            rule = SPELLS[BOOK[name]]
            factor = rule.health_factor
            with self.subTest(name=name):
                self.assertEqual(scaled['cost'], max(1, round(rule.base_cost * skyrim.cost_factor(80))))
                self.assertEqual(scaled['skill'], skyrim.spell_skill(self.cat.spell(BOOK[name])))
                for original, converted in zip(wow['events'] + wow['auras'], scaled['events'] + scaled['auras']):
                    if original['type'] in ('damage', 'heal', 'damage_shield'):
                        for key in ('amount', 'per_tick', 'resisted'):
                            if key in original:
                                self.assertEqual(converted[key], round(original[key] * factor))
                        for a, b in zip(original.get('ticks', []), converted.get('ticks', [])):
                            self.assertEqual(b['amount'], round(a['amount'] * factor))
                    elif original['type'] == 'resurrect':
                        self.assertEqual(converted['health'], round(original['health'] * factor))
                        self.assertEqual(converted['mana'], 75)
                    elif 'amount' in original:
                        self.assertEqual(converted['amount'], original['amount'])
                    self.assertNotIn('skyrim', original)
        req = {'op': 'tick_area', 'spell': 9863, 'targets': [{'id': 1, 'friendly': True}], 'caster': {'items': FULL}}
        wow = self.answer(req)
        req['caster']['skyrim'] = {'restoration': 80}
        self.assertEqual(self.answer(req)['results'][0]['amount'], round(wow['results'][0]['amount'] * 250 / 1470))

    def test_skyrim_buff_table_and_detrimental_armor(self):
        for name in ('mark_of_the_wild', 'gift_of_the_wild'):
            req = self.request(name)
            req['caster']['skyrim'] = {}
            armor, stats, resists = self.answer(req)['auras']
            self.assertEqual(armor['skyrim'], [{'mgef': ARMOR_BUFF, 'magnitude': 40}])
            self.assertEqual(stats['skyrim'], [
                {'mgef': 0x493AA, 'magnitude': 20}, {'mgef': 0x49504, 'magnitude': 20},
                {'mgef': 0x49507, 'magnitude': 20}])
            expected = {}
            for effect in RESISTS.values():
                expected[effect] = expected.get(effect, 0) + resists['amount'] / 3
            self.assertEqual(resists['skyrim'], [{'mgef': k, 'magnitude': v} for k, v in sorted(expected.items())])
        req = self.request('faerie_fire')
        req['caster']['skyrim'] = {}
        armor = self.answer(req)['auras'][0]
        self.assertEqual(armor['skyrim'], [{'mgef': ARMOR_DEBUFF, 'magnitude': 40}])
        req = self.request('innervate')
        req['caster']['skyrim'] = {}
        regen = self.answer(req)['auras'][1]
        self.assertEqual(regen['skyrim'], [{'mgef': 0x7A0FD, 'magnitude': 50}])


if __name__ == '__main__':
    unittest.main()
