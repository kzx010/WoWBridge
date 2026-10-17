"""Classic Night Elf rows, always-on passives and Skyrim conversions."""
import json
import random
import unittest

from data.build_catalog import RACIAL_SPELLS, build
from service import character, formulas as F
from service.catalog import Catalog
from service.server import Service
from service.skyrim_effects import ARMOR_BUFF, RESISTS


class Racials(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog(raw=build())

    def setUp(self):
        self.svc = Service(self.cat, None)

    def answer(self, req):
        return self.svc.respond(json.dumps(req).encode())

    def test_catalog_rows_and_visual(self):
        self.assertEqual(self.cat.raw['racials'], RACIAL_SPELLS)
        for sid in RACIAL_SPELLS.values():
            self.assertIn(sid, self.cat.spells)
        shadow = self.cat.spell(20580)
        self.assertEqual((shadow['SpellVisual'], shadow['AuraInterruptFlags']), (184, 48143))
        self.assertIn(str(shadow['SpellVisual']), self.cat.raw['visuals'])
        for sid in (20582, 20583, 20585):
            self.assertTrue(self.cat.spell(sid)['Attributes'] & 0x40)

    def test_shadowmeld(self):
        for native in (False, True):
            caster = {'in_combat': True, **({'skyrim': {}} if native else {})}
            out = self.answer({'op': 'resolve', 'spell': 20580, 'caster': caster, 'target': {'kind': 'self'}})
            self.assertTrue(out['ok'], out)
            self.assertEqual((out['cost'], out['cast_ms'], out['gcd_ms'], out['cooldown_ms']), (0, 0, 0, 10000))
            self.assertEqual(len(out['auras']), 1)
            a = out['auras'][0]
            self.assertEqual((a['type'], a['amount'], a['duration_ms'], a['interrupt_flags']), ('stealth', 295, -1, 48143))
            self.assertTrue(a['shadowmeld'])
            self.assertNotIn('prowling', a)   # does not switch the Cat bonus bar or grant Pounce/Ravage
            self.assertNotIn('ends_on_form_loss', a)

    def test_shadowmeld_validation(self):
        cases = [({'moving': True}, 'SPELL_FAILED_MOVING'),
                 ({'race': 1}, 'SPELL_FAILED_NOT_KNOWN'),
                 ({'form': 'cat'}, 'SPELL_FAILED_NOT_SHAPESHIFT')]
        for caster, error in cases:
            with self.subTest(caster=caster):
                out = self.answer({'op': 'resolve', 'spell': 20580, 'caster': caster, 'target': {'kind': 'self'}})
                self.assertEqual(out['error'], error)
        out = self.answer({'op': 'resolve', 'spell': 20580, 'target': {'kind': 'unit'}})
        self.assertEqual(out['error'], 'SPELL_FAILED_BAD_TARGETS')
        out = self.answer({'op': 'resolve', 'spell': 20580, 'caster': {'moving': 'yes'}, 'target': {'kind': 'self'}})
        self.assertEqual(out['error'], 'BAD_REQUEST: caster.moving must be a boolean')

    def test_passives_once_in_every_form(self):
        for form in ('none', 'cat', 'bear', 'dire_bear', 'travel', 'aquatic'):
            for auras in ([], [20582, 20582, 20583, 20583, 20585]):
                with self.subTest(form=form, auras=auras):
                    ch = character.build(self.cat, [], auras=auras, form=form)
                    self.assertEqual(ch.total(49), 1)
                    self.assertEqual(ch.resistance(3), 10)
                    self.assertEqual(ch.resistance(2), 0)
        stats = self.answer({'op': 'stats', 'caster': {'race': 1}})
        self.assertEqual((stats['dodge_bonus'], stats['resist']['3'], stats['racials']), (0, 0, []))

    def test_passive_metadata_and_native_equivalents(self):
        out = self.answer({'op': 'stats', 'caster': {'skyrim': {}}})
        self.assertEqual(out['dodge_bonus'], 1)
        self.assertEqual(out['resist']['3'], 10)
        ps = {p['spell']: p for p in out['racials']}
        self.assertEqual(set(ps), {20582, 20583, 20585})
        self.assertTrue(all(p['passive'] and p['duration_ms'] == -1 for p in ps.values()))
        self.assertEqual(ps[20582]['skyrim'][0]['mgef'], ARMOR_BUFF)
        self.assertAlmostEqual(ps[20582]['skyrim'][0]['magnitude'], 1 / .12)
        self.assertEqual(ps[20583]['skyrim'][0]['mgef'], RESISTS['nature_res'])
        self.assertAlmostEqual(ps[20583]['skyrim'][0]['magnitude'], 10 / 3)
        self.assertTrue(ps[20585]['informational'])
        self.assertEqual(ps[20585]['ghost_spell'], 20584)
        self.assertNotIn('skyrim', ps[20585])
        self.assertTrue(all('skyrim' not in p for p in self.answer({'op': 'stats'})['racials']))

    def test_passives_cannot_be_cast(self):
        for sid in (20582, 20583, 20585):
            out = self.answer({'op': 'resolve', 'spell': sid, 'target': {'kind': 'self'}})
            self.assertEqual(out['error'], 'SPELL_FAILED_NOT_KNOWN')

    def test_nature_resistance_stacks_with_gear_and_mark(self):
        naked = character.build(self.cat, [])
        mark = character.build(self.cat, [], auras=[9885, 20583, 20583])
        self.assertEqual((naked.resistance(3), mark.resistance(3), mark.resistance(2)), (10, 30, 20))
        geared = character.build(self.cat, [16897])
        self.assertEqual(geared.resistance(3), 10 + self.cat.item(16897).get('nature_res', 0))

    def test_mark_and_gift_of_the_wild_do_not_stack(self):
        # SpellStacking.cpp: one player family (7) with identical family flags (0x40000), both positive.
        from service import spells
        mark, gift = (spells.stack_group(self.cat.spell(sid)) for sid in (9885, 21850))
        self.assertEqual((mark, gift), ('7:40000', '7:40000'))
        self.assertEqual(spells.stack_group(self.cat.spell(9910)), '7:100')   # Thorns stacks with both
        one = character.build(self.cat, [], auras=[21850])
        both = character.build(self.cat, [], auras=[9885, 21850, 9910])
        self.assertEqual((both.resistance(3), [a['spell'] for a in both.auras if a['spell'] in (9885, 21850)][:1]),
                         (one.resistance(3), [21850]))

    def test_stack_groups_need_a_similar_aura_effect_and_keep_the_stronger(self):
        # IsStackableAuraEffect only reaches the multirank rule for similar effects (effect, aura, sign):
        # Shadowform (36/79/87) and Fear Ward (77) share family flags but no effect, so both stay.
        both = character.build(self.cat, [], auras=[15473, 6346], cls=5)
        self.assertEqual(sorted({a['spell'] for a in both.auras if a['spell'] in (15473, 6346)}), [6346, 15473])
        # IsSimilarExistingAuraStronger: Mark rank 7 (285 armor) outlasts Gift rank 1 (240) on an old save.
        kept = character.build(self.cat, [], auras=[21849, 9885])
        self.assertEqual(sorted({a['spell'] for a in kept.auras if a['spell'] in (21849, 9885)}), [9885])
        out = self.svc.handle({'op': 'resolve', 'spell': 21849, 'seed': 1, 'target': {'kind': 'self'},
                               'caster': {'level': 60, 'talents': {}, 'reagents': {'17021': 1}}})
        self.assertEqual(out['stack_amounts'], {'6:22:+': 240, '6:29:+': 10, '6:143:+': 15})

    def test_incoming_resist_roll_uses_her_nature_resistance(self):
        req = {'op': 'damage_taken', 'damage': 400, 'victim_level': 60, 'school': 3, 'attacker_level': 60}
        # Pick a seeded partial resistance, then check the port's exact roll and unchanged health conservation.
        pct = F.effective_resist_percent(60, 60, 10, False, False)
        seed = next(i for i in range(1000) if F.partial_resist_multiplier(pct, False, False, False, random.Random(i)) > 0)
        expected = int(400 * F.partial_resist_multiplier(pct, False, False, False, random.Random(seed)))
        out = self.answer({**req, 'seed': seed})
        self.assertTrue(out['ok'], out)
        self.assertEqual((out['resistance'], out['resisted'], out['damage']), (10, expected, 400 - expected))
        other = self.answer({**req, 'seed': seed, 'caster': {'race': 1}})
        self.assertEqual((other['resisted'], other['damage']), (0, 400))
        fire = self.answer({**req, 'school': 2, 'seed': seed})
        self.assertEqual((fire['resisted'], fire['damage']), (0, 400))
        legacy = self.answer({k: v for k, v in req.items() if k not in ('school', 'attacker_level')})
        self.assertEqual(set(legacy), {'id', 'ok', 'break', 'chance'})

    def test_binary_resistance_and_malformed_school(self):
        req = {'op': 'damage_taken', 'damage': 100, 'victim_level': 60, 'school': 3, 'binary': True}
        results = [self.answer({**req, 'seed': seed})['resisted'] for seed in range(200)]
        self.assertEqual(set(results), {0, 100})
        for school in (0, 7, True, '3'):
            out = self.answer({**req, 'school': school})
            self.assertEqual(out['error'], 'BAD_REQUEST: school must be a magical school from 1 to 6')
