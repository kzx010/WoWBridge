"""Counterpart budgets, complete slice coverage and unchanged CMaNGOS outcomes."""
import copy
import json
from pathlib import Path
import unittest

from service import character, feral, formulas as F, skyrim, spells
from service.catalog import Catalog
from service.server import Service
from service.skyrim_balance import COUNTERPARTS, SPELLS
from service.tests import test_caster_book, test_feral
from service.tests.test_service import FULL, HOSTILE

# Independent expected magicka spent at skill 15 / 50 / 100, without perks.
COSTS = {
    9912: (36, 30, 24), 9835: (72, 61, 49), 25298: (72, 61, 49),
    17402: (488, 410, 328), 9853: (241, 203, 163), 25299: (64, 54, 43),
    9858: (91, 76, 61), 25297: (111, 93, 75), 9863: (112, 94, 75),
    20748: (882, 741, 594), 18658: (172, 144, 116), 9901: (32, 27, 22),
    9907: (91, 76, 61), 22812: (23, 19, 15), 29166: (11, 9, 7),
    9885: (91, 76, 61), 21850: (245, 206, 165), 9910: (255, 214, 172),
    2782: (64, 54, 43), 8946: (11, 9, 7), 2893: (53, 44, 36),
    5487: (91, 76, 61), 9634: (86, 72, 58), 768: (45, 38, 31),
    783: (34, 29, 23), 1066: (196, 165, 132),
}
HEALTH = {9912: 35/276, 9835: 70/611, 25298: 75/584, 17402: 240/1340,
          9853: 24/270, 25299: 75/888, 9858: 110/2183, 25297: 150/2677,
          9863: 250/1470, 20748: 100/2200, 9910: 8/18,
          5487: 20/560, 9634: 40/1240, 22896: 10/200}
WEAPON_ABILITIES = {9881, 9908, 9850, 9830, 9904, 9896, 31018, 9827, 9867, 9846}
HEALTH.update({sid: 7/96.5 for sid in WEAPON_ABILITIES})
OFFENSIVE = list(range(16795, 16803)) + [18842]  # Arcanist and Staff of Dominance


def native_tooltip_fixture(cat):
    """Native Lua consumes these real service profiles and resolved health amounts."""
    svc = Service(cat, None)
    cases = []
    for sid, anchor, text in (
            (9910, 9910, '$s1'), (9881, 9881, '$s1'), (9846, 9846, '$s1'),
            (9835, 9835, '$s2 / $o1'), (20748, 20748, '$s1 / $q'),
            (22896, 22896, '$s1'), (22842, 22896, '$s1'),
            (5487, 5487, '$1178s2'), (9634, 9634, '$9635s2'),
            (9827, 9827, '$9826o1')):
        req = request_for(cat, anchor, seed=1)
        req['spell'] = sid
        req['caster'].update(talents={}, skyrim={})
        out = svc.handle(req)
        if sid == 9881:
            amount = out['events'][0]['bonus_damage']
        elif sid == 9835:
            rolls = [svc.handle({**req, 'seed': seed}) for seed in range(128)]
            direct = [e['amount'] for cast in rolls for e in cast['events']
                      if e['type'] == 'damage' and not e['crit'] and not e['resisted']]
            amount = f"{min(direct)} to {max(direct)} / {sum(t['amount'] for t in out['auras'][0]['ticks'])}"
        elif sid == 20748:
            amount = f"{out['events'][0]['health']} / {cat.spell(sid)['EffectMiscValue1']}"
        elif sid in (5487, 9634):
            amount = out['auras'][0]['skyrim_health_bonus']
        elif sid in (22896, 22842):
            amount = out['auras'][0]['health_per_rage']
        elif sid == 9827:
            amount = sum(t['amount'] for t in out['auras'][1]['ticks'])
        else:
            amount = out['auras'][0]['amount']
        cases.append({'spell': sid, 'profile': out['tooltip'], 'text': text,
                      'actual': str(amount).removesuffix('.0')})

    def lua(value):
        if isinstance(value, dict):
            return '{' + ','.join(f'[{json.dumps(str(k))}]={lua(v)}' for k, v in value.items()) + '}'
        if isinstance(value, list):
            return '{' + ','.join(lua(v) for v in value) + '}'
        return json.dumps(value)
    return '-- Service snapshots verified by test_native_tooltip_fixture_matches_actual_service.\nreturn ' + lua(cases) + '\n'


def request_for(cat, sid, items=(), seed=11):
    """Valid state for each slice role, shared by the documentation examples."""
    if sid in feral.ABILITIES | feral.FORM_SPELLS.keys():
        req = test_feral.Feral.req(None, sid, seed=seed)
    elif sid in test_caster_book.BOOK.values():
        name = next(n for n, i in test_caster_book.BOOK.items() if i == sid)
        req = test_caster_book.CasterBook.request(None, name, seed=seed)
    else:
        target = {'kind': 'ground', 'distance_yd': 20} if sid == 17402 else (
            {'kind': 'self'} if sid in (25299, 25297) else dict(HOSTILE))
        req = {'op': 'resolve', 'spell': sid, 'seed': seed, 'target': target, 'caster': {}}
    req['caster']['items'] = list(items)
    return req


class SkyrimBalance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()

    def setUp(self):
        self.svc = Service(self.cat, None)

    def answer(self, req):
        out = self.svc.respond(json.dumps(req).encode())
        self.assertTrue(out['ok'], out)
        return out

    def test_native_tooltip_fixture_matches_actual_service(self):
        fixture = Path(__file__).parent / 'fixtures/skyrim_health_tooltips.lua'
        self.assertEqual(fixture.read_text(), native_tooltip_fixture(self.cat))
        # The interface's test (wowcore test_wowapi.cpp) reads its own copy: the same snapshots.
        self.assertEqual((Path(__file__).parents[2] / 'wowcore/tests/fixtures/skyrim_health_tooltips.lua').read_text(),
                         fixture.read_text())

    def test_all_caster_ranks_advance_with_and_without_strong_gear(self):
        # Rolls stay independent and uniform per rank; across many seeds a higher rank's
        # noncritical range and mean must not fall below a lower rank's, nor its cost.
        for top in (9912, 9835, 25298, 17402, 25299, 9858, 25297, 9863):
            ids = sorted((sid for sid, row in self.cat.ranks.items() if row['chain'] == self.cat.chain(top)),
                         key=lambda sid: self.cat.ranks[sid]['rank'])
            self.assertGreater(len(ids), 1)
            for items in ((), OFFENSIVE, FULL):
                lows, highs, means, costs = [], [], [], []
                for sid in ids:
                    totals = []
                    for seed in range(128):
                        req = request_for(self.cat, top, items, seed)
                        req['spell'] = sid
                        req['caster']['skyrim'] = {'destruction': 50, 'restoration': 50}
                        out = self.answer(req)
                        if any(e.get('crit') or e.get('resisted') for e in out['events']):
                            continue
                        total = sum(e['amount'] for e in out['events'] if e['type'] in ('damage', 'heal'))
                        total += sum(t['amount'] for a in out['auras'] if a['type'] in ('damage', 'heal')
                                     for t in a.get('ticks', []))
                        if top in (17402, 9863):
                            tick = self.answer({'op': 'tick_area', 'spell': sid, 'seed': seed,
                                                'caster': req['caster'],
                                                'targets': [{'id': 1, 'level': 60, 'friendly': top == 9863}]})
                            total = sum(e['amount'] for e in tick['results'] if e['type'] in ('damage', 'heal'))
                            self.assertNotIn('cost', tick)
                        totals.append(total)
                    self.assertTrue(totals)
                    lows.append(min(totals)); highs.append(max(totals)); means.append(sum(totals) / len(totals))
                    costs.append(out['cost'])
                with self.subTest(top=top, items=items):
                    for k in range(len(ids) - 1):
                        self.assertLessEqual(lows[k], highs[k + 1], (ids[k], ids[k + 1]))   # never wholly above
                        self.assertLessEqual(means[k], means[k + 1] + 1e-9, (ids[k], ids[k + 1]))
                        self.assertLessEqual(highs[k], highs[k + 1], (ids[k], ids[k + 1]))
                    self.assertEqual(costs, sorted(costs))
                    self.assertGreater(costs[0], 0)

    def test_rank_bonus_conversion_is_shared_and_wow_cache_is_unchanged(self):
        from service.talent_effects import tooltip
        for top in (9912, 9835, 25298, 17402, 25299, 9858, 25297, 9863):
            for sid, row in self.cat.ranks.items():
                if row['chain'] != self.cat.chain(top):
                    continue
                rule = self.cat.balance[sid]
                spell = self.cat.spell(sid)
                wow = self.svc.char({'items': OFFENSIVE, 'talents': {}})
                native = self.svc.char({'items': OFFENSIVE, 'talents': {}, 'skyrim': {}})
                bare = self.svc.char({'talents': {}})
                for e in (1, 2, 3):
                    if spell[f'Effect{e}'] not in (2, 10) and spell[f'EffectApplyAuraName{e}'] not in (3, 8):
                        continue
                    baseline = tooltip(self.cat, bare, spell)['effects'][e-1]
                    original = tooltip(self.cat, wow, spell)['effects'][e-1]
                    adjusted = tooltip(self.cat, native, spell)['effects'][e-1]
                    for b, w, n in zip(baseline, original, adjusted):
                        self.assertAlmostEqual((n-b)*rule.health_factor, (w-b)*SPELLS[top].health_factor)
                self.assertIs(wow, self.svc.char({'items': OFFENSIVE, 'talents': {}}))
                self.assertFalse(wow.health_bonus_scales)

    def test_entire_slice_cost_health_and_wow_compatibility(self):
        self.assertEqual(set(self.cat.slice.values()), set(SPELLS))
        self.assertLessEqual({r.skill for r in SPELLS.values()}, set(skyrim.SKILLS))
        self.assertEqual(set(COSTS), set(self.cat.slice.values()) - feral.ABILITIES)
        for sid in self.cat.slice.values():
            for items in ((), FULL):
                for seed in (1, 11, 31):
                    req = request_for(self.cat, sid, items, seed)
                    original_request = copy.deepcopy(req)
                    wow = self.answer(req)
                    # The Skyrim wrapper is a true no-op without caster.skyrim.
                    ch = self.svc.char(req['caster'])
                    expected_wow = spells.resolve(self.cat, ch, req)
                    self.assertEqual(wow, {'id': None, 'ok': True, **expected_wow})
                    self.assertEqual(req, original_request)
                    for index, level in enumerate((15, 50, 100)):
                        req['caster']['skyrim'] = {k: level for k in skyrim.SKILLS.values()}
                        scaled = self.answer(req)
                        with self.subTest(spell=sid, skill=level, seed=seed, gear=bool(items)):
                            self.assertEqual(scaled['cost'], COSTS[sid][index] if sid in COSTS else wow['cost'])
                            self.assertEqual(scaled['skill'], SPELLS[sid].skill)
                            for key in wow.keys() - {'cost', 'events', 'auras', 'results'}:
                                self.assertEqual(wow[key], scaled[key])
                            factor = HEALTH.get(sid, 0) * (1 + .005 * level if sid in WEAPON_ABILITIES else 1)
                            for key in ('events', 'auras', 'results'):
                                for a, b in zip(wow.get(key, []), scaled.get(key, [])):
                                    expected = copy.deepcopy(a)
                                    if a['type'] in ('damage', 'heal', 'damage_shield', 'damage_done'):
                                        for field in ('amount', 'per_tick', 'resisted', 'absorbed', 'blocked'):
                                            if field in expected:
                                                expected[field] = round(expected[field] * factor)
                                        for tick in expected.get('ticks', []):
                                            for field in ('amount', 'resisted', 'absorbed'):
                                                if field in tick:
                                                    tick[field] = round(tick[field] * factor)
                                    elif a['type'] == 'next_swing':
                                        expected['bonus_damage'] = round(a['bonus_damage'] * factor)
                                    elif a['type'] == 'resurrect':
                                        expected.update(health=100, mana=75)
                                    elif a['type'] == 'shapeshift':
                                        expected['skyrim_health_bonus'] = round(a['health_bonus'] * factor)
                                        expected['skyrim_armor_percent'] = {'bear': 50, 'dire_bear': 100}.get(a['form'], 0)
                                    elif a['type'] == 'rage_heal':
                                        expected['health_per_rage'] *= factor
                                    self.assertEqual({k: v for k, v in b.items() if k != 'skyrim'}, expected)
                        req['caster'].pop('skyrim')
                        self.assertEqual(self.answer(req), wow)  # no cache contamination

    def test_tooltip_conversion_matches_native_damage_and_healing_budgets(self):
        for sid in (9835, 5176, 5185, 774, 9858):
            req = {'op': 'resolve', 'spell': sid, 'seed': 1,
                   'caster': {'items': FULL, 'talents': {}, 'skyrim': {'destruction': 30, 'restoration': 30}},
                   'target': dict(HOSTILE, friendly=sid in (5185, 774, 9858))}
            out = self.svc.respond(json.dumps(req).encode())
            self.assertTrue(out['ok'], out)
            rule = self.cat.balance[sid]
            self.assertEqual(out['tooltip']['health_factor'], rule.health_factor)
            self.assertEqual(out['tooltip']['minimum_health'], rule.minimum_health)
            spell = self.cat.spell(sid)
            for event in out.get('events', []):
                if event['type'] not in ('damage', 'heal') or event.get('crit') or event.get('resisted'):
                    continue
                effect = next(i for i in (1, 2, 3) if spell[f'Effect{i}'] == (2 if event['type'] == 'damage' else 10))
                expected = [round(v * rule.health_factor) for v in out['tooltip']['effects'][effect - 1]]
                if rule.minimum_health:
                    expected = [max(1, v) for v in expected]
                self.assertGreaterEqual(event['amount'], expected[0])
                self.assertLessEqual(event['amount'], expected[1])

    def test_training_spend_matches_native_cast_budgets(self):
        # Compare actual API cost to the counterpart's cost, not merely to the
        # conversion factor. Repeated channels train once, at cast application.
        for sid, counterpart, units in (
                (9912, 'firebolt', 1), (25298, 'firebolt', 2), (17402, 'blizzard', .5),
                (25297, 'close_wounds', 1),
                (25299, 'fast_healing', 1), (9863, 'grand_healing', .5),
                (20748, 'dead_thrall', 1), (9885, 'oakflesh', 1), (18658, 'calm', 40/30)):
            for skill in (15, 50, 100):
                req = request_for(self.cat, sid)
                req['caster']['skyrim'] = {k: skill for k in skyrim.SKILLS.values()}
                cast = self.answer(req)
                native_spend = COUNTERPARTS[counterpart]['cost'] * units * (1 - (skill/400)**.65)
                self.assertLessEqual(abs(cast['cost'] - native_spend), .5)
        req = request_for(self.cat, 9858)
        req['caster']['skyrim'] = {'restoration': 50}
        native_regrowth = (COUNTERPARTS['fast_healing']['cost'] +
                           2.5 * COUNTERPARTS['healing']['cost']) * (1 - (50/400)**.65)
        self.assertLessEqual(abs(self.answer(req)['cost'] - native_regrowth), .5)
        for sid in (17402, 9863):
            tick = self.answer({'op': 'tick_area', 'spell': sid, 'tick': 2,
                                'targets': [{'id': 1, 'level': 60, 'friendly': sid == 9863}],
                                'caster': {'skyrim': {}}})
            self.assertNotIn('cost', tick)

    def test_healing_order_and_damage_do_not_grow_with_skill(self):
        for skill in (15, 50, 100):
            totals = []
            for sid in (25297, 9858, 25299):
                req = request_for(self.cat, sid, seed=1)
                req['caster']['skyrim'] = {'restoration': skill}
                out = self.answer(req)
                totals.append(sum(e['amount'] for e in out['events']) +
                              sum(a['per_tick'] * len(a['ticks']) for a in out['auras']))
            self.assertGreater(totals[0], totals[1])
            self.assertGreater(totals[1], totals[2])
            heals = []
            for sid in (25297, 9858):
                req = request_for(self.cat, sid, seed=1)
                req['caster']['skyrim'] = {'restoration': skill}
                heals.append(self.answer(req)['events'][0]['amount'])
            self.assertGreater(heals[0], heals[1])
        for sid in (9912, 25298, 25297, 9858, 22896):
            outcomes = []
            for skill in (15, 50, 100):
                req = request_for(self.cat, sid, seed=1)
                req['caster']['skyrim'] = {k: skill for k in skyrim.SKILLS.values()}
                out = self.answer(req)
                outcomes.append((out['events'], out['auras']))
            self.assertEqual(outcomes[0], outcomes[1])
            self.assertEqual(outcomes[1], outcomes[2])

    def test_defaults_clamps_cost_modifiers_and_form_exit(self):
        req = request_for(self.cat, 18658)
        req['caster']['skyrim'] = {}
        self.assertEqual(self.answer(req)['cost'], 144)  # missing Alteration defaults to 50
        req['caster']['skyrim'] = {'alteration': -10}
        self.assertEqual(self.answer(req)['cost'], 195)
        req['caster']['skyrim'] = {'alteration': 1000}
        self.assertEqual(self.answer(req)['cost'], 116)
        for sid, spell in self.cat.spells.items():
            if sid in COSTS:
                ch = character.build(self.cat, [])
                self.assertEqual(spells._cost(spell, ch), SPELLS[sid].wow_cost)
        # A completed cost modifier must remain effective after normalization.
        out = {'spell': 9912, 'cost': 90, 'events': []}
        self.assertEqual(skyrim.convert(out, {'skyrim': {'destruction': 50}}, 'Destruction')['cost'], 15)
        for sid, form in feral.FORM_SPELLS.items():
            req = request_for(self.cat, sid)
            req['caster'].update(form=form, skyrim={})
            self.assertEqual(self.answer(req)['cost'], 0)

    def test_wow_health_anchors_are_catalog_maximums(self):
        ch = character.build(self.cat, [])
        for sid in (9912, 9835, 25298, 17402, 9853, 25299, 9858, 25297, 9863, 20748, 9910):
            spell = self.cat.spell(sid)
            budget = 0
            for eff in spells._effects(spell):
                effect, aura = spell[f'Effect{eff}'], spell[f'EffectApplyAuraName{eff}']
                base = int(F.effect_value(spell, eff, 60, None, maximum=True))
                if effect in (2, 10, 113) or aura == 15:
                    budget += base
                elif aura in (3, 8):
                    ticks = self.cat.duration(spell, 60) // spell[f'EffectAmplitude{eff}']
                    budget += base * ticks
            self.assertEqual(SPELLS[sid].wow_health, budget, SPELLS[sid].name)
        cat = character.build(self.cat, [], form='cat')
        low, high = cat.weapon_damage_range()
        self.assertEqual((low + high) / 2, 96.5)
        for sid, form in ((5487, 'bear'), (9634, 'dire_bear')):
            ch = character.build(self.cat, [], form=form)
            self.assertEqual(feral.profile(self.cat, ch)['health_bonus'], SPELLS[sid].wow_health)

    def test_pinned_counterparts_match_inventory(self):
        inventory = Path(__file__).resolve().parents[2] / 'build/skyrim_icons/inventory.json'   # data/skyrim_magic.py's output
        if not inventory.exists():
            self.skipTest('Skyrim inventory not available; runtime uses pinned balance data')
        rows = {r['key']: r for r in json.loads(inventory.read_text())['forms']}
        for name, native in COUNTERPARTS.items():
            if name == 'iron_sword':
                continue  # WEAP was read directly from the plugin DATA subrecord
            row = rows[native['form']]
            self.assertEqual(native['cost'], row.get('base_cost', 0), name)
            self.assertEqual(native['source'], row['source_plugin'], name)
            effect = next(e for e in row['effects'] if e['form'] == native['effect'])
            for field in ('magnitude', 'duration', 'area'):
                self.assertEqual(native[field], effect[field], name)
            self.assertEqual(native['minimum_skill'], rows[effect['form']]['minimum_skill'], name)
            self.assertEqual(native['school'], rows[effect['form']]['school'], name)


if __name__ == '__main__':
    unittest.main()


class SkyrimCosts(unittest.TestCase):
    def test_costs_match_resolve(self):
        """The interface's costs (op skyrim_costs) are what resolve charges, at her skills."""
        from service import server
        from service.catalog import Catalog, DEFAULT
        srv = server.Service(Catalog(DEFAULT), None)
        caster = {'skyrim': {'destruction': 30, 'restoration': 60, 'alteration': 15}}
        costs = srv.handle({'op': 'skyrim_costs', 'caster': caster})['costs']
        out = srv.handle({'op': 'resolve', 'spell': 25297, 'caster': caster, 'target': {'kind': 'self'}})
        self.assertEqual(costs['25297'], out['cost'])
