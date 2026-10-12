#!/usr/bin/env python3
"""Build class progression from the read-only ClassicDB dump and client DBCs.

Usage: python -m data.build_progression [--dump PATH] [--dbc-dir PATH]
Class mappings are data/class_progression.json; no live DB is needed.
"""
import argparse
import hashlib
import json
from pathlib import Path

from data import dbc
from data.build_catalog import DBC_DIR, REPO
from data.sql_dump import tables

DEFAULT_DUMP = Path(__file__).resolve().parents[2] / 'WoW/cmangos/classic-db/Full_DB/ClassicDB_1_12_1_z2815.sql.gz'
CONFIG = REPO / 'data/class_progression.json'
DEFAULT_OUT = REPO / 'data/progression.json'
TRAINER_COLUMNS = ('entry', 'spell', 'reqlevel', 'reqskill', 'reqskillvalue',
                   'ReqAbility1', 'ReqAbility2', 'ReqAbility3', 'condition_id')
DUMP_COLUMNS = {
    'npc_trainer': TRAINER_COLUMNS,
    'npc_trainer_template': TRAINER_COLUMNS,
    'creature_template': ('Entry', 'TrainerType', 'TrainerClass', 'TrainerTemplateId', 'NpcFlags'),
    'spell_template': ('Id', 'SpellName', 'Rank1', 'SpellLevel', 'Attributes', 'Totem1', 'Totem2') + tuple(
        f'{field}{i}' for field in ('Effect', 'EffectTriggerSpell', 'EffectImplicitTargetA',
                                  'EffectImplicitTargetB') for i in (1, 2, 3)) + tuple(
        f'{field}{i}' for field in ('Reagent', 'ReagentCount') for i in range(1, 9)),
    'spell_chain': ('spell_id', 'prev_spell', 'first_spell', 'rank', 'req_spell'),
    'playercreateinfo_spell': ('race', 'class', 'Spell'),
    'quest_template': ('entry', 'MinLevel', 'RequiredClasses', 'RewSpell', 'RewSpellCast'),
    'item_template': ('entry', 'name', 'RequiredLevel') + tuple(f'spellid_{i}' for i in range(1, 6)),
}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def rank_chains(sql, abilities, talents):
    """SpellMgr::LoadSpellChains: Talent + DBC forward links + SQL overrides."""
    spells = {s['Id'] for s in sql['spell_template']}
    chains = {}
    for talent in talents:
        ranks = [sid for sid in talent['SpellRank'] if sid]
        if len(ranks) < 2:
            continue
        for rank, sid in enumerate(ranks, 1):
            chains[sid] = {'spell_id': sid, 'prev_spell': ranks[rank - 2] if rank > 1 else 0,
                           'first_spell': ranks[0], 'rank': rank, 'req_spell': 0}
    ability_spells = {a['Spell'] for a in abilities}
    previous = {}
    for ability in abilities:
        sid, forward = ability['Spell'], ability['SupercededBySpell']
        if sid in spells and forward in spells and forward in ability_spells:
            if forward in previous and previous[forward] != sid:
                raise ValueError(f'conflicting DBC predecessors for {forward}')
            previous[forward] = sid

    def derive(sid, visiting):
        if sid in chains:
            return chains[sid]
        if sid in visiting:
            raise ValueError(f'cycle in DBC rank chain at {sid}')
        prev = previous.get(sid, 0)
        parent = derive(prev, visiting | {sid}) if prev else None
        chains[sid] = {'spell_id': sid, 'prev_spell': prev,
                       'first_spell': parent['first_spell'] if parent else sid,
                       'rank': parent['rank'] + 1 if parent else 1, 'req_spell': 0}
        return chains[sid]

    for sid in previous:
        derive(sid, set())
    for row in sql['spell_chain']:
        if row['spell_id'] in chains:
            old = chains[row['spell_id']]
            if any(old[k] != row[k] for k in ('prev_spell', 'first_spell', 'rank')):
                raise ValueError(f'SQL/DBC chain conflict for {row["spell_id"]}')
        chains[row['spell_id']] = row
    return chains


def build_class(key, definition, sql, abilities, skill_lines, talent_spells, chains, skill_race_info):
    class_id = definition['id']
    mask = 1 << (class_id - 1)
    spells = {s['Id']: s for s in sql['spell_template']}
    lines = {int(k): {**v, 'name': skill_lines[int(k)]['DisplayName_lang']} for k, v in definition['lines'].items()}
    class_gates = {}
    for line_id in lines:
        gates = {(r['MinLevel'], bool(r['Flags'] & 0x100)) for r in skill_race_info
                 if r['SkillID'] == line_id and r['ClassMask'] & mask and r['RaceMask']}
        if len(gates) > 1:
            raise ValueError(f'{key}: race-dependent class gates for line {line_id}; class-only progression cannot represent them')
        class_gates[line_id] = next(iter(gates), (0, False))
    by_spell, spell_abilities = {}, {}
    for ability in abilities:
        if ability['ClassMask'] and not ability['ClassMask'] & mask or ability['ExcludeClass'] & mask:
            continue
        if ability['SkillLine'] in lines:
            by_spell.setdefault(ability['Spell'], set()).add(ability['SkillLine'])
            spell_abilities.setdefault(ability['Spell'], []).append(ability)

    def learned(sid):
        return [spells[sid][f'EffectTriggerSpell{i}'] for i in (1, 2, 3)
                if spells[sid][f'Effect{i}'] == 36 and spells[sid][f'EffectTriggerSpell{i}']]

    def first(sid):
        return chains.get(sid, {}).get('first_spell', sid)

    player_learn_targets = {s[f'EffectTriggerSpell{i}'] for s in spells.values() for i in (1, 2, 3)
                            if s[f'Effect{i}'] == 36 and s[f'EffectImplicitTargetA{i}'] in (0, 1)}

    def record(sid):
        s = spells[sid]
        chain = chains.get(sid, {})
        candidates = by_spell.get(sid, set())
        if len(candidates) > 1:
            raise ValueError(f'{key}: spell {sid} has ambiguous skill lines {sorted(candidates)}')
        line = next(iter(candidates), 0)
        utility = first(sid) in definition.get('utility_chains', [])
        skill = definition.get('utility_skill', definition['default_skill']) if utility else lines.get(line, {}).get('skill', definition['default_skill'])
        required = definition.get('line_prerequisites', {}).get(str(line), 0)
        if required == sid:
            required = 0  # The spell that opens the line cannot require itself.
        if required and chain.get('req_spell', 0) not in (0, required):
            raise ValueError(f'{key}: conflicting prerequisites for {sid}')
        result = {'spell': sid, 'name': s['SpellName'], 'rank_name': s['Rank1'],
                'chain': first(sid), 'rank': chain.get('rank', 1),
                'previous_spell': chain.get('prev_spell', 0), 'required_spell': required or chain.get('req_spell', 0),
                'skill_line': line, 'tab': lines.get(line, {}).get('tab', 0),
                'skill': skill, 'passive': bool(s['Attributes'] & 0x40)}
        race_rows = spell_abilities.get(sid, [])
        if race_rows and all(a['RaceMask'] for a in race_rows):
            result['race_mask'] = 0
            for ability in race_rows:
                result['race_mask'] |= ability['RaceMask']
        if any(a['ExcludeRace'] for a in race_rows):
            result['exclude_race_mask'] = 0
            for ability in race_rows:
                result['exclude_race_mask'] |= ability['ExcludeRace']
        if definition.get('include_cast_requirements'):
            reagents = [{'item': s[f'Reagent{i}'], 'count': s[f'ReagentCount{i}']}
                        for i in range(1, 9) if s[f'Reagent{i}'] > 0]
            tools = [s[f'Totem{i}'] for i in (1, 2) if s[f'Totem{i}']]
            if reagents:
                result['reagents'] = reagents
            if tools:
                result['totem_items'] = tools
            if any(s[f'EffectImplicitTarget{target}{i}'] == 5 for target in ('A', 'B') for i in (1, 2, 3)) or any(
                    s[f'Effect{i}'] in (101, 109) for i in (1, 2, 3)):
                result['requires_pet'] = True
            pet_spells = [s[f'EffectTriggerSpell{i}'] for i in (1, 2, 3)
                          if s[f'Effect{i}'] == 36 and s[f'EffectImplicitTargetA{i}'] == 5]
            if pet_spells:
                result['teaches_pet_spells'] = pet_spells
        return result

    trainers = [c for c in sql['creature_template'] if c['TrainerType'] in definition.get('trainer_types', [0]) and
                c['TrainerClass'] == class_id and c['NpcFlags'] & 16]
    templates = {c['TrainerTemplateId'] for c in trainers} - {0}
    entries = {c['Entry'] for c in trainers}
    template_rows = [r for r in sql['npc_trainer_template'] if r['entry'] in templates]
    template_keys = {(r['entry'], r['spell']) for r in template_rows}
    creature_templates = {c['Entry']: c['TrainerTemplateId'] for c in trainers}
    direct_rows = [r for r in sql['npc_trainer'] if r['entry'] in entries and
                   (creature_templates[r['entry']], r['spell']) not in template_keys]
    candidates, excluded = {}, {}
    for table, rows in (('npc_trainer_template', template_rows), ('npc_trainer', direct_rows)):
        for row in rows:
            trainer_spell = spells[row['spell']]
            # The core stores the last eligible target for trainer state, but
            # casting the wrapper teaches ALL player-targeted LEARN_SPELL effects.
            # Pet-targeted wrappers are player-known Beast Training entries.
            targets = {trainer_spell[f'EffectTriggerSpell{i}'] for i in (1, 2, 3)
                       if trainer_spell[f'Effect{i}'] == 36 and trainer_spell[f'EffectTriggerSpell{i}'] and
                       trainer_spell[f'EffectImplicitTargetA{i}'] in (0, 1)} or {row['spell']}
            for sid in sorted(targets & by_spell.keys()):
                class_level, not_trainable = class_gates[record(sid)['skill_line']]
                if not_trainable:
                    continue
                if first(sid) in talent_spells:
                    excluded[sid] = {**record(sid), 'source': 'talent', 'required_level': spells[sid]['SpellLevel']}
                    continue
                level = row['reqlevel'] or max(trainer_spell['SpellLevel'], class_level)
                gate = {'table': table, 'trainer_spell': row['spell'],
                        'reqlevel': row['reqlevel'], 'required_level': level,
                        'req_skill': row['reqskill'], 'req_skill_value': row['reqskillvalue'],
                        'required_abilities': [row[f'ReqAbility{i}'] for i in (1, 2, 3) if row[f'ReqAbility{i}']],
                        'condition_id': row['condition_id']}
                candidate = candidates.setdefault(sid, {**record(sid), 'source': 'trainer', 'trainer_options': []})
                option = next((o for o in candidate['trainer_options'] if
                               {k: v for k, v in o.items() if k != 'entries'} == gate), None)
                if option is None:
                    option = {**gate, 'entries': []}
                    candidate['trainer_options'].append(option)
                option['entries'].append(row['entry'])

    for sid, candidate in candidates.items():
        candidate['required_level'] = min(o['required_level'] for o in candidate['trainer_options'])
        candidate['trainer_options'].sort(key=lambda o: (o['required_level'], o['table'], o['entries']))
        for option in candidate['trainer_options']:
            option['entries'].sort()
    for row in sql['playercreateinfo_spell']:
        sid = row['Spell']
        if row['class'] == class_id and sid in by_spell:
            candidates[sid] = {**record(sid), 'source': 'starting', 'required_level': 1, 'trainer_options': []}

    # Identify alternative acquisition routes, without turning them into trainer unlocks.
    for quest in sql['quest_template']:
        if quest['RequiredClasses'] and not quest['RequiredClasses'] & mask:
            continue
        rewards = {quest['RewSpell']} - {0}
        if quest['RewSpellCast']:
            rewards.update(learned(quest['RewSpellCast']))
        for sid in rewards & by_spell.keys() - candidates.keys():
            entry = excluded.setdefault(sid, {**record(sid), 'source': 'quest',
                                              'required_level': quest['MinLevel'], 'quests': []})
            entry.setdefault('quests', []).append(quest['entry'])
    for sid in by_spell.keys() - candidates.keys():
        if first(sid) in talent_spells:
            excluded.setdefault(sid, {**record(sid), 'source': 'talent', 'required_level': spells[sid]['SpellLevel']})
    for item in sql['item_template']:
        for i in range(1, 6):
            if not item[f'spellid_{i}']:
                continue
            for sid in learned(item[f'spellid_{i}']):
                if sid in by_spell and sid not in candidates:
                    entry = excluded.setdefault(sid, {**record(sid), 'source': 'item',
                                                      'required_level': item['RequiredLevel'], 'items': []})
                    entry.setdefault('items', []).append(item['entry'])
    for sid in sorted(by_spell):
        metadata = record(sid)
        if metadata.get('race_mask') or metadata.get('exclude_race_mask'):
            if sid not in candidates and sid not in excluded and sid not in player_learn_targets:
                continue  # Racial triggered effects are not player-learned ranks.
            # A class-only request cannot select the appropriate racial book.
            # Preserve trainer/quest/item evidence, but require explicit acquisition.
            old = candidates.pop(sid, None) or excluded.get(sid, {})
            excluded[sid] = {**metadata, 'required_level': spells[sid]['SpellLevel'], **old,
                             'acquisition_source': old.get('source', 'racial'), 'source': 'racial'}
        elif sid not in candidates and sid not in excluded:
            skill_rows = [a for a in spell_abilities[sid] if a['AcquireMethod'] == 1]
            if skill_rows and 'skill' in definition.get('auto_sources', []):
                if not metadata['required_spell']:
                    raise ValueError(f'{key}: skill-acquired spell {sid} needs a line prerequisite')
                excluded[sid] = {**metadata, 'source': 'skill', 'required_level': spells[sid]['SpellLevel'],
                                 'skill_line_abilities': sorted(a['ID'] for a in skill_rows)}
            elif str(metadata['skill_line']) in definition.get('external_line_sources', {}):
                excluded[sid] = {**metadata, 'source': definition['external_line_sources'][str(metadata['skill_line'])],
                                 'required_level': spells[sid]['SpellLevel'],
                                 'skill_line_abilities': sorted(a['ID'] for a in spell_abilities[sid])}
    order = lambda s: (s['tab'], s['chain'], s['rank'], s['spell'])
    return {'id': class_id, 'name': definition['name'], 'default_skill': definition['default_skill'],
            **{field: definition[field] for field in ('auto_sources', 'wow_skill_mappings') if field in definition},
            'lines': {str(k): v for k, v in lines.items()}, 'spells': sorted(candidates.values(), key=order),
            'excluded': sorted(excluded.values(), key=order),
            'counts': {'class_trainers': len(trainers), 'trainer_rows': len(template_rows) + len(direct_rows),
                       'trainer_ranks': sum(s['source'] == 'trainer' for s in candidates.values()),
                       'starting_ranks': sum(s['source'] == 'starting' for s in candidates.values()),
                       'chains': len({s['chain'] for s in candidates.values()}), 'excluded': len(excluded)}}


def build(dump=DEFAULT_DUMP, dbc_dir=DBC_DIR, config=CONFIG):
    config = Path(config)
    definition = json.loads(config.read_text())
    sql = tables(Path(dump), DUMP_COLUMNS)
    abilities = dbc.read(Path(dbc_dir) / 'SkillLineAbility.dbc')
    lines = dbc.by_id(dbc.read(Path(dbc_dir) / 'SkillLine.dbc'))
    talents = dbc.read(Path(dbc_dir) / 'Talent.dbc')
    skill_race_info = dbc.read(Path(dbc_dir) / 'SkillRaceClassInfo.dbc')
    talent_spells = {s for r in talents for s in r['SpellRank'] if s}
    chains = rank_chains(sql, abilities, talents)
    return {'schema': 1, 'curve': definition['curve'],
            'provenance': {'dump': Path(dump).name, 'dump_sha256': sha256(dump),
                           'mapping_sha256': sha256(config), 'client_build': '1.12.1.5875',
                           'dbc_sha256': {name: sha256(Path(dbc_dir) / f'{name}.dbc')
                                          for name in ('SkillLineAbility', 'SkillLine', 'Talent', 'SkillRaceClassInfo')}},
            'classes': {key: build_class(key, c, sql, abilities, lines, talent_spells, chains, skill_race_info)
                        for key, c in definition['classes'].items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dump', type=Path, default=DEFAULT_DUMP)
    parser.add_argument('--dbc-dir', type=Path, default=DBC_DIR)
    parser.add_argument('--config', type=Path, default=CONFIG)
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    output = build(args.dump, args.dbc_dir, args.config)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(output, indent=2, sort_keys=True) + '\n')
    for key, c in output['classes'].items():
        print(f'{args.out}: {key}: {len(c["spells"])} ranks; {json.dumps(c["counts"], sort_keys=True)}')


if __name__ == '__main__':
    main()
