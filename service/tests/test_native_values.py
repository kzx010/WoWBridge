"""Live Skyrim.esm stone magnitudes change the service's actual converted outcomes."""
import copy
import unittest

from service import formulas as F, native_values
from service.catalog import Catalog
from service.server import Service


def snapshot(values=None, owned=None):
    live = {str(av): 0 for av in native_values.VALUES}
    live.update({'27': .7, '28': 3, '29': 5, '155': 100, '156': 100, '157': 100})
    live.update({str(av): value for av, value in (values or {}).items()})
    return {'live': live, 'owned': {str(av): value for av, value in (owned or {}).items()},
            'base': {'27': .7, '28': 3, '29': 5}}


class NativeValues(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()

    def setUp(self):
        self.service = Service(self.cat, None)
        self.caster = {'level': 60, 'items': [], 'skyrim': {}, 'actor_values': snapshot()}
        self.target = {'kind': 'unit', 'level': 60, 'distance_yd': 20, 'alive': True,
                       'attackable': True, 'actor_values': snapshot()}

    def stats(self, data):
        return self.service.handle({'op': 'stats', 'caster': {**self.caster, 'actor_values': data}})

    def resolve(self, data, spell=9835, seed=1):
        return self.service.handle({'op': 'resolve', 'spell': spell, 'caster': self.caster,
                                    'target': {**self.target, 'actor_values': data}, 'seed': seed})

    def test_flat_percent_inverts_cmangos_at_each_level_and_sign(self):
        for attacker, victim in ((1, 1), (20, 30), (60, 60)):
            for pct in (25, 50, -100):
                data = native_values.snapshot(snapshot({44: pct}))
                resistance = native_values.resistance(data, 2, attacker, victim)
                self.assertAlmostEqual(F.effective_resist_percent(attacker, victim, resistance, True, False), pct)

    def test_lord_player_defense_and_incoming_converted_damage(self):
        lord = snapshot({39: 50, 44: 25})
        after = self.stats(lord)
        self.assertEqual(after['resist']['2'], 100)  # 100/300*75 = 25%, not a fixed +3R/point.
        req = {'op': 'damage_taken', 'school': 2, 'damage': 1000, 'victim_level': 60,
               'attacker_level': 60, 'seed': 0}
        base = self.service.handle({**req, 'caster': self.caster})
        changed = self.service.handle({**req, 'caster': {**self.caster, 'actor_values': lord}})
        self.assertAlmostEqual(changed['resist_percent'], 25)
        self.assertGreater(changed['resisted'], 0)
        self.assertLess(changed['damage'], base['damage'])

    def test_apprentice_negative_resistance_doubles_incoming_and_outgoing(self):
        apprentice = snapshot({44: -100, 156: 200})
        req = {'op': 'damage_taken', 'school': 2, 'damage': 1000, 'victim_level': 60,
               'attacker_level': 60, 'seed': 1}
        changed = self.service.handle({**req, 'caster': {**self.caster, 'actor_values': apprentice}})
        self.assertEqual((changed['damage'], changed['resisted'], changed['resist_percent']), (500, -250, -100))
        binary = self.service.handle({**req, 'binary': True, 'caster': {**self.caster, 'actor_values': apprentice}})
        self.assertEqual(binary['damage'], changed['damage'])
        base, changed = self.resolve(snapshot()), self.resolve(apprentice)
        damage = lambda out: next(e['amount'] for e in out['events'] if e['type'] == 'damage')
        self.assertGreater(damage(changed), damage(base))
        self.assertEqual(changed['hit_chance'], base['hit_chance'])

    def test_apprentice_does_not_remove_binary_miss_chance(self):
        base = self.resolve(snapshot(), spell=9853)
        changed = self.resolve(snapshot({44: -100}), spell=9853)
        self.assertEqual(changed['hit_chance'], base['hit_chance'])

    def test_stone_regeneration_and_removal_invalidate_character_cache(self):
        base = self.stats(snapshot())['regen']['per_sec']
        self.assertEqual(self.stats(snapshot({156: 200}))['regen']['per_sec'], base * 2)
        self.assertEqual(self.stats(snapshot({156: 50}))['regen']['per_sec'], base * .5)
        self.assertEqual(self.stats(snapshot())['regen']['per_sec'], base)
        # A disease changing the underlying rate also reaches spirit regen.
        self.assertEqual(self.stats(snapshot({28: 1.5}))['regen']['per_sec'], base * .5)

    def test_atronach_absorbs_whole_spell_once_and_restores_native_cost(self):
        # Pick an absorbed member of the deterministic 50% sequence.
        hits = [self.resolve(snapshot({83: 50, 156: 50}), seed=seed) for seed in range(20)]
        absorbed = [out for out in hits if any(e['type'] == 'absorb' for e in out['events'])]
        landed = [out for out in hits if any(e['type'] == 'damage' for e in out['events'])]
        self.assertTrue(absorbed)
        self.assertTrue(landed)
        for out in absorbed:
            self.assertEqual(out['auras'], [])  # Moonfire's DoT cannot get through an absorbed application.
            self.assertEqual(out['events'], [{'type': 'absorb', 'mana': out['cost']}])

    def test_atronach_incoming_absorption_and_physical_exclusion(self):
        req = {'op': 'damage_taken', 'school': 2, 'damage': 1000, 'victim_level': 60,
               'attacker_level': 60, 'seed': 1, 'caster': {**self.caster, 'actor_values': snapshot({83: 50})}}
        out = self.service.handle(req)
        self.assertEqual((out['damage'], out['absorbed']), (0, 250))
        import random
        self.assertFalse(native_values.absorbs({'actor_values': snapshot({83: 100})}, 0, random.Random(1)))

    def test_lord_target_changes_partial_resist_and_armor(self):
        lord = snapshot({39: 50, 44: 25})
        before, after = self.resolve(snapshot()), self.resolve(lord)
        self.assertLess(after['events'][0]['amount'], before['events'][0]['amount'])
        self.assertGreater(after['events'][0]['resisted'], 0)
        target = {**self.target, 'distance_yd': 2, 'in_front': False}
        req = {'op': 'melee', 'caster': {**self.caster, 'items': [873]}, 'target': target, 'seed': 3}
        before = self.service.handle(req)
        after = self.service.handle({**req, 'target': {**target, 'actor_values': lord}})
        self.assertGreater(before['damage'], after['damage'])

    def test_area_targets_use_live_resistance_and_absorption(self):
        target = {**self.target, 'id': 1}
        req = {'op': 'tick_area', 'spell': 17402, 'caster': self.caster, 'targets': [target], 'seed': 1}
        base = self.service.handle(req)['results'][0]
        vulnerable = self.service.handle({**req, 'targets': [{**target, 'actor_values': snapshot({44: -100})}]})['results'][0]
        self.assertGreater(vulnerable['amount'], base['amount'])
        absorbed = self.service.handle({**req, 'targets': [{**target, 'actor_values': snapshot({83: 100})}]})['results'][0]
        self.assertEqual(absorbed['type'], 'absorb')

    def test_incoming_weapon_proc_respects_target_absorption(self):
        req = {'op': 'incoming_hit', 'weapon_hit': True,
               'caster': {**self.caster, 'items': [17182]},
               'target': {**self.target, 'distance_yd': 2, 'actor_values': snapshot({83: 100})}, 'seed': 1}
        out = self.service.handle(req)
        shield = next(p for p in out['procs'] if p['spell'] == 21142)
        self.assertEqual(shield['events'], [{'type': 'absorb', 'mana': 0}])

    def test_external_snapshot_replaces_worn_enchantment_totals(self):
        caster = {**self.caster, 'actor_values': snapshot({41: 25, 156: 150}),
                  'equipment_modifiers': {'enchantments': [{'av': 41, 'magnitude': 25}, {'av': 156, 'magnitude': 50}]}}
        out = self.service.handle({'op': 'stats', 'caster': caster})
        self.assertEqual(out['resist']['2'], 100)
        self.assertEqual(out['regen']['per_sec'], self.stats(snapshot())['regen']['per_sec'] * 1.5)

    def test_owned_carriers_are_subtracted_before_conversion(self):
        base = self.stats(snapshot())
        # Own Innervate/equipment/racial resistance contributions must not feed back.
        data = snapshot({44: 10, 40: 10 / 3, 156: 500}, {44: 10, 40: 10 / 3, 156: 400})
        self.assertEqual(self.stats(data), base)

    def test_element_poison_disease_and_magic_combine_without_losing_values(self):
        data = native_values.snapshot(snapshot({44: 25, 41: 50, 42: 10, 43: 20, 40: 30, 45: 50}))
        self.assertAlmostEqual(native_values.resist_percent(data, 2), 62.5)
        self.assertAlmostEqual(native_values.resist_percent(data, 4), 40)
        self.assertAlmostEqual(native_values.resist_percent(data, 3), 32.5)        # Shock only: poison immunity is not Nature
        self.assertAlmostEqual(native_values.resist_percent(data, 3, 3), 66.25)    # a Disease spell meets disease resistance
        self.assertAlmostEqual(native_values.resist_percent(data, 3, 4), 52.75)    # a Poison spell meets poison resistance

    def test_zero_base_regeneration_leaves_the_wow_rate(self):
        data = native_values.snapshot(snapshot({28: 0}))
        data['base']['28'] = 0
        self.assertEqual(native_values.regen_multipliers(data)['magicka'], 1)

    def test_diagnostics_show_current_target_and_native_regeneration(self):
        target = {**self.target, 'actor_values': snapshot({44: 25, 155: 125, 157: 125})}
        out = self.service.handle({'op': 'native_defenses', 'caster': self.caster, 'target': target})
        self.assertAlmostEqual(out['target']['resist_percent']['2'], 25)
        self.assertEqual(out['target']['regen_multipliers'], {'health': 1.25, 'magicka': 1, 'stamina': 1.25})
        self.assertEqual(out['target']['actor_values'], target['actor_values'])

    def test_snapshot_validation_rejects_nonfinite_and_unknown_values(self):
        for value in (True, float('inf'), float('nan'), '25'):
            with self.assertRaises(ValueError):
                self.stats(snapshot({44: value}))
        data = copy.deepcopy(snapshot())
        data['live']['999'] = 1
        with self.assertRaises(ValueError):
            self.stats(data)
