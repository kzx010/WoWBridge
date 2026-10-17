"""The service side of the tooltip audit (wowcore/tests/test_tooltip_audit.cpp, [tooltips][audit]).

For a few live-shaped casters this records what the game hands the interface (op skyrim_costs: magicka costs and the
spell tooltip profiles; op talents; op known_spells) and what the service actually deals, heals and charges for every
druid spell rank (op resolve / tick_area, many seeds). The C++ audit renders every rank's tooltip from the first and
checks its numbers against the second. Regenerate after a service or catalog change:
    toolchain/venv/bin/python -m service.tests.test_tooltip_audit --write
"""
import json
from pathlib import Path
import sys
import unittest

from service import feral
from service.catalog import Catalog
from service.progression import Progression
from service.server import Service
from service.spells import CastError
from service.talents import book as talent_book

FIXTURE = Path(__file__).resolve().parents[2] / 'wowcore/tests/fixtures/tooltip_audit.json'
# The plugin's skill keys (skse/src/Character.cpp kSkills): the service's, then the interface's.
SKILLS = {'destruction': 'Destruction', 'restoration': 'Restoration', 'alteration': 'Alteration',
          'archery': 'Marksman', 'one_handed': 'OneHanded', 'two_handed': 'TwoHanded'}
HOSTILE = {'kind': 'unit', 'reaction': 'hostile', 'alive': True, 'distance_yd': 20, 'los': True, 'id': 7}
SEEDS = 48


def talent_build(level):
    """A Balance/Restoration allocation using every point at this level, in tier order (the service validates it)."""
    books = talent_book()
    _, definition = books.class_definition(11)
    ranks = {}
    budget = sum(1 for _ in range(10, level + 1))
    for tree in (definition['trees'][0], definition['trees'][2], definition['trees'][1]):
        for t in sorted(tree['talents'], key=lambda t: (t['tier'], t['column'])):
            for rank in range(t['max_rank'], 0, -1):
                trial = {**ranks, str(t['id']): rank}
                if sum(trial.values()) > budget:
                    continue
                try:
                    books.allocation(definition, trial, level)
                except ValueError:
                    continue
                ranks = trial
                break
    return ranks


def casters():
    return {
        # A fresh level-60 druid at Skyrim's middle skills, no talents or gear.
        'level60': {'level': 60, 'skills': 50, 'talents': {}},
        # Early game: low skills, the ranks she can have.
        'level20': {'level': 20, 'skills': 15, 'talents': {}},
        # Talents that change numbers (Moonfury, Gift of Nature, Improved Rejuvenation...) at high skills.
        'talented60': {'level': 60, 'skills': 75, 'talents': talent_build(60)},
    }


def live_caster(spec):
    """As skse/src/Character.cpp Caster() and Progression::Augment send it."""
    return {'level': spec['level'], 'items': [], 'skyrim': {k: spec['skills'] for k in SKILLS},
            'equipment_modifiers': {}, 'class': 11, 'talents': spec['talents'], 'perks': {}, 'stealthed': False,
            'wearing_armor': False, 'indoors': False, 'reagents': {'17026': 5, '17038': 5}, 'in_combat': False,
            'moving': False}


def _health_effects(spell):
    """Effect index -> 'direct' | 'periodic' | 'shield' for the effects that deal or heal health."""
    out = {}
    for e in (1, 2, 3):
        effect, aura = spell[f'Effect{e}'], spell[f'EffectApplyAuraName{e}']
        if effect in (2, 10):
            out[e] = 'direct'
        elif effect in (6, 27, 35, 65) and aura in (3, 8):
            out[e] = 'periodic'
        elif effect in (6, 35, 65) and aura == 15:
            out[e] = 'shield'
    return out


def _attempt(svc, cat, sid, caster, level, seed):
    spell = cat.spell(sid)
    effects = _health_effects(spell)
    heal = any(spell[f'Effect{e}'] == 10 or spell[f'EffectApplyAuraName{e}'] in (8, 15) for e in effects)
    unit = {**HOSTILE, 'level': level}
    if heal:
        unit = {**unit, 'reaction': 'friendly', 'friendly': True, 'id': 8}
    for target in (unit, {'kind': 'self'}, {'kind': 'ground', 'distance_yd': 20}):
        try:
            return svc.handle({'op': 'resolve', 'spell': sid, 'seed': seed, 'target': target, 'caster': caster})
        except CastError:
            continue
    return None


def resolved(svc, cat, sid, caster, level):
    """Bounds of what the service deals or heals per effect for this caster (no crits, resists or absorbs)."""
    spell = cat.spell(sid)
    effects = _health_effects(spell)
    if not effects or feral.ability(cat, sid) in feral.ABILITIES:
        return None
    rolls = any(kind == 'direct' and spell[f'EffectDieSides{e}'] > 1 for e, kind in effects.items())
    direct, periodic, shields, cost = [], {}, {}, None
    for seed in range(SEEDS if rolls else 1):
        out = _attempt(svc, cat, sid, caster, level, seed)
        if out is None:
            return None
        cost = out.get('cost', cost)
        for e in out['events']:
            if e['type'] in ('damage', 'heal') and not e.get('crit') and not e.get('resisted') and not e.get('absorbed'):
                direct.append(e['amount'])
        for a in out['auras']:
            if a.get('type') in ('damage', 'heal') and 'per_tick' in a:
                amount = a['per_tick']
                if 'area' in a:   # Hurricane, Tranquility: each tick reaches its targets through tick_area
                    tick = svc.handle({'op': 'tick_area', 'spell': sid, 'seed': seed, 'caster': caster,
                                       'targets': [{'id': 1, 'level': level, 'friendly': a['type'] == 'heal'}]})
                    amount = next(r['amount'] for r in tick['results'] if r['type'] == a['type'])
                ticks = len(a['ticks']) if 'ticks' in a else a['duration_ms'] // a['amplitude_ms']
                low, high, _ = periodic.get(str(a['eff']), (amount, amount, ticks))
                periodic[str(a['eff'])] = (min(low, amount), max(high, amount), ticks)
            elif a.get('type') == 'damage_shield':
                shields[str(a['eff'])] = a['amount']
    out = {'cost': cost}
    if direct:
        out['direct'] = [min(direct), max(direct)]
        out['direct_effect'] = next(e for e, kind in effects.items() if kind == 'direct')
    if periodic:
        out['periodic'] = {e: list(v) for e, v in periodic.items()}
    if shields:
        out['shield'] = shields
    return out if len(out) > 1 else None


def weapon_damages():
    """Gear::WeaponDamages for a bag of every imported weapon and every fourth Armory weapon: entry -> native damage."""
    root = FIXTURE.parents[3]
    gear = json.loads((root / 'build/gear.json').read_text())['items']
    armory = json.loads((root / 'build/armory.json').read_text())['items']
    out = {e: r['skyrim']['damage'] for e, r in gear.items() if r.get('kind') == 'WEAP' and r.get('skyrim', {}).get('damage')}
    weapons = sorted((int(e) for e, r in armory.items() if r['kind'] == 'WEAP' and r['skyrim'].get('damage')))
    out.update({str(e): armory[str(e)]['skyrim']['damage'] for e in weapons[::4]})
    return out


def build(cat):
    svc = Service(cat, None)
    _, definition = Progression().class_definition(11)
    data = {'casters': {}}
    for name, spec in casters().items():
        caster = live_caster(spec)
        reply = svc.handle({'op': 'skyrim_costs', 'caster': caster})
        profiles = reply['spell_tooltips']
        druid = sorted(int(sid) for sid, p in profiles.items() if not p.get('item_use'))
        if name != 'level60':   # Items' use profiles do not depend on the class book: kept once
            profiles = {sid: p for sid, p in profiles.items() if not p.get('item_use')}
        level_ok = {row['spell'] for row in definition['spells'] + definition['excluded']
                    if row.get('required_level', 0) <= spec['level']}
        data['casters'][name] = {
            'level': spec['level'],
            'skyrim': {SKILLS[k]: v for k, v in caster['skyrim'].items()},
            'costs': {str(sid): reply['costs'][str(sid)] for sid in druid if str(sid) in reply['costs']},
            'spell_tooltips': profiles,
            'resolve': {str(sid): r for sid in druid if sid in level_ok
                        if (r := resolved(svc, cat, sid, caster, spec['level']))},
        }
        if name == 'level60':
            # Her swing with each weapon (Cast.cpp sends min and max; the service takes 256 at a time).
            damages, swings = weapon_damages(), {}
            batch = sorted(damages, key=int)
            for at in range(0, len(batch), 256):
                part = {e: damages[e] for e in batch[at:at + 256]}
                swings.update(svc.handle({'op': 'skyrim_costs', 'caster': caster, 'weapons': part})['weapon_tooltips'])
            data['casters'][name]['weapons'] = {e: [round(w['min'], 6), round(w['max'], 6)] for e, w in swings.items()}
            # Every talent at rank 0: the audit renders each of its ranks in turn.
            data['talents'] = svc.handle({'op': 'talents', 'class': 11, 'caster': caster})
        if name == 'level20':
            # A young druid's book: most ranks still to learn, each with its "Learned at" lines.
            book = svc.handle({'op': 'known_spells', 'class': 11, 'caster': caster})
            # Its profiles are the costs answer's (test_book_profiles_are_the_costs_profiles): kept as their ids.
            book['spell_tooltips'] = sorted(int(sid) for sid in book['spell_tooltips'])
            data['book'] = book
    return data


def dump(data):
    return json.dumps(data, sort_keys=True, separators=(',', ':')) + '\n'


class TooltipAuditFixture(unittest.TestCase):
    def test_book_profiles_are_the_costs_profiles(self):
        """The book's answer and the costs answer arrive in either order: each spell's profile must be the same, and
        the book's must cover every spell it shows (learned or not)."""
        cat = Catalog()
        svc = Service(cat, None)
        for name, spec in casters().items():
            caster = live_caster(spec)
            book = svc.handle({'op': 'known_spells', 'class': 11, 'caster': caster})
            costs = svc.handle({'op': 'skyrim_costs', 'caster': caster})['spell_tooltips']
            shown = set(book['known_spells']) | {row['spell'] for row in book['unlearned_spells']}
            with self.subTest(caster=name):
                self.assertEqual({int(sid) for sid in book['spell_tooltips']}, shown)
                for sid, profile in book['spell_tooltips'].items():
                    self.assertEqual(profile, costs[sid], sid)

    def test_fixture_matches_the_service(self):
        if not all((FIXTURE.parents[3] / 'build' / f).exists() for f in ('gear.json', 'armory.json')):
            self.skipTest('build/gear.json and build/armory.json not built')
        self.assertEqual(FIXTURE.read_text(), dump(build(Catalog())),
                         'regenerate: python -m service.tests.test_tooltip_audit --write')


if __name__ == '__main__':
    if '--write' in sys.argv:
        FIXTURE.write_text(dump(build(Catalog())))
        print(FIXTURE, FIXTURE.stat().st_size)
    else:
        unittest.main()
