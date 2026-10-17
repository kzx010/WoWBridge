#!/usr/bin/env python3
"""Export epic ground and flying mount previews from the pinned 1.12 DBC and ClassicDB dump."""
import json
from pathlib import Path
from data import dbc
from data.sql_dump import tables
from data.build_catalog import DBC_DIR, DEFAULT_DUMP, REPO, sha256

SPELLS = (23219, 23220, 23221, 23222, 23223, 23225, 23227, 23228, 23229,
          23238, 23239, 23240, 23241, 23242, 23243, 23246, 23247, 23248, 23249, 23250, 23251)
# TBC-style epic flyers have no 1.12 spell. Each keeps a TBC spell id that the 1.12 Spell.dbc lacks
# (Golden Gryphon names the taxi gryphon's skin; no Swift Golden Gryphon exists), the 1.12 creature
# whose model it rides, a 1.12 icon (the client has no gryphon or wind rider icon) and its kind.
FLYERS = (
    (32235, 'Swift Golden Gryphon', 541, 'Ability_Hunter_Pet_Owl', 'Gryphon', 'GryphonAggro'),         # the taxi Riding Gryphon
    (32295, 'Swift Green Wind Rider', 5934, 'Ability_Hunter_Pet_Bat', 'Wind Rider', 'WyvernAggro'),  # Heartrazor's green wyvern
)   # the last: its summoning call, the creature's own (no 1.12 spell kit has one)
FLIGHT_SPEED_PERCENT = 280  # TBC epic flying
# Class mounts: summoned by a class spell she learns (the Paladin's librams), costing mana, at their own speed (aura 32:
# Summon Warhorse +60%, Summon Charger +100%). No reins.
CLASS_MOUNTS = ((13819, 2), (23214, 2))


def build():
    spells = dbc.by_id(dbc.read(DBC_DIR / 'Spell.dbc'))
    icons = dbc.by_id(dbc.read(DBC_DIR / 'SpellIcon.dbc'))
    times = dbc.by_id(dbc.read(DBC_DIR / 'SpellCastTimes.dbc'))
    displays = dbc.by_id(dbc.read(DBC_DIR / 'CreatureDisplayInfo.dbc'))
    models = dbc.by_id(dbc.read(DBC_DIR / 'CreatureModelData.dbc'))
    entries = {spells[s]['EffectMiscValue'][spells[s]['EffectAura'].index(78)] for s in SPELLS}
    entries |= {f[2] for f in FLYERS}
    entries |= {spells[s]['EffectMiscValue'][spells[s]['EffectAura'].index(78)] for s, _ in CLASS_MOUNTS}
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
    # Flyers share the swift ground mounts' summon (cast time) and ground speed.
    ground = mounts[0]
    textures = {r['TextureFilename'] for r in icons.values()}
    sounds = {r['Name']: r['ID'] for r in dbc.read(DBC_DIR / 'SoundEntries.dbc')}
    for sid, name, entry, icon, kind, call in FLYERS:
        assert sid not in spells, f'{sid} is a 1.12 spell'
        display = creatures[entry]['ModelId1']
        model = models[displays[display]['ModelID']]
        icon = 'Interface\\Icons\\' + icon
        assert display > 0 and icon in textures
        mounts.append(dict(key=name.lower().replace(' ', '_'), spell=sid, name=name, creature=entry,
            display=display, model=model['ModelName'], speed_percent=ground['speed_percent'],
            cast_ms=ground['cast_ms'], icon=icon, flying=True, flight_speed_percent=FLIGHT_SPEED_PERCENT,
            summon_sound=sounds[call],
            description=f'Summons and dismisses a rideable {kind}.  This is a very fast mount that can fly outdoors.'))
    for sid, cls in CLASS_MOUNTS:
        s = spells[sid]
        entry = s['EffectMiscValue'][s['EffectAura'].index(78)]
        display = creatures[entry]['ModelId1']
        model = models[displays[display]['ModelID']]
        speed = s['EffectBasePoints'][s['EffectAura'].index(32)] + 1
        assert display > 0 and speed in (60, 100) and s['ManaCost'] > 0
        mounts.append(dict(key=s['Name_lang'].lower().replace(' ', '_'), spell=sid, name=s['Name_lang'], creature=entry,
            display=display, model=model['ModelName'], speed_percent=speed,
            cast_ms=times[s['CastingTimeIndex']]['Base'], icon=icons[s['SpellIconID']]['TextureFilename'],
            **{'class': cls, 'level': s['SpellLevel'], 'mana': s['ManaCost']}))
    return {'version': 1, 'source': {'spell_sha256': sha256(DBC_DIR / 'Spell.dbc'),
        'classicdb_sha256': sha256(DEFAULT_DUMP)}, 'mounts': mounts}


if __name__ == '__main__':
    out = REPO / 'data/mounts.json'
    out.write_text(json.dumps(build(), indent=2) + '\n')
    print(out)
