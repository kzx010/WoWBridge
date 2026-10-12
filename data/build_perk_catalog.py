"""Poison payload supplement: consumable spells absent from the trainer catalog.

Keep the dump's spell fields and the core's coefficients, as build_catalog does.
This does not grant poisons, ingredients, items or consumable charges.
"""
import argparse
import json
from pathlib import Path

from data import dbc
from data.build_catalog import CORE_SPELLS, DBC_DIR, DEFAULT_DUMP, PLAYER_POISON_PAYLOADS, REPO, sha256
from data.sql_dump import tables


PAYLOADS = PLAYER_POISON_PAYLOADS


def poison(s):
    return s['Id'] in PAYLOADS


def build(dump=DEFAULT_DUMP, core_spells=CORE_SPELLS, dbc_dir=DBC_DIR):
    rows = tables(dump, {'spell_template': None}, filters={'spell_template': poison})['spell_template']
    coeffs = tuple(f'{key}{e}' for key in ('EffectBonusCoefficient', 'EffectBonusCoefficientFromAP') for e in (1, 2, 3))
    core = {s['Id']: s for s in tables(core_spells, {'spell_template': ('Id',) + coeffs})['spell_template']}
    spells = {s['Id']: {**s, **{k: s[k] if k in s else core[s['Id']][k] for k in coeffs}} for s in rows}
    if spells.keys() != PAYLOADS:
        raise ValueError(f'missing player poison payloads: {sorted(PAYLOADS - spells.keys())}')
    def index(table, field):
        used = {s[field] for s in spells.values()}
        return {r['ID']: r for r in dbc.read(dbc_dir / f'{table}.dbc') if r['ID'] in used}
    return {'provenance': {'dump_sha256': sha256(dump), 'core_spells_sha256': sha256(core_spells),
                          'dbc_sha256': {t: sha256(dbc_dir / f'{t}.dbc') for t in
                                         ('SpellCastTimes', 'SpellDuration', 'SpellRange', 'SpellIcon')}},
            'spells': spells, 'poison_payloads': sorted(spells),
            'cast_times': index('SpellCastTimes', 'CastingTimeIndex'),
            'durations': index('SpellDuration', 'DurationIndex'),
            'ranges': index('SpellRange', 'RangeIndex'), 'icons': index('SpellIcon', 'SpellIconID')}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dump', type=Path, default=DEFAULT_DUMP)
    ap.add_argument('--core-spells', type=Path, default=CORE_SPELLS)
    ap.add_argument('--dbc-dir', type=Path, default=DBC_DIR)
    ap.add_argument('--out', type=Path, default=REPO / 'data/perk_catalog.json')
    a = ap.parse_args()
    a.out.write_text(json.dumps(build(a.dump, a.core_spells, a.dbc_dir), indent=1, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
