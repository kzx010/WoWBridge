"""Approved tuning: actual native outcomes, tooltip tokens and WoW isolation."""
import copy
import random
import unittest

from service import feral, formulas as F, skyrim
from service.catalog import Catalog
from service.server import Service
from service.tests.test_skyrim_balance import request_for, FULL
from service.tests import test_druid_feral_talents as feral_tests, test_druid_balance_resto as caster_tests


class BalanceTuning(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()
        feral_tests.DruidFeralTalents.setUpClass()
        caster_tests.DruidBalanceRestoration.setUpClass()

    def setUp(self):
        self.svc = Service(self.cat, None)

    def request(self, sid, skill=50, **caster):
        req = request_for(self.cat, sid, seed=1)
        req['caster'].update(level=60, talents={}, skyrim={key: skill for key in skyrim.SKILLS.values()}, **caster)
        return req

    def test_changed_costs_and_tooltip_costs_match_all_ranks_skills_gear_talents(self):
        # The client reads these cost entries directly for its spell/form tooltips.
        for anchor in (20748, 17402, 25298, 9863, 29166, 768, 9634):
            ids = [sid for sid in self.cat.ranks if self.cat.chain(sid) == self.cat.chain(anchor)]
            if anchor in (768, 9634):
                ids = [anchor]
            for skill in (15, 50, 100):
                for equipped in (False, True):
                    req = self.request(anchor, skill, items=FULL if equipped else [],
                                       equipment_modifiers={'enchantments': [{'av': 110, 'magnitude': 20},
                                                                            {'av': 112, 'magnitude': 15}]})
                    if anchor in (768, 9634):
                        # Natural Shapeshifter, with legal prerequisite filler.
                        helper = caster_tests.DruidBalanceRestoration(); helper.setUp()
                        req['caster'].update(level=100, talents=helper.ranks(781, 3))
                    book = self.svc.handle({'op': 'skyrim_costs', 'caster': req['caster']})
                    costs = []
                    for sid in sorted(ids, key=lambda sid: self.cat.ranks[sid]['rank']):
                        req['spell'] = sid
                        spell = self.cat.spell(sid)
                        req['caster']['reagents'] = {str(spell[f'Reagent{e}']): 99 for e in range(1, 9) if spell[f'Reagent{e}'] > 0}
                        out = self.svc.handle(req)
                        self.assertEqual(out['cost'], book['costs'][str(sid)], (sid, skill, equipped))
                        costs.append(out['cost'])
                    self.assertEqual(costs, sorted(costs))
                    if anchor == 20748:
                        self.assertLess(costs[0], costs[-1])
        for sid in (768, 9634):
            req = self.request(sid)
            req['caster']['form'] = feral.FORM_SPELLS[sid]
            self.assertEqual(self.svc.handle(req)['cost'], 0)
            self.assertEqual(self.svc.handle({'op': 'skyrim_costs', 'caster': req['caster']})['costs'][str(sid)], 0)

    def test_innervate_keeps_only_regeneration_and_is_net_positive(self):
        for skill in (15, 50, 100):
            for items in ([], FULL):
                req = self.request(29166, skill, items=items)
                out = self.svc.handle(req)
                self.assertEqual(out['events'], [])
                self.assertEqual([a['type'] for a in out['auras']], ['mana_regen_interrupt', 'mana_regen_percent'])
                regen = out['auras'][1]
                native = regen['skyrim'][0]['magnitude']
                self.assertEqual(native, out['tooltip']['effects'][1][0])
                self.assertEqual(native, 200)
                self.assertEqual(regen['duration_ms'], out['tooltip']['duration_ms'])
                self.assertGreater(.99 * native / 100 * regen['duration_ms'] / 1000, out['cost'])
                req['caster'].pop('skyrim')
                wow = self.svc.handle(req)
                self.assertEqual(wow['auras'][1]['amount'], 400)
                self.assertEqual(wow['tooltip']['effects'][1], [400, 400])

    def test_changed_caster_health_tokens_match_actual_outcomes(self):
        for anchor in (20748, 17402, 25298, 9863):
            ids = [sid for sid in self.cat.ranks if self.cat.chain(sid) == self.cat.chain(anchor)]
            for sid in ids:
                for skill in (15, 50, 100):
                    req = self.request(anchor, skill, items=FULL)
                    req['spell'] = sid
                    spell = self.cat.spell(sid)
                    req['caster']['reagents'] = {str(spell[f'Reagent{e}']): 99 for e in range(1, 9) if spell[f'Reagent{e}'] > 0}
                    out = self.svc.handle(req)
                    if anchor in (17402, 9863):
                        tick = self.svc.handle({'op': 'tick_area', 'spell': sid, 'seed': 1, 'caster': req['caster'],
                                                'targets': [{'id': 1, 'level': 60, 'friendly': anchor == 9863}]})
                        events = tick['results']; eff = 1
                    else:
                        events = out['events']; eff = next(e for e in (1, 2, 3) if spell[f'Effect{e}'] in (2, 113))
                    bounds = [round(v * out['tooltip']['health_factor']) for v in out['tooltip']['effects'][eff-1]]
                    if out['tooltip']['minimum_health']:
                        bounds = [max(1, v) for v in bounds]
                    for event in events:
                        if event.get('crit') or event.get('resisted'):
                            continue
                        value = event.get('health', event.get('amount'))
                        self.assertGreaterEqual(value, bounds[0], (sid, event, bounds))
                        self.assertLessEqual(value, bounds[1], (sid, event, bounds))

    def test_form_armor_and_thick_hide_tokens_match_service_profile(self):
        helper = feral_tests.DruidFeralTalents(); helper.setUpClass(); helper.setUp()
        for sid, form, bonus, passive in ((5487, 'bear', 50, 1178), (9634, 'dire_bear', 100, 9635), (24858, 'moonkin', 100, 24905)):
            for rank in range(6):
                for skill in (15, 50, 100):
                    talents = helper.ranks(794, rank)
                    if sid == 24858:
                        moon = caster_tests.DruidBalanceRestoration(); moon.setUp()
                        talents.update(moon.ranks(793, 1))
                    req = {'op': 'resolve', 'spell': sid, 'seed': 1, 'target': {'kind': 'self'},
                           'caster': {'level': 100, 'talents': talents, 'items': FULL, 'skyrim': {'alteration': skill}}}
                    out = self.svc.handle(req)
                    armor = out['auras'][0]['skyrim_armor_percent']
                    self.assertAlmostEqual(armor, ((1 + bonus/100) * (1 + .02*rank) - 1) * 100)
                    self.assertEqual(out['tooltip']['linked_spells'][str(passive)]['effects'][0], [armor, armor])
                    stats = self.svc.handle({'op': 'stats', 'caster': {**req['caster'], 'form': form}})
                    self.assertEqual(armor, stats['form']['skyrim_armor_percent'])
                    if rank:
                        from service.talent_effects import tooltip
                        talent = self.cat.spell(helper.talents[794]['rank_spells'][rank-1])
                        self.assertEqual(tooltip(self.cat, self.svc.char(req['caster']), talent)['effects'][0], [2*rank, 2*rank])

    def test_feral_floor_keeps_gear_and_talent_additions_at_full_conversion(self):
        from service.talent_effects import tooltip
        # Swipe rank 1: floor acts on the original 18-point roll, not on gear
        # or Savage Fury's completed additions. Repeat with/without equipment.
        helper = feral_tests.DruidFeralTalents(); helper.setUpClass(); helper.setUp()
        for items in ([], FULL):
            for rank in (0, 2):
                level = 20 if rank == 0 else 100
                caster = {'level': level, 'form': 'bear', 'items': items, 'talents': helper.ranks(805, rank) if rank else {},
                          'power': {'rage': 100}, 'skyrim': {'one_handed': 15}}
                native = self.svc.char(caster)
                wow = self.svc.char({k:v for k,v in caster.items() if k != 'skyrim'})
                spell = self.cat.spell(779)
                raw = int(F.effect_value(spell, 1, level, None, maximum=True))
                top = int(F.effect_value(self.cat.spell(9908), 1, 60, None, maximum=True))
                original = feral._bonus(wow, spell, 1, raw)
                adjusted = tooltip(self.cat, native, spell)['effects'][0][0]
                self.assertEqual(adjusted, original + .5 * (top - raw))
                req = {'op': 'resolve', 'spell': 779, 'seed': 1, 'caster': caster,
                       'target': {'kind': 'unit', 'level': level, 'id': 1, 'in_front': True, 'behind': True, 'distance_yd': 2,
                                  'armor_reduction': self.cat.creature_armor(level)}}
                out = self.svc.handle(req)
                self.assertFalse(out['events'][0]['crit'])
                self.assertEqual(out['events'][0]['amount'], round(int(adjusted) * out['tooltip']['health_factor']))

    def test_white_novice_floor_uses_naked_roll_and_preserves_gear_bonus(self):
        for items in ([], FULL):
            req = {'op': 'melee', 'seed': 11, 'caster': {'level': 20, 'form': 'cat', 'items': items,
                   'talents': {}, 'skyrim': {'one_handed': 15}},
                   'target': {'level': 20, 'distance_yd': 2, 'in_front': False,
                              'armor_reduction': self.cat.creature_armor(20)}}
            ch = self.svc.char(req['caster'])
            low, high = ch.weapon_damage_range()
            rng = random.Random(11); rng.randint(1, 10000)
            roll = rng.randint(int(low), int(high))
            position = max(0, min(1, (roll-low)/(high-low)))
            bare = self.svc.char({'level': 20, 'form': 'cat'}).weapon_damage_range()
            top = self.svc.char({'level': 60, 'form': 'cat'}).weapon_damage_range()
            baseline = bare[0] + (bare[1]-bare[0])*position
            reference = top[0] + (top[1]-top[0])*position
            adjusted = roll + .5*(reference-baseline)
            out = self.svc.handle(req)
            self.assertEqual(out['outcome'], 'hit')
            self.assertEqual(out['damage'], round(int(adjusted) * 7/96.5 * 1.075))

    def test_feral_skill_fortify_weapon_isolation_and_bleed_tooltips(self):
        for skill in (15, 50, 100):
            for fortify in (0, 50):
                for sid in (9881, 9908, 9850, 9830, 9904, 9896, 9827, 9867, 9846):
                    req = self.request(sid, skill, items=[19364], equipment_modifiers={
                        'weapon_entry': 19364, 'weapon_skill': 'TwoHanded', 'temper_multiplier': 2,
                        'enchantments': [{'av': 96, 'magnitude': fortify}]})
                    out = self.svc.handle(req)
                    factor = 7/96.5 * (1+.005*skill) * (1+fortify/100)
                    self.assertAlmostEqual(out['tooltip']['health_factor'], factor)
                    wow_request = copy.deepcopy(req); wow_request['caster'].pop('skyrim')
                    wow = self.svc.handle(wow_request)
                    for original, native in zip(wow['events'], out['events']):
                        if original['type'] == 'damage':
                            self.assertEqual(native['amount'], round(original['amount'] * factor))
                        elif original['type'] == 'next_swing':
                            self.assertEqual(native['bonus_damage'], round(original['bonus_damage'] * factor))
                    plain = copy.deepcopy(req); plain['caster']['equipment_modifiers']['temper_multiplier'] = 1
                    self.assertEqual(out, self.svc.handle(plain))
                    for aura in out['auras']:
                        if aura['type'] == 'damage':
                            profile = out['tooltip']
                            if sid == 9827:
                                profile = profile['linked_spells'][str(self.cat.spell(sid)['EffectTriggerSpell2'])]
                                eff = 1
                            else:
                                eff = aura['eff']
                            # Rip's client description uses literal combo tables;
                            # its token profile is the zero-combo component.
                            if sid == 9896:
                                continue
                            expected = round(profile['effects'][eff-1][0] * out['tooltip']['health_factor'])
                            self.assertEqual(aura['per_tick'], expected)
                            self.assertEqual([t['amount'] for t in aura['ticks']], [expected] * len(aura['ticks']))

    def test_feral_rank_order_with_floor_skill_fortify_and_gear(self):
        for top in (9881, 9908, 9850, 9830, 9904, 9896, 9827, 9867, 9846, 31018):
            ids = sorted((sid for sid in self.cat.ranks if self.cat.chain(sid) == self.cat.chain(top)),
                         key=lambda sid: self.cat.ranks[sid]['rank'])
            for skill in (15, 50, 100):
                for items in ([], FULL):
                    means = []
                    for sid in ids:
                        totals = []
                        for seed in range(32):
                            req = self.request(top, skill, items=items, equipment_modifiers={
                                'enchantments': [{'av': 96, 'magnitude': 50}]})
                            req.update(spell=sid, seed=seed)
                            out = self.svc.handle(req)
                            totals.append(sum(e.get('amount', e.get('bonus_damage', 0)) for e in out['events']) +
                                          sum(sum(t['amount'] for t in a.get('ticks', [])) if a['type'] == 'damage'
                                              else a.get('amount', 0) if a['type'] == 'damage_done' else 0 for a in out['auras']))
                        means.append(sum(totals)/len(totals))
                    self.assertEqual(means, sorted(means), (top, skill, items, means))


if __name__ == '__main__':
    unittest.main()
