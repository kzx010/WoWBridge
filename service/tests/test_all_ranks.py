"""Progression-to-combat coverage at each rank's own CMaNGOS level."""
import copy
import json
from pathlib import Path
import unittest

from data.build_catalog import build, REPO
from service import character, feral, skyrim
from service.catalog import Catalog
from service.progression import Progression
from service.server import Service
from service.skyrim_balance import SPELLS
from service.tests.test_service import FULL
from service.tests.test_skyrim_balance import request_for


def value_bounds(s, e, level):
    """Naked rank roll interval from Object.cpp:CalculateSpellEffectValue."""
    delta = max(s['BaseLevel'], min(level, s['MaxLevel'] or level)) - s['SpellLevel']
    base = s[f'EffectBasePoints{e}']
    dice = s[f'EffectDieSides{e}']
    if s[f'EffectRealPointsPerLevel{e}']:
        base += delta * s[f'EffectRealPointsPerLevel{e}']
        dice += int(delta * s[f'EffectDicePerLevel{e}'])
    lo, hi = sorted((s[f'EffectBaseDice{e}'], dice)) if dice not in (0, 1) else (s[f'EffectBaseDice{e}'],) * 2
    return int(base + lo), int(base + hi)


class AllRanks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Build in memory: workers need not mutate the shared build artifact to
        # exercise every rank. The CLI and this fixture call the same builder.
        cls.raw = build()
        cls.cat = Catalog(raw=cls.raw)
        cls.old = Catalog()
        cls.progression = Progression()
        data = json.loads((REPO / 'data/progression.json').read_text())
        definition = data['classes']['druid']
        cls.rows = definition['spells'] + [r for r in definition['excluded'] if r['source'] != 'talent']
        cls.ff = [r for r in definition['excluded'] if r['chain'] == 16857]
        cls.by_id = {r['spell']: r for r in cls.rows + cls.ff}

    def setUp(self):
        self.svc = Service(self.cat, None, self.progression)

    def request(self, row, seed=11):
        sid = row['spell']
        anchor = next((i for i in self.old.slice.values() if self.cat.chain(i) == row['chain']), None)
        if sid in self.old.slice.values():
            anchor = sid  # Bear and Dire Bear have distinct form rules in one chain.
        if anchor:
            req = request_for(self.old, anchor, seed=seed)
        else:
            req = {'op': 'resolve', 'seed': seed, 'caster': {}, 'target': {'kind': 'self'}}
            req['caster']['form'] = 'bear' if sid == 5209 else 'cat'
            req['caster']['power'] = {'rage': 100, 'energy': 100}
        req['spell'] = sid
        req['caster']['level'] = row['required_level']
        s = self.cat.spell(sid)
        req['caster']['reagents'] = {str(s[f'Reagent{e}']): s[f'ReagentCount{e}']
                                     for e in range(1, 9) if s[f'Reagent{e}'] > 0}
        if req['target']['kind'] == 'unit':
            req['target']['level'] = row['required_level']
        return req

    def answer(self, req):
        out = self.svc.respond(json.dumps(req).encode())
        self.assertTrue(out['ok'], out)
        return out

    def test_tracking_uses_dbc_mask_form_lifetime_and_cat_entry_trigger(self):
        req = self.request(self.by_id[5225])
        out = self.answer(req)
        a, = out['auras']
        self.assertEqual((a['aura'], a['type'], a['misc'], a['mask']), (44, 'track_creatures', 7, 0x40))
        self.assertEqual((a['duration_ms'], a['stack']), (-1, 'replace_tracking'))
        self.assertTrue(a['ends_on_form_loss'])
        req['caster']['form'] = 'none'
        self.assertEqual(self.svc.respond(json.dumps(req).encode())['error'], 'SPELL_FAILED_ONLY_SHAPESHIFT')
        req = self.request(self.by_id[768])
        req['caster']['form'] = 'none'
        entered = self.answer(req)
        self.assertEqual(entered['form_tracking']['spell'], 5225)
        self.assertEqual(entered['form_tracking']['auras'][0], a)
        req['caster']['form'] = 'cat'
        self.assertNotIn('form_tracking', self.answer(req))

    def test_catalog_covers_progression_assets_triggers_and_original_rows(self):
        expected = {r['spell'] for definition in self.progression.data['classes'].values()
                    for r in definition['spells'] + definition['excluded']}
        self.assertEqual(set(self.cat.ranks), expected)
        druid = self.progression.data['classes']['druid']
        self.assertEqual(len(druid['spells'] + druid['excluded']), 344)
        for sid, row in self.cat.ranks.items():
            with self.subTest(spell=sid):
                s = self.cat.spell(sid)
                self.assertIn(sid, self.cat.balance)
                for field, table in (('CastingTimeIndex', 'cast_times'), ('DurationIndex', 'durations'),
                                     ('RangeIndex', 'ranges'), ('SpellIconID', 'icons'), ('SpellVisual', 'visuals')):
                    if s[field]:
                        self.assertIn(s[field], self.raw[table])
                for e in (1, 2, 3):
                    if s[f'EffectRadiusIndex{e}']:
                        self.assertIn(s[f'EffectRadiusIndex{e}'], self.raw['radii'])
                    if s[f'EffectTriggerSpell{e}']:
                        self.assertIn(s[f'EffectTriggerSpell{e}'], self.cat.spells)
        for section in ('spells', 'items', 'cast_times', 'durations', 'ranges', 'radii', 'icons', 'visuals',
                        'reagents', 'item_sets', 'creatures'):
            for key, value in self.old.raw[section].items():
                self.assertEqual(self.cat.raw[section].get(key), value, (section, key))
        for section in ('spell_affect', 'spell_proc_event', 'spell_facing'):
            for row in self.old.raw[section]:
                self.assertIn(row, self.raw[section])

    def test_every_known_pending_and_quest_rank_resolves_at_its_level(self):
        acquired = [r['spell'] for r in self.rows if r['source'] in ('quest', 'item')]
        novice = self.svc.handle({'op': 'known_spells', 'class': 'druid', 'caster': {'skyrim': {}}})
        master = self.svc.handle({'op': 'known_spells', 'class': 'druid',
                                'caster': {'skyrim': {k: 100 for k in skyrim.SKILLS.values()}},
                                'acquired_spells': acquired})
        self.assertEqual(set(master['known_spells']), {r['spell'] for r in self.rows})
        pending = {r['spell'] for r in novice['unlearned_spells']}
        self.assertLessEqual(pending, set(master['known_spells']))
        for row in self.rows + self.ff:
            sid, level = row['spell'], row['required_level']
            with self.subTest(spell=sid, level=level):
                req = self.request(row)
                out = self.answer(req)
                s = self.cat.spell(sid)
                ch = self.svc.char(req['caster'])
                self.assertEqual(ch.base_mana, self.raw['character']['classlevelstats_by_level'][level]['basemana'])
                rank_level = min(level, s['MaxLevel'] or level)
                cost = s['ManaCost'] + s['ManaCostPerlevel'] * (rank_level - s['BaseLevel'])
                cost += s['ManaCostPercentage'] * ch.base_mana // 100
                if s['PowerType'] == 1:
                    cost /= 10
                self.assertEqual(out['cost'], cost)
                self.assertEqual((out['name'], out['rank'], out['icon']), (s['SpellName'], s['Rank1'], self.cat.icon(s)))
                self.assertEqual(out['cast_ms'], self.cat.cast_time(s))
                # Naked caster ranks: each direct/periodic health amount must
                # fall in the source rank's roll interval (including its crit).
                if feral.ability(self.cat, sid) is None:
                    for event in out['events']:
                        if event['type'] in ('damage', 'heal'):
                            e = next(i for i in (1, 2, 3) if s[f'Effect{i}'] == (2 if event['type'] == 'damage' else 10))
                            lo, hi = value_bounds(s, e, level)
                            mult = 1.5 if event['crit'] else 1
                            self.assertGreaterEqual(event['amount'], int(lo * mult))
                            self.assertLessEqual(event['amount'], int(hi * mult))
                for aura in out['auras']:
                    if aura['type'] in ('heal', 'damage'):
                        if feral.ability(self.cat, sid) is None:
                            lo, hi = value_bounds(s, aura['eff'], level)
                            self.assertGreaterEqual(aura['per_tick'], lo)
                            self.assertLessEqual(aura['per_tick'], hi)
                        if 'ticks' in aura:
                            self.assertEqual(len(aura['ticks']), aura['duration_ms'] // aura['amplitude_ms'])
                req['caster']['skyrim'] = {k: 15 for k in skyrim.SKILLS.values()}
                converted = self.answer(req)
                self.assertEqual(converted['skill'], self.cat.balance[sid].skill)
                costs = self.svc.handle({'op': 'skyrim_costs', 'caster': req['caster']})['costs']
                self.assertEqual(costs[str(sid)], converted['cost'])

    def test_max_rank_combat_replies_are_unchanged(self):
        old_svc = Service(self.old, None)
        for sid in self.old.slice.values():
            for gear in ((), FULL):
                for seed in (1, 11, 31):
                    for skill in (None, 15, 50, 100):
                        with self.subTest(spell=sid, gear=gear, seed=seed, skill=skill):
                            req = request_for(self.old, sid, gear, seed)
                            if skill is not None:
                                req['caster']['skyrim'] = {k: skill for k in skyrim.SKILLS.values()}
                            self.assertEqual(self.svc.handle(req), old_svc.handle(req))

    def test_rank_prices_sizes_and_native_buffs_scale_from_maximum(self):
        for row in self.rows + self.ff:
            sid = row['spell']
            req = self.request(row)
            wow = self.answer(req)
            req['caster']['skyrim'] = {k: 15 for k in skyrim.SKILLS.values()}
            scaled = self.answer(req)
            rule = self.cat.balance[sid]
            with self.subTest(spell=sid):
                if wow.get('power', 'mana') == 'mana' and rule.cost:
                    self.assertEqual(scaled['cost'], max(1, round(rule.base_cost * skyrim.cost_factor(15) * wow['cost'] / rule.wow_cost)))
                else:
                    self.assertEqual(scaled['cost'], wow['cost'])
                for kind in ('events', 'auras'):
                    for a, b in zip(wow[kind], scaled[kind]):
                        if a['type'] in ('damage', 'heal', 'damage_shield'):
                            for field in ('amount', 'per_tick'):
                                if field in a:
                                    self.assertEqual(b[field], max(1, round(a[field] * rule.health_factor)) if rule.minimum_health and a[field] > 0 and rule.health_factor > 0 else round(a[field] * rule.health_factor))
                        if a['type'] == 'armor' and sid not in SPELLS:
                            anchor = next((i for i in SPELLS if self.cat.chain(i) == row['chain']), None)
                            if anchor:
                                native = SPELLS[anchor].native or SPELLS[SPELLS[anchor].native_from].native
                                expected = [abs(a['amount']) * target / reference for _, target, reference in native['armor']]
                                self.assertEqual([e['magnitude'] for e in b['skyrim']], expected)
        # These are novice-sized budgets, rather than full maximum-rank casts.
        for sid, cost, health in ((5176, 12, 8), (5185, 14, 16), (8921, 22, 6), (774, 10, 3)):
            req = self.request(self.by_id[sid], seed=1)
            req['caster']['skyrim'] = {k: 15 for k in skyrim.SKILLS.values()}
            out = self.answer(req)
            self.assertEqual(out['cost'], cost)
            amount = (out['events'][0]['amount'] if out['events'] else out['auras'][0]['per_tick'])
            self.assertEqual(amount, health)

    def test_stronger_caster_ranks_keep_advancing_with_and_without_gear(self):
        # Empirical completed casts, not just the tuning formula: later ranks
        # must remain stronger after bonus coefficients and integer tick rounding.
        for top in (9912, 9835, 25298, 17402, 25299, 9858, 25297, 9863):
            chain = self.cat.chain(top)
            rows = sorted((r for r in self.rows if r['chain'] == chain), key=lambda r: r['rank'])
            for items in ((), FULL):
                peaks, costs = [], []
                for row in rows:
                    values = []
                    req = self.request(row)
                    req['caster'].update(level=60, items=list(items), skyrim={'destruction': 15, 'restoration': 15})
                    if req['target']['kind'] == 'unit':
                        req['target']['level'] = 60
                    for seed in range(64):
                        req['seed'] = seed
                        out = self.answer(req)
                        if any(e.get('crit') or e['type'] in ('miss', 'resist') for e in out['events']):
                            continue
                        total = sum(e.get('amount', 0) for e in out['events'] if e['type'] in ('damage', 'heal'))
                        total += sum(a['per_tick'] * (a['duration_ms'] // a['amplitude_ms'])
                                     for a in out['auras'] if a['type'] in ('damage', 'heal'))
                        values.append(total)
                    self.assertTrue(values, row)
                    peaks.append(max(values)); costs.append(out['cost'])
                with self.subTest(chain=chain, gear=bool(items)):
                    self.assertEqual(peaks, sorted(peaks))
                    self.assertEqual(costs, sorted(costs))
                    self.assertLess(peaks[0], peaks[-1])
                    self.assertGreater(costs[0], 0)

    def test_every_channel_rank_ticks_without_charging_again(self):
        for row in self.rows:
            if row['chain'] not in (740, 16914):
                continue
            req = self.request(row)
            application = self.answer(req)
            aura = next(a for a in application['auras'] if a['type'] in ('heal', 'damage'))
            tick = {'op': 'tick_area', 'spell': row['spell'], 'caster': req['caster'], 'tick': 1, 'seed': 11,
                    'targets': [{'id': 7, 'level': row['required_level'], 'friendly': row['chain'] == 740}]}
            out = self.answer(tick)
            self.assertNotIn('cost', out)
            self.assertEqual(out['results'][0]['amount'], aura['per_tick'])
            tick['caster']['skyrim'] = {k: 15 for k in skyrim.SKILLS.values()}
            scaled = self.answer(tick)
            self.assertEqual(scaled['results'][0]['amount'], round(aura['per_tick'] * self.cat.balance[row['spell']].health_factor))
            self.assertEqual(scaled['skill'], self.cat.balance[row['spell']].skill)

    def test_cost_book_covers_every_rank_and_progresses_from_cheap_ranks(self):
        books = []
        for skill in (15, 50, 100):
            costs = self.svc.handle({'op': 'skyrim_costs', 'caster': {
                'level': 60, 'skyrim': {k: skill for k in skyrim.SKILLS.values()}}})['costs']
            self.assertEqual(set(costs), {str(sid) for sid in self.cat.class_spells})
            books.append(costs)
        for sid in self.cat.class_spells:
            self.assertGreaterEqual(books[0][str(sid)], books[1][str(sid)])
            self.assertGreaterEqual(books[1][str(sid)], books[2][str(sid)])
        for chain in {row['chain'] for row in self.rows}:
            ids = [r['spell'] for r in sorted(self.rows, key=lambda r: r['rank']) if r['chain'] == chain]
            values = [books[0][str(sid)] for sid in ids]
            self.assertEqual(values, sorted(values), chain)
        for first, last in ((5176, 9912), (5185, 25297), (774, 25299), (8921, 9835), (1126, 9885)):
            self.assertLess(books[0][str(first)], books[0][str(last)] * (.4 if first in (5176, 8921) else .2))
        for sid, form in feral.FORM_SPELLS.items():
            req = self.request(self.by_id[sid])
            req['caster'].update(form=form, skyrim={'alteration': 15})
            self.assertEqual(self.answer(req)['cost'], 0)
            costs = self.svc.handle({'op': 'skyrim_costs', 'caster': req['caster']})['costs']
            self.assertEqual(costs[str(sid)], 0)

    def test_lower_feral_ranks_obey_mechanics_and_queue_their_own_id(self):
        for row in self.rows + self.ff:
            sid = row['spell']
            kind = feral.ability(self.cat, sid)
            if kind is None:
                continue
            with self.subTest(spell=sid):
                req = self.request(row, seed=1)
                good = self.answer(req)
                req['caster']['form'] = 'none'
                self.assertEqual(self.svc.respond(json.dumps(req).encode())['error'], 'SPELL_FAILED_ONLY_SHAPESHIFT')
                req = self.request(row, seed=1)
                if kind == 9881:
                    self.assertEqual(good['events'][0]['spell'], sid)
                    self.assertTrue(good['cost_deferred'])
                    req.update(op='melee')
                    req['caster']['next_swing'] = sid
                    swing = self.answer(req)
                    self.assertEqual((swing['spell'], swing['cost'], swing['rage_gained']), (sid, 15, 0))
                    self.assertTrue(swing['next_swing_consumed'])
                    req['caster']['power']['rage'] = 0
                    fallback = self.answer(req)
                    self.assertIn('ERR_OUT_OF_RAGE', fallback['next_swing_error'])
                if kind in (9896, 31018):
                    req['caster']['combo_points'] = 0
                    self.assertEqual(self.svc.respond(json.dumps(req).encode())['error'], 'SPELL_FAILED_NO_COMBO_POINTS')
                    self.assertEqual(good['combo_points_spent'], 2)
                if kind in (9827, 9867):
                    req['caster']['prowling'] = False
                    self.assertEqual(self.svc.respond(json.dumps(req).encode())['error'], 'SPELL_FAILED_ONLY_STEALTHED')
                if kind in (9830, 9867):
                    req = self.request(row)
                    req['target']['behind'] = False
                    self.assertEqual(self.svc.respond(json.dumps(req).encode())['error'], 'SPELL_FAILED_NOT_BEHIND')
                if kind in (9913, 9846, 9821, 22896):
                    self.assertTrue(all(a['ends_on_form_loss'] for a in good['auras']))
                if kind in (9850, 9830, 9904, 9827, 9867):
                    miss = self.answer(self.request(row, seed=31))
                    self.assertEqual(miss['events'], [{'type': 'miss'}])
                    self.assertEqual(miss['power_refund'], int(good['cost'] * .8))
                    self.assertNotIn('combo_points', miss)
                if kind == 9908:
                    req = self.request(row, seed=1)
                    req['targets'] = [{**req['target'], 'id': i} for i in range(4)]
                    swipe = self.answer(req)
                    self.assertEqual(len(swipe['results']), 3)
                if kind == 22896:
                    expected = {22842: 10, 22895: 15, 22896: 20}[sid]
                    self.assertEqual(good['auras'][0]['health_per_rage'], expected)
                if kind == 9896:
                    # Rip's AP addition is capped at four CP for every rank.
                    req = self.request(row)
                    req['caster']['combo_points'] = 5
                    rip = self.answer(req)['auras'][0]['per_tick']
                    s = self.cat.spell(sid)
                    ch = self.svc.char(req['caster'])
                    expected = s['EffectBasePoints1'] + s['EffectBaseDice1'] + int(s['EffectPointsPerComboPoint1'] * 5) + int(ch.attack_power() * .04)
                    self.assertEqual(rip, expected)

    def test_class_driven_rules_do_not_require_druid_ids(self):
        raw = copy.deepcopy(self.raw)
        # A configured class has a two-handed support chain, with ordinary
        # stat effects and arbitrary spell IDs absent from the pinned slice.
        raw['spells'][400001] = {**raw['spells'][1126], 'Id': 400001}
        raw['spells'][400002] = {**raw['spells'][9885], 'Id': 400002}
        raw['ranks'] = {400001: {'chain': 400001, 'rank': 1, 'skill': 'two_handed'},
                        400002: {'chain': 400001, 'rank': 2, 'skill': 'two_handed'}}
        cat = Catalog(raw=raw)
        self.assertEqual(cat.balance[400001].skill, 'TwoHanded')
        self.assertEqual(cat.balance[400001].wow_cost, cat.spell(400002)['ManaCost'])
        self.assertEqual(cat.balance[400001].base_cost, cat.balance[400002].base_cost)
