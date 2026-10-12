"""Vanilla Skyrim perks translated to class-driven WoW spell modifiers.

Rules and form IDs are pinned data, not class branches. Native abilities and
crafting perks deliberately stay in Skyrim; see docs/perks.md.
"""
from dataclasses import replace
import json
from pathlib import Path

from service import formulas as F
from service.progression import Progression, skill_value

DEFAULT = Path(__file__).resolve().parents[1] / 'data/perks.json'
WHITE = {'Id': 0, 'SpellFamilyName': 0, 'SpellFamilyFlags': 0}
DAMAGE_EFFECTS = {2, 9, 17, 31, 58, 121}


class Perks:
    def __init__(self, cat, progression=None, path=DEFAULT):
        self.cat = cat
        self.progression = progression or Progression()
        self.data = json.loads(Path(path).read_text())
        self._metadata = {}

    def taken(self, caster):
        values = caster.get('perks', {})
        if not isinstance(values, dict) or any(type(v) is not bool for v in values.values()):
            raise ValueError('caster.perks must be an object of editor-ID keys and booleans')
        if values.keys() - self.data['perks'].keys():
            raise ValueError(f'unknown vanilla perks: {sorted(values.keys() - self.data["perks"].keys())}')
        for key in ('wearing_armor', 'stealthed'):
            if key in caster and type(caster[key]) is not bool:
                raise ValueError(f'caster.{key} must be a boolean')
        return {key for key, value in values.items() if value}

    def metadata(self, cls):
        if cls in self._metadata:
            return self._metadata[cls]
        _, definition = self.progression.class_definition(cls)
        rows = {r['spell']: r for r in definition['excluded'] + definition['spells'] if not r['passive']}
        # Triggered combat effects inherit the line of their active parent;
        # pet-learning wrappers and passive talent links are not player casts.
        pending = list(rows)
        while pending:
            sid = pending.pop()
            s = self.cat.spells.get(sid)
            if not s:
                continue
            for e in (1, 2, 3):
                trigger = s[f'EffectTriggerSpell{e}']
                if trigger and trigger not in rows and (s[f'Effect{e}'] == 64 or
                        s[f'Effect{e}'] == 6 and s[f'EffectApplyAuraName{e}'] in (23, 42)):
                    rows[trigger] = {**rows[sid], 'spell': trigger}
                    pending.append(trigger)
        for parent, children in self.data.get('code_links', {}).items():
            if int(parent) in rows:
                for sid in children:
                    rows[sid] = {**rows[int(parent)], 'spell': sid}
        # Poison payloads are applied by consumables, not trainer spells.
        # DBC family + pinned player payloads identify these without class branches.
        families = {self.cat.spells[sid]['SpellFamilyName'] for sid in rows if sid in self.cat.spells}
        families.discard(0)
        for sid, s in self.cat.spells.items():
            if s['SpellFamilyName'] in families and sid in self.cat.poison_payloads:
                rows.setdefault(sid, {'spell': sid, 'chain': self.cat.chain(sid),
                                      'skill': 'alchemy', 'required_level': s['SpellLevel']})
        self._metadata[cls] = rows
        return rows

    def tier(self, s, row):
        rule = self.cat.balance.get(s['Id'])
        from service.skyrim_balance import COUNTERPARTS, SPELLS
        pinned = {self.cat.chain(sid) for sid in SPELLS}
        if rule and rule.cost and self.cat.chain(s['Id']) in pinned:
            minimum = max(COUNTERPARTS[name]['minimum_skill'] for name, _ in rule.cost)
        else:
            minimum = skill_value(row['required_level'], self.progression.curve) or 100
        return sum(minimum >= gate for gate in (25, 50, 75, 100))

    @staticmethod
    def effects(s, role):
        if role == 'armor':
            return [e for e in (1, 2, 3) if s[f'EffectApplyAuraName{e}'] == 22
                    and s[f'EffectMiscValue{e}'] == 1 and s[f'EffectBasePoints{e}'] >= 0]
        return []

    def matches(self, select, s, row):
        if row['chain'] in select.get('exclude_chains', ()):
            return False
        if 'skill' in select and select['skill'] != row['skill']:
            return False
        if 'school' in select and select['school'] != s['School']:
            return False
        if select.get('mana') and s['PowerType'] != 0:
            return False
        if 'tier' in select and select['tier'] != self.tier(s, row):
            return False
        if select.get('lasting') and self.cat.duration(s, 60) <= 0:
            return False
        role = select.get('role')
        active = [e for e in (1, 2, 3) if s[f'Effect{e}']]
        damage = any(s[f'Effect{e}'] in DAMAGE_EFFECTS or s[f'EffectApplyAuraName{e}'] in (3, 15, 53) for e in active)
        direct = s['School'] == 0 and any(s[f'Effect{e}'] in DAMAGE_EFFECTS for e in active)
        if role == 'damage':
            return damage
        if role == 'direct_attack':
            return direct
        if role == 'bleed':
            return s['School'] == 0 and any(s[f'EffectApplyAuraName{e}'] == 3 for e in active)
        if role == 'heal':
            return any(s[f'Effect{e}'] == 10 or s[f'EffectApplyAuraName{e}'] == 8 for e in active)
        if role == 'armor_ward':
            return bool(self.effects(s, 'armor'))
        if role == 'poison_damage':
            return damage and s['Id'] in self.cat.poison_payloads
        if role in self.data['roles']:
            return row['chain'] in self.data['roles'][role] and damage
        return role is None

    def selected(self, taken):
        chosen = []
        for rule in self.data['rules']:
            owned = [(rank, key) for key, rank in rule['perks'].items() if key in taken]
            if owned:
                rank, key = max(owned)
                chosen.append((rule, rank, key))
        # Assassin's Blade replaces Backstab. Rank-chain perks above likewise
        # use only the highest owned rank, even when HasPerk reports all ranks.
        best = {}
        for rule, rank, key in chosen:
            if rule.get('stack_group'):
                best[rule['stack_group']] = max(best.get(rule['stack_group'], 0), rank)
        return [(r, rank, key) for r, rank, key in chosen
                if not r.get('stack_group') or rank == best[r['stack_group']]]

    def augment(self, ch, caster):
        taken = self.taken(caster)
        if not taken:
            return ch
        from service.skyrim import melee_skill
        white_skill = 'one_handed' if ch.form in ('cat', 'bear', 'dire_bear') else (
            'two_handed' if melee_skill(self.cat, ch) == 'TwoHanded' else 'one_handed')
        mods = list(ch.mods)
        for rule, rank, key in self.selected(taken):
            if rule.get('condition') == 'unarmored' and caster.get('wearing_armor', True):
                continue  # missing equipment state never grants Mage Armor
            if rule.get('condition') == 'stealthed' and not (caster.get('prowling') or caster.get('stealthed')):
                continue
            matched = [sid for sid, row in self.metadata(ch.cls).items() if sid in self.cat.spells
                       and self.matches(rule['select'], self.cat.spells[sid], row)]
            if rule.get('white') and rule['select'].get('skill') == white_skill:
                matched.append(0)
            effects = ({sid: self.effects(self.cat.spells[sid], rule['effect_role']) for sid in matched}
                       if rule.get('effect_role') else None)
            for op, value in rule['mods'].items():
                value = rank if value == 'rank' else value[rank - 1] if isinstance(value, list) else value
                mods.append({'op': int(op), 'type': F.SPELLMOD_FLAT if int(op) == 7 else F.SPELLMOD_PCT,
                             'value': value, 'spells': frozenset(matched), 'spell': 0,
                             'family': 0, 'mask': 0, 'class_driven': True, 'perk': key,
                             **({'effects': effects} if effects is not None else {})})
        return replace(ch, mods=mods)

    def finish(self, caster, out):
        """Respite uses completed Skyrim healing, so it never scales twice."""
        if 'skyrim' not in caster or not caster.get('perks', {}).get('Respite'):
            return out
        cls = caster.get('class', self.cat.raw['character'].get('class', 11))
        if out.get('spell') not in self.metadata(cls):
            return out
        for part in out.get('events', []) + out.get('results', []):
            if part['type'] == 'heal':
                part['stamina'] = part['amount']
        for aura in out.get('auras', []):
            if aura['type'] in ('heal', 'rage_heal'):
                aura['respite'] = True
        return out
