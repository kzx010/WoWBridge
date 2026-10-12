#!/usr/bin/env python3
"""Export epic ground mount previews from the pinned 1.12 DBC and ClassicDB dump."""
import json
from pathlib import Path
from data import dbc
from data.sql_dump import tables
from data.build_catalog import DBC_DIR, DEFAULT_DUMP, REPO, sha256

SPELLS = (23219, 23220, 23221, 23222, 23223, 23225, 23227, 23228, 23229,
          23238, 23239, 23240, 23241, 23242, 23243, 23246, 23247, 23248, 23249, 23250, 23251)


def build():
    spells = dbc.by_id(dbc.read(DBC_DIR / 'Spell.dbc'))
    icons = dbc.by_id(dbc.read(DBC_DIR / 'SpellIcon.dbc'))
    times = dbc.by_id(dbc.read(DBC_DIR / 'SpellCastTimes.dbc'))
    displays = dbc.by_id(dbc.read(DBC_DIR / 'CreatureDisplayInfo.dbc'))
    models = dbc.by_id(dbc.read(DBC_DIR / 'CreatureModelData.dbc'))
    entries = {spells[s]['EffectMiscValue'][spells[s]['EffectAura'].index(78)] for s in SPELLS}
    creatures = {r['Entry']: r for r in tables(DEFAULT_DUMP,
        {'creature_template': ('Entry', 'Name', 'ModelId1')},
        {'creature_template': lambda r: r['Entry'] in entries})['creature_template']}
    mounts = []
    for sid in SPELLS:
        s = spells[sid]
        entry = s['EffectMiscValue'][s['EffectAura'].index(78)]
        display = creatures[entry]['ModelId1']
        model = models[displays[display]['ModelID']]
        speed = s['EffectBasePoints'][s['EffectAura'].index(32)] + 1
        assert speed == 100 and display > 0
        key = s['Name_lang'].lower().replace(' ', '_')
        mounts.append(dict(key=key, spell=sid, name=s['Name_lang'], creature=entry,
            display=display, model=model['ModelName'], speed_percent=speed,
            cast_ms=times[s['CastingTimeIndex']]['Base'],
            icon=icons[s['SpellIconID']]['TextureFilename']))
    return {'version': 1, 'source': {'spell_sha256': sha256(DBC_DIR / 'Spell.dbc'),
        'classicdb_sha256': sha256(DEFAULT_DUMP)}, 'mounts': mounts}


if __name__ == '__main__':
    out = REPO / 'data/mounts.json'
    out.write_text(json.dumps(build(), indent=2) + '\n')
    print(out)
