"""Regressions from the Druid audit; CMaNGOS references are local src/game paths.

Native timer application/restoration is also asserted by wow_druid_gaps in SelfTest.
"""
import json
import unittest

from service import formulas as F
from service.catalog import Catalog
from service.server import Service

TARGET = {'kind': 'unit', 'level': 60, 'distance_yd': 2, 'reaction': 'hostile',
          'alive': True, 'id': 1, 'in_front': True, 'facing': True}
OMEN = {762: 5, 761: 1, 921: 4, 787: 3, 763: 5, 791: 5, 781: 3, 788: 1}
FERAL = {796: 5, 794: 5, 807: 2, 798: 3, 803: 3, 800: 2, 805: 2, 1162: 1, 799: 2}


class DruidGaps(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()

    def setUp(self):
        self.svc = Service(self.cat, None)

    def test_clearcasting_consumed_by_execution_of_every_maul_rank(self):
        # Spell.cpp Spell::cast/TakePower and SpellModRAII::RemoveSpellMods:
        # on-next-melee is paid/exhausts a charged cost mod when it fires.
        for sid in (6807, 6808, 6809, 8972, 9745, 9880, 9881):
            caster = {'level': 60, 'talents': OMEN, 'form': 'bear',
                      'power': {'rage': 0}, 'auras': [16870]}
            queued = self.svc.handle({'op': 'resolve', 'spell': sid, 'caster': caster, 'target': TARGET})
            self.assertTrue(queued['cost_deferred'])
            self.assertNotIn('consumed_auras', queued)
            for seed in (1, 31):  # consumption is execution, not a successful-hit proc
                out = self.svc.handle({'op': 'melee', 'seed': seed,
                                       'caster': {**caster, 'next_swing': sid}, 'target': TARGET})
                self.assertEqual((out['cost'], out['consumed_auras']), (0, [16870]))
                self.assertTrue(out['next_swing_consumed'])
            normal = self.svc.handle({'op': 'melee', 'caster': caster, 'target': TARGET})
            self.assertNotIn('consumed_auras', normal)

    def test_grasp_uses_manual_roots_native_budget_at_every_rank(self):
        # Spell.dbc Grasp EffectTriggerSpell2; SpellAuras.cpp Aura::TriggerSpell
        # resolves a Roots rank with the same damage aura as the manual spell.
        for skills in ({'alteration': 15}, {'alteration': 100}):
            for manual, grasp in zip((339, 1062, 5195, 5196, 9852, 9853),
                                     (16689, 16810, 16811, 16812, 16813, 17329)):
                caster = {'level': 60, 'talents': OMEN, 'skyrim': skills}
                cast = self.svc.handle({'op': 'resolve', 'spell': manual, 'seed': 1,
                                        'caster': caster, 'target': TARGET})
                proc = self.svc.handle({'op': 'incoming_hit', 'weapon_hit': True, 'seed': 1,
                                       'caster': {**caster, 'auras': [grasp]}, 'target': TARGET})['procs'][0]
                damage = lambda out: next(a for a in out['auras'] if a['type'] == 'damage')
                self.assertEqual(damage(proc)['per_tick'], damage(cast)['per_tick'])
                self.assertEqual(damage(proc)['ticks'], damage(cast)['ticks'])
                self.assertEqual(proc['tooltip']['health_factor'], cast['tooltip']['health_factor'])

    def test_empty_druid_dispels_fail_before_cost(self):
        # Spell.cpp Spell::CheckCast returns NOTHING_TO_DISPEL for pure dispels;
        # the core exempts multi-effect spells such as Abolish Poison (below).
        for sid, kind in ((2782, 'Curse'), (8946, 'Poison')):
            for target in ({'kind': 'self'}, {**TARGET, 'reaction': 'friendly', 'friendly': True}):
                req = {'op': 'resolve', 'spell': sid, 'caster': {'level': 60}, 'target': target}
                for available in ([], ['Disease']):
                    out = self.svc.respond(json.dumps({**req, 'target': {**target, 'dispellable': available}}).encode())
                    self.assertEqual(out['error'], 'SPELL_FAILED_NOTHING_TO_DISPEL')
                    self.assertFalse(out['ok'])
                    self.assertNotIn('cost', out)
                out = self.svc.handle({**req, 'target': {**target, 'dispellable': [kind]}})
                self.assertGreater(out['cost'], 0)
                self.assertEqual(out['events'][0]['dispel'], kind)

    def test_abolish_poison_casts_with_nothing_to_dispel(self):
        # Spell::CheckCast exempts Abolish Poison (dispel + periodic trigger 3137): it is cast and paid as in 1.12.
        req = {'op': 'resolve', 'spell': 2893, 'caster': {'level': 60}, 'target': {'kind': 'self', 'dispellable': []}}
        out = self.svc.handle(req)
        self.assertGreater(out['cost'], 0)

    def test_heart_of_the_wild_recomputes_from_base_without_rounding_drift(self):
        # Unit.cpp GetTotalStatValue / StatSystem.cpp Player::UpdateStats use
        # float32 then truncate. Audit: NE60 rank4 int 100 -> 115 (wrong), 116 (core).
        for rank in range(1, 6):
            for form, stat in (('none', 'int'), ('cat', 'str'), ('bear', 'sta'), ('dire_bear', 'sta')):
                base = self.svc.char({'level': 60, 'race': 4, 'talents': FERAL, 'form': form})
                caster = {'level': 60, 'race': 4, 'talents': {**FERAL, 808: rank}, 'form': form}
                expected = F.stat_total(base.stats[stat], 0, (), [4 * rank])
                for _ in range(3):
                    self.assertEqual(self.svc.char(caster).stats[stat], expected)
        self.assertEqual(self.svc.char({'level': 60, 'race': 4, 'talents': {**FERAL, 808: 4}}).stats['int'], 116)

    def test_bear_periodic_effects_require_and_end_with_form(self):
        # Spell.dbc Stances=Bear|DireBear; Aura::HandleShapeshiftBoosts removes
        # holders marked IsRemovedOnShapeLost, including both periodic payloads.
        for sid in (5229, 22842, 22895, 22896):
            for form in ('bear', 'dire_bear'):
                out = self.svc.handle({'op': 'resolve', 'spell': sid, 'target': {'kind': 'self'},
                                       'caster': {'level': 60, 'form': form, 'power': {'rage': 100}}})
                self.assertTrue(all(a['ends_on_form_loss'] for a in out['auras']))
            out = self.svc.respond(json.dumps({'op': 'resolve', 'spell': sid, 'target': {'kind': 'self'},
                                              'caster': {'level': 60, 'form': 'none'}}).encode())
            self.assertEqual(out['error'], 'SPELL_FAILED_ONLY_SHAPESHIFT')

    def test_periodic_dispel_and_resource_schedule_contract(self):
        # Spell.dbc: Abolish -> 3137 every 2000 ms for 8000 ms. Player.cpp
        # RegenerateAll/Regenerate: energy and rage use the 2-second power clock.
        out = self.svc.handle({'op': 'resolve', 'spell': 2893, 'caster': {'level': 60},
                               'target': {'kind': 'self', 'dispellable': ['Poison']}})
        aura = next(a for a in out['auras'] if a['type'] == 'periodic_dispel')
        self.assertEqual((aura['spell'], aura['duration_ms'], aura['amplitude_ms'], aura['count']),
                         (3137, 8000, 2000, 1))
        resources = self.svc.handle({'op': 'stats', 'caster': {'level': 60}})['resources']
        self.assertEqual((resources['energy_tick_ms'], resources['rage_decay_tick_ms']), (2000, 2000))
