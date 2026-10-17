#!/usr/bin/env python3
"""Pin race stats, starter outfits and racials from ClassicDB, the extracted 1.12 client tables and pinned CMaNGOS.

The Druid's keys keep their rows: levelstats_by_race (Night Elf and Tauren) and starting_outfits. levelstats_by_class and
starting_outfits_by_class carry every playable class (data/build_catalog.py PLAYABLE: the Druid's Night Elf and Tauren,
the Paladin's Human and Dwarf). 'racials' lists the Human's and the Dwarf's General-tab racials: each ability on the
race's racial skill line that playercreateinfo_spell gives its Paladin, with its client effects and how the service and
the plugin treat it ('runtime': 'delivered' once service/racials.py applies it and the plugin carries its Skyrim side,
'pending' while part of that is still missing).
"""
import argparse
import json
from pathlib import Path

from data import dbc
from data.build_catalog import DBC_DIR, DEFAULT_DUMP, PLAYABLE, RACIAL_LINKED_SPELLS, RACIAL_SPELLS, REPO, sha256
from data.build_paladin import CMANGOS, CMANGOS_COMMIT, Source
from data.sql_dump import tables

DRUID, PALADIN = 11, 2
RACIAL_LINES = {1: 754, 3: 101}            # SkillLine.dbc "Racial - Human", "Dwarven Racial"
DBC_INPUTS = ('CharStartOutfit', 'ChrRaces', 'SkillLine', 'SkillLineAbility', 'SkillRaceClassInfo', 'Spell',
              'SpellDuration')
AURAS, PLAYER = 'src/game/Spells/SpellAuras.cpp', 'src/game/Entities/Player.cpp'
UNIT, OBJECTS = 'src/game/Entities/Unit.cpp', 'src/game/Globals/ObjectMgr.cpp'
SPELL_ATTR_PASSIVE = 0x40                  # SpellDefines.h
STATS = ('str', 'agi', 'sta', 'int', 'spi')  # SharedDefines.h Stats: STAT_STRENGTH .. STAT_SPIRIT
WEAPON_SKILL = ('+5 weapon skill on the attack table melee.py rolls (level x 5) while the main hand is of one of these '
                "skills; Skyrim's own One-Handed/Two-Handed skill is not raised")
# How service/racials.py and the plugin (skse/src/Carriers.cpp, Cast) treat each racial. 'type' extends
# racials.passives()'s vocabulary; 'skyrim' says whether an existing route covers it ('converted':
# service/skyrim_effects.py aura_effects or the WoW combat formulas), the plugin has a native carrier of its own
# ('native'), or it stays a tooltip line and a named Skyrim marker ('informational').
TREATMENTS = {
    20598: ('stat_percent', 'converted',
            'Character.build scales total Spirit by the aura as it scales stats for talents (aura 137 misc 4) and '
            'Character.regen reads it; its share of their Spirit is bonus Spirit natively, Fortify Magicka Rate a point a '
            "Spirit point (aura_effects' stat route), the plugin's constant The Human Spirit"),
    20599: ('reputation_gain', 'informational',
            'Player::CalculateReputationGain adds it to every reputation gain; Skyrim keeps no WoW reputation and the '
            'character pane has none to show: a named zero-magnitude Skyrim marker'),
    20600: ('stealth_detection', 'native',
            'a self cast validated as Shadowmeld is (self target) with its 3-minute cooldown; for its 20 seconds the '
            "plugin keeps Skyrim's Detect Life on the player (the Thieves Guild scroll's effects): Skyrim has no stealth "
            'detection value, and Detect Life is its sight of hidden creatures'),
    20597: ('weapon_skill', 'converted', WEAPON_SKILL + ' (a named zero-magnitude Skyrim marker)'),
    20864: ('weapon_skill', 'converted', WEAPON_SKILL + ' (a named zero-magnitude Skyrim marker)'),
    20595: ('weapon_skill', 'informational',
            'no playable class of the race here has the skill (SkillRaceClassInfo gives Guns to Warriors, Rogues and '
            'Hunters): the bonus is listed but never applies, as in WoW; Skyrim has no guns: a named zero-magnitude '
            'marker'),
    2481: ('tracking', 'native',
           'a toggled tracking aura (PLAYER_TRACK_RESOURCES bit for lock type 6, Treasure): Cast keeps it as one '
           'tracker until cancelled; marking treasure containers on the minimap needs resource blips there (pending)'),
    20594: ('stoneform', 'native',
            "armor +10% of their worn armor (Cast's armor_percent, as Enrage's); Resist Poison and Resist Disease 100 "
            "(Skyrim's own immunity, Hevnoraak's); applying its immunities purges active poison, disease and bleed "
            'effects (SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT) and refuses new ones for 8 seconds: the plugin ends their '
            "Skyrim poisons, diseases and Bleeding Damage while it lasts"),
    20596: ('resistance', 'converted',
            "as the Night Elf's and Tauren's Nature Resistance: Character.resistance, and gear's /3 rate as Resist "
            "Frost (aura_effects' frost_res; the plugin's constant Frost Resistance)"),
}
# What is still missing at runtime (every other racial is 'delivered').
PENDING = {2481: 'treasure containers on the minimap'}


def effects(spell, durations):
    """A Spell.dbc row's effects with WorldObject::CalculateSpellEffectValue's fixed amount (base points plus the base
    die where it cannot roll; none of these scale with level), its duration and its cooldown."""
    out = []
    for e in range(3):
        if not spell['Effect'][e]:
            continue
        if spell['EffectRealPointsPerLevel'][e] or spell['EffectDieSides'][e] > 1:
            raise ValueError(f"{spell['ID']}: effect {e + 1} rolls or scales; its amount is not fixed")
        out.append({'effect': spell['Effect'][e], 'aura': spell['EffectAura'][e], 'misc': spell['EffectMiscValue'][e],
                    'amount': spell['EffectBasePoints'][e] + spell['EffectBaseDice'][e]})
    duration = durations[spell['DurationIndex']]['Duration'] if spell['DurationIndex'] else -1
    return {'effects': out, 'duration_ms': duration, 'cooldown_ms': spell['RecoveryTime']}


def treatment(sid, race, row, linked, lines, skill_classes):
    """What service/racials.py would compute for the racial: its type, the amounts it reads, and the Skyrim route."""
    kind, skyrim, note = TREATMENTS[sid]
    first = row['effects'][0]
    out = {'type': kind, 'skyrim': skyrim, 'note': note, 'runtime': 'pending' if sid in PENDING else 'delivered'}
    if kind == 'stat_percent':
        out.update(stat=STATS[first['misc']], amount=first['amount'])
    elif kind in ('reputation_gain', 'stealth_detection'):
        out['amount'] = first['amount']
    elif kind == 'weapon_skill':
        out.update(amount=first['amount'],
                   skills=[{'skill': e['misc'], 'name': lines[e['misc']]['DisplayName_lang'],
                            'classes': skill_classes(e['misc'], race)} for e in row['effects']])
    elif kind == 'tracking':
        out['lock_type'] = first['misc']
    elif kind == 'resistance':
        out.update(amount=first['amount'], school_mask=first['misc'])
    elif kind == 'stoneform':
        every = row['effects'] + [e for spell in linked for e in spell['effects']]
        out.update(armor_percent=next(e['amount'] for e in every if e['aura'] == 101 and e['misc'] == 1),
                   dispel_immunity=sorted(e['misc'] for e in every if e['aura'] == 41),
                   mechanic_immunity=sorted(e['misc'] for e in every if e['aura'] == 77))
    if not row['passive']:
        out.update(duration_ms=row['duration_ms'], cooldown_ms=row['cooldown_ms'])
    return out


def build(dump=DEFAULT_DUMP, dbc_dir=DBC_DIR, cmangos=CMANGOS):
    dbc_dir, src = Path(dbc_dir), Source(cmangos)
    sql = tables(dump, {'player_levelstats': None, 'playercreateinfo_spell': None},
                 filters={'player_levelstats': lambda r: r['race'] in PLAYABLE.get(r['class'], ()),
                          'playercreateinfo_spell': lambda r: r['race'] in PLAYABLE[PALADIN] and r['class'] == PALADIN})
    rows = sql['player_levelstats']
    outfits = dbc.read(dbc_dir / 'CharStartOutfit.dbc')
    spells, durations = dbc.by_id(dbc.read(dbc_dir / 'Spell.dbc')), dbc.by_id(dbc.read(dbc_dir / 'SpellDuration.dbc'))
    lines = dbc.by_id(dbc.read(dbc_dir / 'SkillLine.dbc'))
    abilities = dbc.read(dbc_dir / 'SkillLineAbility.dbc')
    skill_info = dbc.read(dbc_dir / 'SkillRaceClassInfo.dbc')
    names = {sid: key for key, sid in RACIAL_SPELLS.items()}

    def skill_classes(skill, race):
        """The playable classes of the race that SkillRaceClassInfo lets have the skill."""
        return sorted(c for c, races in PLAYABLE.items() if race in races and any(
            r['SkillID'] == skill and r['RaceMask'] & 1 << race - 1 and r['ClassMask'] & 1 << c - 1 for r in skill_info))

    # Stoneform's code-linked boost (SpellAuraHolder::HandleSpellSpecificBoosts), the line after its case.
    boost = src.cite(AURAS, 'case 20594:', after='void SpellAuraHolder::HandleSpellSpecificBoosts')
    if src.text(AURAS)[int(boost.rsplit(':', 1)[1])].strip() != f'boostSpells.push_back({RACIAL_LINKED_SPELLS[0]});':
        raise ValueError('Stoneform no longer boosts the linked spell')
    racials = {}
    for race in PLAYABLE[PALADIN]:
        created = {r['Spell'] for r in sql['playercreateinfo_spell'] if r['race'] == race}
        own = sorted({a['Spell'] for a in abilities if a['SkillLine'] == RACIAL_LINES[race] and a['RaceMask'] & 1 << race - 1
                      and a['Spell'] in created})
        if set(own) - TREATMENTS.keys() or set(own) - names.keys():
            raise ValueError(f'race {race}: racials without a treatment or catalog key: {sorted(set(own) - TREATMENTS.keys())}')
        racials[str(race)] = []
        for sid in own:
            s = spells[sid]
            row = {'spell': sid, 'key': names[sid], 'name': s['Name_lang'], 'skill_line': RACIAL_LINES[race],
                   'passive': bool(s['Attributes'] & SPELL_ATTR_PASSIVE), **effects(s, durations)}
            linked = [{'spell': lid, 'name': spells[lid]['Name_lang'], **effects(spells[lid], durations)}
                      for lid in (RACIAL_LINKED_SPELLS if sid == 20594 else [])]
            # The aura handlers (SpellAuras.cpp's table), then the code each racial reaches.
            evidence = [src.cite(AURAS, f"// {e['aura']} SPELL_AURA_")
                        for e in row['effects'] + [e for spell in linked for e in spell['effects']]]
            if sid == 20594:
                evidence += [boost,
                             src.cite(UNIT, 'RemoveAurasWithDispelType(type)', after='void Unit::ApplySpellDispelImmunity'),
                             src.cite(AURAS, 'RemoveAurasAtMechanicImmunity(mechanic, GetId())',
                                      after='void Aura::HandleModMechanicImmunity(bool')]
            evidence += {20598: [src.cite(AURAS, 'void Aura::HandleModTotalPercentStat(')],
                         20599: [src.cite(PLAYER, 'GetTotalAuraModifier(SPELL_AURA_MOD_REPUTATION_GAIN)')],
                         20600: [src.cite(AURAS, 'AddStealthDetectionStrength(')],
                         2481: [src.cite(AURAS, 'SetFlag(PLAYER_TRACK_RESOURCES')],
                         20596: [src.cite(AURAS, 'void Aura::HandleAuraModResistance(')]}.get(sid, [])
            if TREATMENTS[sid][0] == 'weapon_skill':
                evidence.append(src.cite(PLAYER, 'SPELL_AURA_MOD_SKILL, SPELL_AURA_MOD_SKILL_TALENT'))
            racials[str(race)].append({**row, **({'linked': linked} if linked else {}),
                                       'service': treatment(sid, race, row, linked, lines, skill_classes),
                                       'evidence': list(dict.fromkeys(evidence))})
    evidence = {
        'levelstats': [src.cite(PLAYER, 'GetPlayerLevelInfo(getRace(), plClass, level, &info)',
                                after='void Player::InitStatsForLevel'),
                       src.cite(OBJECTS, 'FROM player_levelstats')],
        'starting_outfits': [src.cite(PLAYER, 'entry->RaceClassGender == raceClassGender')],
        'racials': [src.cite(OBJECTS, 'FROM playercreateinfo_spell')],
    }
    return {'provenance': {'client_build': '1.12.1.5875', 'dump_sha256': sha256(dump),
                           'dbc_sha256': {name: sha256(dbc_dir / f'{name}.dbc') for name in DBC_INPUTS},
                           'cmangos_commit': CMANGOS_COMMIT,
                           'cmangos_sha256': {p: sha256(src.root / p) for p in sorted(src.cited)}},
            # The Druid's, as service/racials.levelstats reads them.
            'levelstats_by_race': {str(race): {str(r['level']): r for r in rows if r['race'] == race and r['class'] == DRUID}
                                   for race in PLAYABLE[DRUID]},
            'starting_outfits': [r for r in outfits if r['RaceID'] in PLAYABLE[DRUID] and r['ClassID'] == DRUID],
            # Every playable class: class -> race -> level -> player_levelstats row; class -> CharStartOutfit rows.
            'levelstats_by_class': {str(c): {str(race): {str(r['level']): r for r in rows if r['race'] == race and r['class'] == c}
                                             for race in races} for c, races in PLAYABLE.items()},
            'starting_outfits_by_class': {str(c): [r for r in outfits if r['RaceID'] in races and r['ClassID'] == c]
                                          for c, races in PLAYABLE.items()},
            'racials': racials,
            'evidence': evidence}


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dump', type=Path, default=DEFAULT_DUMP)
    ap.add_argument('--dbc-dir', type=Path, default=DBC_DIR)
    ap.add_argument('--defs', type=Path, default=dbc.DEFS)
    ap.add_argument('--cmangos', type=Path, default=CMANGOS)
    ap.add_argument('--out', type=Path, default=REPO / 'data/racial_data.json')
    args = ap.parse_args()
    dbc.DEFS = args.defs
    args.out.write_text(json.dumps(build(args.dump, args.dbc_dir, args.cmangos), indent=1, sort_keys=True) + '\n')
