"""Deterministic Classic talent trees, client presentation and trainer upgrades.

Run python -m data.build_talents. Inputs are read-only client DBCs/ClassicDB.
"""
import argparse
import json
from pathlib import Path

from data import dbc
from data.build_catalog import DBC_DIR, REPO
from data.build_progression import CONFIG, DEFAULT_DUMP, DUMP_COLUMNS, sha256
from data.sql_dump import tables

DEFAULT_OUT = REPO / 'data/talents.json'
DBC_INPUTS = ('Talent', 'TalentTab', 'Spell', 'SpellIcon')


def build(dump=DEFAULT_DUMP, dbc_dir=DBC_DIR, config=CONFIG):
    client = {name: dbc.read(Path(dbc_dir) / f'{name}.dbc') for name in DBC_INPUTS}
    spells = dbc.by_id(client['Spell'])
    icons = dbc.by_id(client['SpellIcon'])
    sql = tables(Path(dump), {**DUMP_COLUMNS, 'spell_affect': None, 'spell_proc_event': None})
    server_spells = {s['Id']: s for s in sql['spell_template']}
    classes = {}
    for key, definition in json.loads(Path(config).read_text())['classes'].items():
        mask = 1 << (definition['id'] - 1)
        trees = []
        for tab in sorted((t for t in client['TalentTab'] if t['ClassMask'] & mask), key=lambda t: t['OrderIndex']):
            talents = []
            for t in sorted((t for t in client['Talent'] if t['TabID'] == tab['ID']),
                            key=lambda t: (t['TierID'], t['ColumnIndex'], t['ID'])):
                ranks = [sid for sid in t['SpellRank'] if sid]
                if not ranks or any(sid not in server_spells for sid in ranks):
                    raise ValueError(f'missing talent rank for {t["ID"]}')
                s = spells[ranks[0]]
                talents.append({'id': t['ID'], 'name': s['Name_lang'], 'icon': icons[s['SpellIconID']]['TextureFilename'],
                                'tier': t['TierID'], 'column': t['ColumnIndex'], 'max_rank': len(ranks),
                                'rank_spells': ranks, 'descriptions': [spells[sid]['Description_lang'] for sid in ranks],
                                'prerequisites': [{'talent': tid, 'rank': rank + 1}
                                                  for tid, rank in zip(t['PrereqTalent'], t['PrereqRank']) if tid],
                                'required_spell': t['RequiredSpellID'], 'exceptional': bool(t['Flags'] & 1)})
                talents[-1]['granted_spells'] = {
                    str(sid): [server_spells[sid][f'EffectTriggerSpell{e}'] for e in (1, 2, 3)
                               if server_spells[sid][f'Effect{e}'] == 36 and
                               server_spells[sid][f'EffectImplicitTargetA{e}'] in (0, 1)] for sid in ranks}
            trees.append({'id': tab['ID'], 'tab': tab['OrderIndex'] + 1, 'name': tab['Name_lang'],
                          'icon': icons[tab['SpellIconID']]['TextureFilename'], 'background': tab['BackgroundFile'],
                          'background_textures': {corner: 'Interface\\TalentFrame\\' + tab['BackgroundFile'] + '-' + corner
                                                  for corner in ('TopLeft', 'TopRight', 'BottomLeft', 'BottomRight')},
                          'talents': talents})
        if len(trees) != 3:
            raise ValueError(f'{key} has {len(trees)} talent trees')
        # Preserve the trainer's actual alternative gates for talent-granted upgrades.
        trainers = [c for c in sql['creature_template'] if c['TrainerClass'] == definition['id'] and
                    c['TrainerType'] in definition.get('trainer_types', [0]) and c['NpcFlags'] & 16]
        templates = {c['TrainerTemplateId'] for c in trainers}
        entries = {c['Entry']: c['TrainerTemplateId'] for c in trainers}
        template_rows = [r for r in sql['npc_trainer_template'] if r['entry'] in templates]
        overrides = {(r['entry'], r['spell']) for r in template_rows}
        rows = template_rows + [r for r in sql['npc_trainer'] if r['entry'] in entries and
                                (entries[r['entry']], r['spell']) not in overrides]
        options = {}
        for r in rows:
            s = server_spells[r['spell']]
            targets = {s[f'EffectTriggerSpell{e}'] for e in (1, 2, 3) if s[f'Effect{e}'] == 36 and
                       s[f'EffectImplicitTargetA{e}'] in (0, 1) and s[f'EffectTriggerSpell{e}']} or {r['spell']}
            for sid in sorted(targets):
                gate = {'trainer_spell': r['spell'], 'required_level': r['reqlevel'] or s['SpellLevel'],
                        'req_skill': r['reqskill'], 'req_skill_value': r['reqskillvalue'],
                        'required_abilities': [r[f'ReqAbility{e}'] for e in (1, 2, 3) if r[f'ReqAbility{e}']],
                        'condition_id': r['condition_id']}
                if gate not in options.setdefault(str(sid), []):
                    options[str(sid)].append(gate)
        talent_ids = {sid for tree in trees for t in tree['talents'] for sid in t['rank_spells']}
        # Only keep upgrades of talent chains, including non-DBC trainer ranks.
        chains = {r['spell_id']: r['first_spell'] for r in sql['spell_chain']}
        granted = []
        for tree in trees:
            for talent in tree['talents']:
                for sid in sorted({sid for values in talent['granted_spells'].values() for sid in values}):
                    s = server_spells[sid]
                    chain = next((r for r in sql['spell_chain'] if r['spell_id'] == sid), {})
                    granted.append({'spell': sid, 'name': s['SpellName'], 'rank_name': s['Rank1'],
                                    'chain': chain.get('first_spell', sid), 'rank': chain.get('rank', 1),
                                    'previous_spell': chain.get('prev_spell', 0), 'required_spell': chain.get('req_spell', 0),
                                    'skill_line': 0, 'tab': tree['tab'], 'skill': definition['default_skill'],
                                    'source': 'talent', 'required_level': s['SpellLevel'], 'passive': bool(s['Attributes'] & 0x40)})
        classes[key] = {'id': definition['id'], 'name': definition['name'], 'trees': trees, 'granted_spellbook': granted,
                        'trainer_options': {sid: sorted(opts, key=lambda o: json.dumps(o, sort_keys=True))
                                            for sid, opts in sorted(options.items(), key=lambda p: int(p[0]))
                                            if chains.get(int(sid), int(sid)) in talent_ids}}
    talent_spells = {sid for c in classes.values() for tree in c['trees'] for t in tree['talents'] for sid in t['rank_spells']}
    return {'schema': 1, 'provenance': {'client_build': '1.12.1.5875', 'dump_sha256': sha256(dump),
                                      'mapping_sha256': sha256(config),
                                      'dbc_sha256': {name: sha256(Path(dbc_dir) / f'{name}.dbc') for name in DBC_INPUTS}},
            'classes': classes,
            'spell_affect': [r for r in sql['spell_affect'] if r['entry'] in talent_spells],
            'spell_proc_event': [r for r in sql['spell_proc_event'] if r['entry'] in talent_spells]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dump', type=Path, default=DEFAULT_DUMP)
    parser.add_argument('--dbc-dir', type=Path, default=DBC_DIR)
    parser.add_argument('--config', type=Path, default=CONFIG)
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    args.out.write_text(json.dumps(build(args.dump, args.dbc_dir, args.config), indent=2, sort_keys=True) + '\n')
    print(args.out)


if __name__ == '__main__':
    main()
