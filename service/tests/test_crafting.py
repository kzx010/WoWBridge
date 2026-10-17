import copy
import unittest

from service.catalog import Catalog
from service.server import Service


class Crafting(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()

    def setUp(self):
        self.service = Service(self.cat, None)
        self.caster = {'items': [873, 16897], 'level': 60,
                       'skyrim': {'destruction': 50, 'two_handed': 50}}

    def stats(self, caster):
        return self.service.handle({'op': 'stats', 'caster': caster})

    def test_sheet_attack_rating_matches_combat_weapon_skill(self):
        for level in (15, 30, 60):
            self.assertEqual(self.stats({**self.caster, 'level': level})['attack_rating'], level * 5)

    def test_vanilla_weapon_and_bare_hands_swing_at_their_native_damage(self):
        # A vanilla weapon (no WoW entry) and bare hands (the plugin's UnarmedDamage) govern the swing like a WoW
        # weapon's Skyrim damage: WoW's roll lends spread/table/crits only, attack power widens nothing. Before, both
        # fell back to scaling the whole WoW roll (AP included): a Daedric warhammer swung as WoW's 1-2 fist.
        target = {'level': 60, 'alive': True, 'attackable': True, 'in_front': False}
        vanilla = {**self.caster, 'items': [], 'equipment_modifiers': {'weapon_entry': 0, 'weapon_skill': 'TwoHanded',
                   'weapon_damage': 25, 'temper_multiplier': 1}}
        fists = {**self.caster, 'items': [], 'equipment_modifiers': {'weapon_entry': 0, 'weapon_skill': 'OneHanded',
                 'weapon_damage': 4, 'weapon_unarmed': True}}
        legacy = {**self.caster, 'items': [], 'equipment_modifiers': {}}
        def average(caster):
            total = n = 0
            for seed in range(300):
                out = self.service.handle({'op': 'melee', 'target': target, 'seed': seed, 'caster': caster})
                if out.get('outcome') == 'hit' and out.get('damage', 0) > 0:
                    total += out['damage']; n += 1
            return total / n if n else 0.0
        hammer, punch, old = average(vanilla), average(fists), average(legacy)
        self.assertGreater(hammer, punch * 3)                 # the warhammer is not a fist
        self.assertLess(punch, old)                           # a punch no longer carries the whole WoW roll's AP
        self.assertAlmostEqual(hammer / punch, 25 / 4, delta=1.5)   # both at their native damage, same multiplier

    def test_tempering_changes_cached_stats_and_same_seed_combat(self):
        before = self.stats(self.caster)
        improved = {**self.caster, 'equipment_modifiers': {
            'weapon_entry': 873, 'temper_multiplier': 1.5}}
        after = self.stats(improved)
        self.assertEqual(after['form']['weapon_damage']['min'], 119 * 1.5)
        self.assertGreater(after['form']['damage']['min'], before['form']['damage']['min'])
        target = {'level': 60, 'alive': True, 'attackable': True, 'in_front': False}
        hit = None
        for seed in range(100):
            req = {'op': 'melee', 'target': target, 'seed': seed}
            base = self.service.handle({**req, 'caster': self.caster})
            if base['damage'] > 0:
                hit = self.service.handle({**req, 'caster': improved})
                self.assertEqual(hit['outcome'], base['outcome'])
                self.assertGreater(hit['damage'], base['damage'])
                break
        self.assertIsNotNone(hit)
        self.assertEqual(self.stats(self.caster), before)
        # Selecting an offhand/higher entry must not displace the actual main hand.
        dual = {**improved, 'items': [873, 19019]}
        self.assertEqual(self.stats(dual)['form']['weapon_damage'], after['form']['weapon_damage'])

    def test_bow_tempering_and_archery_enchantment_reach_damage(self):
        caster = {**self.caster, 'items': [18713], 'equipment_modifiers': {
            'weapon_entry': 18713, 'weapon_skill': 'Marksman', 'temper_multiplier': 1.5,
            'enchantments': [{'av': 98, 'magnitude': 25}]}}
        ch = self.service.char(caster)
        bow = self.cat.item(18713)
        self.assertEqual(ch.weapon['min'], bow['dmg_min1'] * 1.5)
        self.assertEqual(ch.weapon_multiplier, 1.25)
        self.assertEqual(ch.weapon_skill, 'Marksman')
        reply = self.service.handle({'op': 'melee', 'caster': caster,
                                    'target': {'level': 60, 'attackable': True}, 'seed': 3})
        self.assertEqual(reply['skill'], 'Marksman')

    def test_enchantments_resources_resistance_cost_and_removal(self):
        before = self.stats(self.caster)
        enchanted = copy.deepcopy(self.caster)
        enchanted['equipment_modifiers'] = {'enchantments': [
            {'av': 25, 'magnitude': 30}, {'av': 24, 'magnitude': 50},
            {'av': 156, 'magnitude': 50}, {'av': 41, 'magnitude': 10},
            {'av': 43, 'magnitude': 12}, {'av': 40, 'magnitude': 8},
            {'av': 44, 'magnitude': 5}, {'av': 97, 'magnitude': 25},
            {'av': 110, 'magnitude': 25}]}
        after = self.stats(enchanted)
        self.assertEqual(after['max_mana'] - before['max_mana'], 150)
        self.assertEqual(after['native_health_bonus'], 50)
        self.assertGreater(after['regen']['per_sec'], before['regen']['per_sec'])
        for school, delta in ((2, 45), (4, 51), (3, 39), (5, 15), (6, 15)):
            self.assertEqual(after['resist'][str(school)] - before['resist'][str(school)], delta)
        self.assertAlmostEqual(after['form']['damage']['min'], before['form']['damage']['min'] * 1.25)
        costs = lambda c: self.service.handle({'op': 'skyrim_costs', 'caster': c})['costs']['9912']
        self.assertAlmostEqual(costs(enchanted), costs(self.caster) * .75, delta=1)
        self.assertEqual(self.stats(self.caster), before)
        free = copy.deepcopy(enchanted)
        free['equipment_modifiers']['enchantments'].append({'av': 110, 'magnitude': 75})
        self.assertEqual(costs(free), 0)
        # A potion or blessing: Skyrim's live Fortify Destruction value replaces the worn enchantments' sum.
        potion = copy.deepcopy(self.caster)
        potion['equipment_modifiers'] = {'enchantments': [{'av': 110, 'magnitude': 25}], 'live': {'110': 50}}
        self.assertAlmostEqual(costs(potion), costs(self.caster) * .5, delta=1)
        with self.assertRaises(Exception):
            costs({**self.caster, 'equipment_modifiers': {'live': {'110': 'nan'}}})

    def test_feral_claws_ignore_weapon_tempering_and_weapon_enchants(self):
        caster = {**self.caster, 'form': 'cat'}
        before = self.stats(caster)
        caster['equipment_modifiers'] = {'weapon_entry': 873, 'temper_multiplier': 2,
                                        'enchantments': [{'av': 97, 'magnitude': 50}]}
        self.assertEqual(self.stats(caster)['form']['damage'], before['form']['damage'])

    def test_invalid_modifier_cannot_poison_cached_character(self):
        bad = {**self.caster, 'equipment_modifiers': {'temper_multiplier': float('inf')}}
        reply = self.service.respond(__import__('json').dumps({'op': 'stats', 'caster': bad}).encode())
        self.assertFalse(reply['ok'])
        self.assertIn('BAD_REQUEST', reply['error'])
        self.assertGreater(self.stats(self.caster)['form']['damage']['min'], 0)
