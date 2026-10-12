"""Stateless, class-data-driven spell acquisition; opt-in through known_spells."""
import json
import math
from pathlib import Path

from service.skyrim import SKILLS

DEFAULT = Path(__file__).resolve().parents[1] / 'data/progression.json'
SKILL_NAMES = {value: {'OneHanded': 'One-handed', 'TwoHanded': 'Two-handed', 'Marksman': 'Archery'}.get(name, name)
               for name, value in SKILLS.items()}


def number(value, field):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f'{field} must be a finite number')
    return value


def ids(values, field):
    if not isinstance(values, list) or any(isinstance(v, bool) or not isinstance(v, int) or v <= 0 for v in values):
        raise ValueError(f'{field} must be an array of positive integer IDs')
    return set(values)


def wow_level(skill, curve):
    """Floor a clamped linear skill-to-level curve, so ranks never unlock early."""
    skill = min(curve['skill_max'], max(curve['skill_min'], number(skill, 'skill')))
    return curve['level_min'] + math.floor((skill - curve['skill_min']) *
                                          (curve['level_max'] - curve['level_min']) /
                                          (curve['skill_max'] - curve['skill_min']))


def skill_value(level, curve):
    """Invert the curve: the lowest whole skill (Skyrim's skills are whole numbers) that reaches the level."""
    if level <= curve['level_min']:
        return curve['skill_min']
    if level > curve['level_max'] or curve['level_max'] == curve['level_min']:
        return None  # No value on this curve teaches this rank.
    value = curve['skill_min'] + ((level - curve['level_min']) *
                                 (curve['skill_max'] - curve['skill_min']) /
                                 (curve['level_max'] - curve['level_min']))
    value = math.ceil(value - 1e-9)
    while wow_level(value, curve) < level:
        value += 1
    return value


class Progression:
    def __init__(self, path=DEFAULT, *, data=None):
        self.data = data if data is not None else json.loads(Path(path).read_text())
        if self.data['schema'] != 1:
            raise ValueError('unsupported progression schema')
        self.curve = self.data['curve']
        if self.curve['skill_max'] <= self.curve['skill_min'] or self.curve['level_max'] < self.curve['level_min']:
            raise ValueError('invalid progression curve')
        supported = set(SKILLS.values())
        for definition in self.data['classes'].values():
            used = {s['skill'] for s in definition['spells'] + definition.get('excluded', [])}
            used.update(line['skill'] for line in definition['lines'].values())
            used.add(definition['default_skill'])
            used.update(mapping['skill'] for mapping in definition.get('wow_skill_mappings', {}).values())
            if used - supported:
                raise ValueError(f'unsupported Skyrim skills: {sorted(used - supported)}')

    def class_definition(self, value):
        if isinstance(value, str) and value.lower() in self.data['classes']:
            key = value.lower()
            return key, self.data['classes'][key]
        if isinstance(value, int) and not isinstance(value, bool):
            for key, definition in self.data['classes'].items():
                if definition['id'] == value:
                    return key, definition
        raise ValueError(f'unknown progression class {value!r}')

    def known_spells(self, req):
        key, definition = self.class_definition(req.get('class'))
        caster = req.get('caster', {})
        if not isinstance(caster, dict):
            raise ValueError('caster must be an object')
        skills = caster.get('skyrim')
        if skills is not None and not isinstance(skills, dict):
            raise ValueError('caster.skyrim must be an object')
        if skills is None:
            if 'level' not in caster:
                raise ValueError('known_spells requires caster.skyrim or caster.level')
            level = number(caster['level'], 'caster.level')
            if int(level) != level:
                raise ValueError('caster.level must be an integer')
            level = min(self.curve['level_max'], max(self.curve['level_min'], int(level)))
            levels = {skill: level for skill in SKILLS.values()}
        else:
            # Progression defaults to novice, unlike combat conversion's skill 50.
            for skill, value in skills.items():
                if skill not in SKILLS.values():
                    raise ValueError(f'unknown Skyrim skill {skill!r}')
                number(value, f'caster.skyrim.{skill}')
            levels = {skill: wow_level(skills.get(skill, self.curve['skill_min']), self.curve)
                      for skill in SKILLS.values()}
        since = ids(req.get('since', []), 'since')
        acquired = ids(req.get('acquired_spells', []), 'acquired_spells')
        all_rows = {s['spell']: s for s in definition['spells'] + definition.get('excluded', [])}
        talent_known, talent_upgrades = set(), []
        if 'talents' in caster:
            from service.talents import book
            _, talent_class = book().class_definition(definition['id'])
            allocation = book().allocation(talent_class, caster['talents'], caster.get('level', 60))
            talent_rows = book().rows(talent_class)
            for tid, rank in allocation.items():
                talent_known.update(talent_rows[tid]['rank_spells'][:rank])
                for sid in talent_rows[tid]['rank_spells'][:rank]:
                    talent_known.update(talent_rows[tid].get('granted_spells', {}).get(str(sid), []))
            for row in talent_class.get('granted_spellbook', []):
                all_rows.setdefault(row['spell'], row)
            direct = {sid for row in talent_rows.values() for sid in row['rank_spells']}
            roots = {all_rows[sid]['chain'] for sid in talent_known}
            for row in definition.get('excluded', []):
                if row['source'] == 'talent' and row['spell'] not in direct and row['chain'] in roots:
                    options = talent_class['trainer_options'].get(str(row['spell']), [])
                    if options:
                        upgrade = {**row, 'trainer_options': options}
                        talent_upgrades.append(upgrade)
                        all_rows[row['spell']] = upgrade
        if acquired - all_rows.keys():
            raise ValueError(f'acquired_spells contains spells outside {key} progression')
        wow_skills = caster.get('wow_skills', {})
        if not isinstance(wow_skills, dict):
            raise ValueError('caster.wow_skills must be an object')
        conditions = ids(req.get('satisfied_conditions', []), 'satisfied_conditions')

        def gate_open(option, level, known):
            if level < option['required_level'] or option['condition_id'] and option['condition_id'] not in conditions:
                return False
            if option['req_skill']:
                value = wow_skill_value(option['req_skill'], known)
                if number(value, 'caster.wow_skills value') < option['req_skill_value']:
                    return False
            return all(sid in known for sid in option['required_abilities'])

        def wow_skill_value(skill_id, known):
            key = str(skill_id)
            if key in wow_skills:
                return wow_skills[key]
            mapping = definition.get('wow_skill_mappings', {}).get(key)
            if mapping and (not mapping.get('required_spell') or mapping['required_spell'] in known):
                return levels[mapping['skill']] * mapping['values_per_level']
            return 0

        # Acquire in dependency order, independent of tab order or file ordering.
        # Automatic alternative routes and virtual WoW skills are class data.
        known = set(acquired) | talent_known
        automatic = [r for r in definition.get('excluded', []) if r['source'] in definition.get('auto_sources', ('quest',))]
        pending = list(definition['spells']) + automatic + talent_upgrades
        while pending:
            remaining = []
            for row in pending:
                sid = row['spell']
                if sid in known:
                    continue
                prerequisites = [row['previous_spell'], row['required_spell']]
                level = levels[row['skill']]
                eligible = row['required_level'] <= level and all(p == 0 or p in known for p in prerequisites)
                if row.get('trainer_options'):
                    eligible = eligible and any(gate_open(option, level, known) for option in row['trainer_options'])
                if eligible:
                    known.add(sid)
                else:
                    remaining.append(row)
            if len(remaining) == len(pending):
                break
            pending = remaining
        rows = sorted((all_rows[sid] for sid in known), key=lambda s: (s['tab'], s['chain'], s['rank'], s['spell']))
        highest = {}
        old_highest = {}
        for row in rows:
            chain = str(row['chain'])
            if chain not in highest or (row['rank'], row['spell']) > (all_rows[highest[chain]]['rank'], highest[chain]):
                highest[chain] = row['spell']
        for sid in since & all_rows.keys():
            row = all_rows[sid]
            chain = str(row['chain'])
            if chain not in old_highest or row['rank'] > all_rows[old_highest[chain]]['rank']:
                old_highest[chain] = sid
        replacements = [{'chain': int(chain), 'old_spell': old, 'new_spell': highest[chain]}
                        for chain, old in old_highest.items() if chain in highest and
                        all_rows[highest[chain]]['rank'] > all_rows[old]['rank']]
        line_results = {}
        for line_id, line in definition['lines'].items():
            line_rows = [row for row in rows if row['skill_line'] == int(line_id)]
            line_results[str(line_id)] = {**line, 'wow_level': levels[line['skill']],
                                         'spells': [s['spell'] for s in line_rows],
                                         'highest_ranks': [s['spell'] for s in line_rows if highest[str(s['chain'])] == s['spell']]}
        unlearned = []
        for row in sorted(definition['spells'] + automatic + talent_upgrades, key=lambda s: (s['tab'], s['chain'], s['rank'], s['spell'])):
            value = skill_value(row['required_level'], self.curve)
            if row['spell'] in known or value is None:
                continue
            requirements = []
            for sid in (row['previous_spell'], row['required_spell']):
                if sid and sid not in known:
                    prerequisite = all_rows.get(sid)
                    label = str(sid) if prerequisite is None else prerequisite['name'] + (
                        ' (' + prerequisite['rank_name'] + ')' if prerequisite.get('rank_name') else '')
                    route = prerequisite and prerequisite['source']
                    requirements.append('Requires ' + label + (f' ({route})' if route in ('quest', 'item', 'talent') else ''))
            alternatives = []
            for option in row.get('trainer_options', []):
                gates = []
                if option['condition_id'] and option['condition_id'] not in conditions:
                    gates.append(f"condition {option['condition_id']}")
                if option['req_skill'] and number(wow_skill_value(option['req_skill'], known), 'caster.wow_skills value') < option['req_skill_value']:
                    gates.append(f"WoW skill {option['req_skill']} {option['req_skill_value']}")
                gates.extend(f'spell {sid}' for sid in option['required_abilities'] if sid not in known)
                if option['required_level'] > row['required_level']:
                    gates.append(f"{SKILL_NAMES[row['skill']]} {skill_value(option['required_level'], self.curve)}")
                label = ', '.join(gates)
                if label not in alternatives:
                    alternatives.append(label)
            if alternatives and '' not in alternatives:
                requirements.append('Also requires ' + ' or '.join(alternatives))
            unlearned.append({**{k: v for k, v in row.items() if k != 'trainer_options'},
                              'wow_level': levels[row['skill']], 'learn_skill': SKILL_NAMES[row['skill']],
                              'learn_skill_value': value, 'learn_requirements': requirements})
        return {'class': key, 'class_id': definition['id'], 'wow_levels': levels, 'lines': line_results,
                'unlearned_spells': unlearned,
                'known_spells': [s['spell'] for s in rows], 'highest_ranks': highest,
                'newly_learned': [s['spell'] for s in rows if s['spell'] not in since],
                'replacements': sorted(replacements, key=lambda r: r['chain']),
                'spells': [{**{k: v for k, v in s.items() if k != 'trainer_options'},
                            'wow_level': levels[s['skill']]} for s in rows]}
