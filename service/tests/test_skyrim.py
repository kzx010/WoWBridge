"""Skyrim protocol amounts paired with the same seeded WoW outcomes."""
import copy
import json
import unittest

from service import skyrim
from service.catalog import Catalog
from service.server import Service
from service.tests.test_service import FULL, HOSTILE

MOB = {'level': 60, 'distance_yd': 3.0, 'alive': True, 'reaction': 'hostile', 'facing': True, 'in_front': True}


class Skyrim(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()

    def setUp(self):
        self.svc = Service(self.cat, None)

    def pair(self, req, skills, items=FULL):
        req = {**req, 'caster': {'items': items}}
        wow = self.svc.respond(json.dumps(req).encode())
        req['caster']['skyrim'] = skills
        scaled = self.svc.respond(json.dumps(req).encode())
        self.assertTrue(wow['ok'], wow)
        self.assertTrue(scaled['ok'], scaled)
        self.assertNotIn('skill', wow)
        return wow, scaled

    def spell_request(self, name, seed=1, target=HOSTILE):
        return {'op': 'resolve', 'spell': self.cat.slice[name], 'seed': seed, 'target': target}

    def assert_health(self, wow, scaled, factor, keys=('amount', 'resisted', 'absorbed')):
        expected = copy.deepcopy(wow)
        for key in keys:
            if key in expected:
                expected[key] = round(expected[key] * factor)
        self.assertEqual(scaled, expected)

    def test_wrath_cost_damage_and_seeded_rolls(self):
        resistant = {**HOSTILE, 'resist': {'3': 300}}
        kinds, crits = set(), set()
        for seed in range(100):
            for level in (15, 50, 100):
                wow, scaled = self.pair(self.spell_request('wrath', seed, resistant), {'destruction': level})
                self.assertEqual(wow['cost'], 180)
                self.assertEqual(scaled['cost'], {15: 36, 50: 30, 100: 24}[level])
                self.assertEqual(scaled['skill'], 'Destruction')
                for a, b in zip(wow['events'], scaled['events']):
                    self.assert_health(a, b, 35 / 276)
                    kinds.add(a['type'])
                    if 'crit' in a:
                        crits.add(a['crit'])
                for key in wow.keys() - {'cost', 'events'}:
                    self.assertEqual(wow[key], scaled[key])
        self.assertTrue({'damage', 'miss', 'resist'} <= kinds, kinds)
        self.assertEqual(crits, {False, True})

    def test_healing_touch_and_missing_skill(self):
        req = self.spell_request('healing_touch', target={'kind': 'self'})
        for skills in ({'restoration': 15, 'destruction': 100}, {'restoration': 100}, {}):
            wow, scaled = self.pair(req, skills)
            self.assertEqual(scaled['skill'], 'Restoration')
            self.assert_health(wow['events'][0], scaled['events'][0], 150 / 2677)
        wow, scaled = self.pair(self.spell_request('wrath'), {'restoration': 100})
        self.assert_health(wow['events'][0], scaled['events'][0], 35 / 276)

    def test_roots_and_buffs_train_alteration(self):
        wow, scaled = self.pair(self.spell_request('entangling_roots', seed=11),
                                {'alteration': 15, 'destruction': 100})
        self.assertEqual(scaled['skill'], 'Alteration')
        self.assertEqual(wow['auras'][0], scaled['auras'][0])
        self.assert_periodic(wow['auras'][1], scaled['auras'][1], 24 / 270)
        # A buff's WoW stat magnitude stays intact; the added Skyrim effects use
        # the gear conversion table, independently of the caster's skill.
        self.svc.cat = Catalog()
        buff = -1
        self.svc.cat.spells[buff] = {**self.cat.spell(self.cat.slice['rejuvenation']), 'Id': buff,
                                    'EffectApplyAuraName1': 29}
        wow, scaled = self.pair({'op': 'resolve', 'spell': buff, 'target': {'kind': 'self'}}, {'alteration': 15})
        self.assertEqual(scaled['skill'], 'Alteration')
        self.assertEqual(scaled['auras'][0].pop('skyrim'), [
            {'mgef': 0x7A0F4, 'magnitude': wow['auras'][0]['amount']},
            {'mgef': 0x7A0FF, 'magnitude': wow['auras'][0]['amount'] * .5},
            {'mgef': 0x7A106, 'magnitude': wow['auras'][0]['amount'] * .5}])
        self.assertEqual(wow['auras'], scaled['auras'])

    def assert_periodic(self, wow, scaled, factor):
        self.assertEqual(scaled['per_tick'], round(wow['per_tick'] * factor))
        self.assertEqual(len(wow.get('ticks', [])), len(scaled.get('ticks', [])))
        for a, b in zip(wow.get('ticks', []), scaled.get('ticks', [])):
            self.assert_health(a, b, factor)
        for key in wow.keys() - {'per_tick', 'ticks'}:
            self.assertEqual(wow[key], scaled[key])

    def test_moonfire_and_periodic_healing(self):
        for name, skills, target, skill in (
                ('moonfire', {'destruction': 15}, {**HOSTILE, 'resist': {'6': 300}}, 'Destruction'),
                ('rejuvenation', {'restoration': 15}, {'kind': 'self'}, 'Restoration')):
            wow, scaled = self.pair(self.spell_request(name, seed=0, target=target), skills)
            self.assertEqual(scaled['skill'], skill)
            self.assertTrue(wow['auras'])
            for a, b in zip(wow['events'], scaled['events']):
                self.assert_health(a, b, 70 / 611 if name == 'moonfire' else 75 / 888)
            for a, b in zip(wow['auras'], scaled['auras']):
                self.assert_periodic(a, b, 70 / 611 if name == 'moonfire' else 75 / 888)

    def test_hurricane_snapshot_and_area_rolls(self):
        wow, scaled = self.pair(self.spell_request('hurricane', target={'kind': 'ground', 'distance_yd': 20}),
                                {'destruction': 100})
        self.assertEqual(scaled['skill'], 'Destruction')
        self.assert_periodic(wow['auras'][0], scaled['auras'][0], 240 / 1340)
        kinds = set()
        for seed in range(50):
            req = {'op': 'tick_area', 'spell': 17402, 'tick': 3, 'seed': seed,
                   'targets': [{'id': 1, 'level': 60, 'resist': {'3': 300}}, {'id': 2, 'level': 63}]}
            wow, scaled = self.pair(req, {'destruction': 100})
            self.assertEqual(scaled['skill'], 'Destruction')
            self.assertEqual(wow['tick'], scaled['tick'])
            self.assertEqual(len(wow['results']), len(scaled['results']))
            for a, b in zip(wow['results'], scaled['results']):
                self.assert_health(a, b, 240 / 1340)
                kinds.add(a['type'])
                if a['type'] == 'damage':   # its storm slows each unit it strikes by 25%
                    self.assertEqual((a['attack_slow_percent'], b['attack_slow_percent']), (25, 25))
        self.assertEqual(kinds, {'damage', 'miss'})

    def test_melee_staff_bare_hands_and_one_hand(self):
        # The slice has only a staff; use its item facts with a one-hand inventory type.
        self.svc.cat = Catalog()
        one_hand = -1
        self.svc.cat.items[one_hand] = {**self.cat.item(873), 'entry': one_hand, 'InventoryType': 13}
        outcomes = set()
        for items, skill in ((FULL, 'TwoHanded'), ([], 'OneHanded'), ([one_hand], 'OneHanded')):
            for seed in range(100):
                wow, scaled = self.pair({'op': 'melee', 'seed': seed, 'target': MOB},
                                        {'two_handed': 100, 'one_handed': 15}, items)
                self.assertEqual(scaled.pop('skill'), skill)
                self.assert_health(wow, scaled, 0.25 * (1.5 if skill == 'TwoHanded' else 0.65),
                                   ('damage', 'blocked'))
                outcomes.add(wow['outcome'])
        self.assertTrue({'hit', 'crit', 'block', 'glance', 'miss', 'dodge', 'parry'} <= outcomes, outcomes)

    def test_damage_taken_has_no_amounts_or_changed_rolls(self):
        for seed in range(20):
            wow, scaled = self.pair({'op': 'damage_taken', 'damage': 135, 'victim_level': 60, 'seed': seed}, {})
            self.assertEqual(scaled.pop('skill'), 'Alteration')
            self.assertEqual(wow, scaled)

    def test_minimum_cost_and_absorbed_parts(self):
        for cost, expected in ((0, 0), (1, 1), (2, 1), (180, 36)):
            out = {'cost': cost, 'events': [{'type': 'damage', 'amount': 101, 'resisted': 20, 'absorbed': 30}]}
            scaled = skyrim.convert(out, {'skyrim': {}}, 'Destruction')
            self.assertEqual(scaled['cost'], expected)
            self.assertEqual(scaled['events'][0], {'type': 'damage', 'amount': 25, 'resisted': 5, 'absorbed': 8})


if __name__ == '__main__':
    unittest.main()
