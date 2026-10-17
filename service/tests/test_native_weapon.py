"""Her swings in Skyrim's health come from the weapon's native damage at her skill, and the tooltip says what they deal."""
import statistics
import unittest

from service.catalog import Catalog
from service.server import Service
from service.skyrim import weapon_skill_multiplier

TARGET = {'level': 60, 'alive': True, 'attackable': True, 'in_front': False, 'armor_reduction': 10 ** 6}


class NativeWeapon(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()

    def setUp(self):
        self.service = Service(self.cat, None)

    def caster(self, skill, entry=873, damage=22, **modifiers):
        return {'level': 60, 'items': [entry, 16897],
                'skyrim': {'one_handed': skill, 'two_handed': skill, 'archery': skill, 'destruction': 50},
                'equipment_modifiers': {'weapon_entry': entry, 'weapon_damage': damage, 'enchantments': [], **modifiers}}

    def swings(self, caster, count=600):
        out = [self.service.handle({'op': 'melee', 'caster': caster, 'target': TARGET, 'seed': seed}) for seed in range(count)]
        self.assertTrue(all('weapon_factor' not in r for r in out))
        return out

    def tooltip(self, caster, entry=873, damage=22):
        reply = self.service.handle({'op': 'skyrim_costs', 'caster': caster, 'weapons': {str(entry): damage}})
        return reply['weapon_tooltips'][str(entry)]

    def test_vanilla_skill_formula(self):
        self.assertEqual([weapon_skill_multiplier(s) for s in (-5, 0, 15, 50, 100, 140)], [1, 1, 1.075, 1.25, 1.5, 1.5])

    def test_tooltip_is_what_her_swings_deal_at_skills_15_50_100(self):
        for skill in (15, 50, 100):
            caster = self.caster(skill)
            tip = self.tooltip(caster)
            # Staff of Jordan's Skyrim damage 22 at her skill; nothing else multiplies it here.
            self.assertAlmostEqual((tip['min'] + tip['max']) / 2, 22 * (1 + skill / 200), places=6)
            self.assertAlmostEqual(tip['dps'], (tip['min'] + tip['max']) / 2 / 3.7, places=6)
            replies = self.swings(caster)
            hits = [r['damage'] for r in replies if r['outcome'] == 'hit']
            crits = [r['damage'] for r in replies if r['outcome'] == 'crit']
            self.assertGreater(len(hits), 150, skill)
            self.assertGreater(len(crits), 5, skill)
            # Each plain hit lies in the tooltip's range (whole health points), and they average its middle.
            self.assertTrue(all(round(tip['min']) - 1 <= h <= round(tip['max']) + 1 for h in hits), (skill, tip, hits))
            self.assertAlmostEqual(statistics.mean(hits), (tip['min'] + tip['max']) / 2, delta=(tip['max'] - tip['min']) * .1)
            # WoW's crits still double, against the same tooltip.
            self.assertAlmostEqual(statistics.mean(crits), tip['min'] + tip['max'], delta=(tip['max'] - tip['min']) * .3)

    def test_tempering_fortify_and_perks_reach_both(self):
        plain = self.tooltip(self.caster(50))
        # The wielded copy's tempering scales the swing; the per-entry tooltip stays untempered (Lua scales each copy).
        tempered = self.caster(50, temper_multiplier=1.5, weapon_skill='TwoHanded', live={'97': 20})
        tip = self.tooltip(tempered)
        self.assertAlmostEqual((tip['min'] + tip['max']) / (plain['min'] + plain['max']), 1.2, places=6)   # Fortify 20%
        hits = [r['damage'] for r in self.swings(tempered, 400) if r['outcome'] == 'hit']
        self.assertAlmostEqual(statistics.mean(hits), (tip['min'] + tip['max']) / 2 * 1.5, delta=(tip['max'] - tip['min']) * .15)

    def test_other_weapons_tooltip_as_if_wielded(self):
        caster = self.caster(100)
        reply = self.service.handle({'op': 'skyrim_costs', 'caster': caster,
                                     'weapons': {'873': 22, '17182': 27, '19019': 12, '16897': 45}})
        tips = reply['weapon_tooltips']
        self.assertEqual(set(tips), {'873', '17182', '19019'})   # a chest is no weapon
        self.assertAlmostEqual((tips['17182']['min'] + tips['17182']['max']) / 2, 27 * 1.5, places=6)
        self.assertAlmostEqual((tips['19019']['min'] + tips['19019']['max']) / 2, 12 * 1.5, places=6)
        self.assertEqual(tips['19019']['delay_ms'], 1900)
        sulfuras = self.caster(100, 17182, 27)
        hits = [r['damage'] for r in self.swings(sulfuras, 400) if r['outcome'] == 'hit']
        self.assertAlmostEqual(statistics.mean(hits), (tips['17182']['min'] + tips['17182']['max']) / 2,
                               delta=(tips['17182']['max'] - tips['17182']['min']) * .1)

    def test_pure_wow_older_plugins_and_claws_are_unchanged(self):
        native = self.caster(50)
        older = {**native, 'equipment_modifiers': {'weapon_entry': 873, 'enchantments': []}}
        wow = {k: v for k, v in native.items() if k != 'skyrim'}
        for seed in range(40):
            req = {'op': 'melee', 'target': TARGET, 'seed': seed}
            pure = self.service.handle({**req, 'caster': wow})
            self.assertNotIn('weapon_factor', pure)
            roll = self.service.handle({**req, 'caster': {**wow, 'equipment_modifiers': {'weapon_entry': 873}}})
            self.assertEqual(pure['damage'], roll['damage'])   # WoW units: a native damage changes nothing
            old = self.service.handle({**req, 'caster': older})
            self.assertEqual(old['damage'], round(pure['damage'] * .25 * (.5 + 50 / 100)))   # the earlier conversion
            for form in ('cat', 'bear'):
                claws = self.service.handle({**req, 'caster': {**native, 'form': form}})
                bare = self.service.handle({**req, 'caster': {**older, 'form': form}})
                self.assertEqual(claws['damage'], bare['damage'], form)
        self.assertEqual(self.service.handle({'op': 'skyrim_costs', 'caster': wow, 'weapons': {'873': 22}})['weapon_tooltips'], {})


if __name__ == '__main__':
    unittest.main()
