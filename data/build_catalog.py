#!/usr/bin/env python3
"""Build build/catalog.json for configured class ranks from the ClassicDB dump,
CMaNGOS original_data coefficients, and 1.12.1 client DBCs. The legacy slice
remains a maximum-rank index. Only explicit --character refreshes the live
realm character snapshot (looks and equipment) used by the SKSE plugin.

usage: build_catalog.py [--out build/catalog.json] [--character NAME]
"""
import argparse, hashlib, json, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from data import dbc, worlddb  # noqa: E402
from data.sql_dump import tables  # noqa: E402

DBC_DIR = Path(__file__).resolve().parents[2] / 'WoW/realm/extracted-data/dbc'
DEFAULT_DUMP = Path(__file__).resolve().parents[2] / 'WoW/cmangos/classic-db/Full_DB/ClassicDB_1_12_1_z2815.sql.gz'
CORE_SPELLS = Path(__file__).resolve().parents[2] / 'WoW/cmangos/mangos-classic/sql/base/dbc/original_data/Spell.sql'
REPO = Path(__file__).resolve().parent.parent

SLICE_SPELLS = {  # max ranks for a level-60 druid (verified against spell_template)
    'wrath': 9912, 'moonfire': 9835, 'rejuvenation': 25299, 'healing_touch': 25297,
    'entangling_roots': 9853, 'hurricane': 17402,
    # The rest of her caster-form book (her spellbook, the realm's character): balance, restoration, utility.
    'starfire': 25298, 'regrowth': 9858, 'faerie_fire': 9907, 'hibernate': 18658, 'barkskin': 22812,
    'innervate': 29166, 'mark_of_the_wild': 9885, 'gift_of_the_wild': 21850, 'thorns': 9910, 'tranquility': 9863,
    'rebirth': 20748, 'remove_curse': 2782, 'abolish_poison': 2893, 'cure_poison': 8946, 'soothe_animal': 9901,
    # Her forms and their abilities (feral; the forms a level-60 druid knows, max ranks).
    'bear_form': 5487, 'dire_bear_form': 9634, 'cat_form': 768, 'travel_form': 783, 'aquatic_form': 1066,
    'maul': 9881, 'swipe': 9908, 'growl': 6795, 'demoralizing_roar': 9898, 'bash': 8983, 'enrage': 5229,
    'frenzied_regeneration': 22896, 'claw': 9850, 'shred': 9830, 'rake': 9904, 'rip': 9896, 'ferocious_bite': 31018,
    'prowl': 9913, 'pounce': 9827, 'ravage': 9867, 'tigers_fury': 9846, 'dash': 9821, 'cower': 9892,
    'faerie_fire_feral': 17392,
}
# General-tab racials are separate from the class-rank/balance slice: Night Elf, Tauren, then the Paladin's Human and
# Dwarf (SkillLineAbility lines 754 and 101; data/build_racial_data.py exports how the service would treat each).
RACIAL_SPELLS = {'shadowmeld': 20580, 'quickness': 20582, 'nature_resistance': 20583, 'wisp_spirit': 20585,
                 'war_stomp': 20549, 'endurance': 20550, 'tauren_nature_resistance': 20551, 'cultivation': 20552,
                 'perception': 20600, 'diplomacy': 20599, 'the_human_spirit': 20598, 'sword_specialization': 20597,
                 'mace_specialization': 20864, 'stoneform': 20594, 'frost_resistance': 20596,
                 'gun_specialization': 20595, 'find_treasure': 2481}
# SpellAuraHolder::HandleSpellSpecificBoosts (SpellAuras.cpp): Stoneform's code-linked boost (bleed and disease
# immunity), not a trigger.
RACIAL_LINKED_SPELLS = [20612]
# Every playable class and its races (ChrClasses / ChrRaces ids): Player::InitStatsForLevel reads
# ObjectMgr::GetPlayerLevelInfo(race, class, level) and GetPlayerClassLevelInfo(class, level), which
# ObjectMgr::LoadPlayerInfo fills from player_levelstats and player_classlevelstats.
PLAYABLE = {11: (4, 6), 2: (1, 3)}                       # druid: Night Elf, Tauren; paladin: Human, Dwarf
# Spells the core links to others in code, not by trigger (Aura::HandleShapeshiftBoosts, Spell::EffectDummy): the
# forms' passives (Bear, Cat, Dire Bear, Aquatic, Travel), their effect spells, and the released Night Elf wisp.
LINKED_SPELLS = [1178, 3025, 5419, 5421, 9635, 21178, 22845, 25503, 20584]
# Aura::HandleShapeshiftBoosts/HandleAuraModShapeshift code links, not DBC triggers.
TALENT_LINKED_SPELLS = [24905, 24932, 24900, 24899, 17099, 17057, 24867, 24864]
# Consumable combat payloads have no trainer entry or spell trigger on their recipe.
PLAYER_POISON_PAYLOADS = {8680, 8685, 8689, 11335, 11336, 11337, 2818, 2819, 11353, 11354, 25349}
# Class manifests (data/build_paladin.py): every spell a class's ranks reach, code links included (seal procs and
# judgements, Holy Light's heal, Forbearance), each link with its CMaNGOS evidence there.
CLASS_MANIFESTS = (REPO / 'data/paladin_manifest.json',)
EQUIPMENT = REPO / 'data/equipment.json'
CHARACTER = {'race': 4, 'class': 11, 'level': 60}                         # night elf druid
MAX_LEVEL = 60                                                            # player_levelstats rows a class/race has
# What the plugin needs of each worn item (item_template): its model, where it sheathes, how it swings and sounds.
WORN_FIELDS = ('displayid', 'InventoryType', 'class', 'subclass', 'sheath', 'Material', 'delay')


def character(name: str) -> dict:
    """The realm's character: race, class, sex, level, the looks packed in playerBytes (skin, face, hair style, hair
    colour) and playerBytes2 (facial hair), and the equipped items (bag 0, slots 0-18) with their item_template rows."""
    c = worlddb.query(f"SELECT guid, race, class, gender, level, playerBytes, playerBytes2 FROM characters WHERE name='{name}'",
                      'characters')
    if not c:
        raise SystemExit(f'no character {name!r} on the realm')
    c = c[0]
    b, b2 = c['playerBytes'], c['playerBytes2']
    slots = worlddb.query(f"SELECT slot, item_template FROM character_inventory WHERE guid={c['guid']} AND bag=0 AND slot < 19 "
                          'ORDER BY slot', 'characters')
    rows = {r['entry']: r for r in worlddb.query('SELECT entry, ' + ', '.join(WORN_FIELDS) + ' FROM item_template WHERE entry IN '
                                                 f"({worlddb.ids(s['item_template'] for s in slots)})")}
    # Her action bar, as the server keeps it (character_action: button 0 is slot 1; type 0 a spell).
    actions = worlddb.query(f"SELECT button, action FROM character_action WHERE guid={c['guid']} AND type=0 ORDER BY button",
                            'characters')
    return {'name': name, 'race': c['race'], 'class': c['class'], 'sex': c['gender'], 'level': c['level'],
            'skin': b & 0xFF, 'face': b >> 8 & 0xFF, 'hairStyle': b >> 16 & 0xFF, 'hairColor': b >> 24 & 0xFF,
            'facialHair': b2 & 0xFF,
            'actions': [{'slot': a['button'] + 1, 'spell': a['action']} for a in actions],
            'equipment': [{'slot': s['slot'], 'entry': s['item_template'], **{k: rows[s['item_template']][k] for k in WORN_FIELDS}}
                          for s in slots]}


def sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def equipment(path: Path, client_sets: list[dict], item_rows: dict) -> tuple[list[dict], list[dict], dict]:
    """Expand client sets and apply Skyrim's gear requirement policy to item_template."""
    selection = json.loads(Path(path).read_text())
    by_id = {s['ID']: s for s in client_sets}
    entries = set(selection['items'])
    groups = {}
    for key, sid in selection['sets'].items():
        if not key or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789_' for c in key):
            raise ValueError(f'invalid console set key {key!r}')
        row = by_id[sid]
        members = [entry for entry in row['ItemID'] if entry]
        if not members or any(item_rows[entry]['itemset'] != sid for entry in members):
            raise ValueError(f'set {key}: client membership disagrees with item_template')
        groups[key] = {'id': sid, 'name': row['Name_lang'], 'entries': members}
        entries.update(members)
    items = [{**item_rows[entry], 'RequiredLevel': selection.get('required_level', 0)}
             for entry in sorted(entries)]
    # Individual selections can also belong to sets: retain their bonuses.
    sets = [s for s in client_sets if entries.intersection(s['ItemID'])]
    return items, sets, groups


# z2817/z2818 moved spell_bonus_data into per-effect columns. Read those
# columns from the core's authoritative original_data; keep the dump's
# remaining spell fields. Missing coefficients are an input error.
COEFFICIENTS = tuple(f'{field}{e}' for field in ('EffectBonusCoefficient', 'EffectBonusCoefficientFromAP') for e in (1, 2, 3))


def closure(wanted, spell_rows: dict, core: dict, bonuses: dict, first=lambda sid: sid) -> dict:
    """These spells and every spell they trigger, as the service reads them: the dump's rows with the core's
    coefficient columns (data/build_armory.py closes its items' spells the same way)."""
    wanted, spells = set(wanted), {}
    while wanted - spells.keys():
        rows = [spell_rows[sid] for sid in sorted(wanted - spells.keys()) if sid in spell_rows]
        for r in rows:
            s = {**r, **{k: r[k] if k in r else core[r['Id']][k] for k in COEFFICIENTS}}
            # The original_data AP columns are zero for these chains. The old
            # dump's spell_bonus_data retains the realm's explicit AP override,
            # inherited by rank, including triggered chains such as Pounce bleed.
            bonus = bonuses.get(r['Id'], bonuses.get(first(r['Id'])))
            if bonus:
                for e in (1, 2, 3):
                    key = f'EffectBonusCoefficientFromAP{e}'
                    if key not in r:
                        periodic = s[f'EffectApplyAuraName{e}'] in (3, 8)
                        if periodic or s[f'Effect{e}'] in (2, 10):
                            s[key] = bonus['ap_dot_bonus' if periodic else 'ap_bonus']
            spells[r['Id']] = s
        missing = (wanted - spells.keys())
        if missing and not rows:
            raise SystemExit(f'spells missing from spell_template: {sorted(missing)}')
        wanted |= {r[f'EffectTriggerSpell{k}'] for r in rows for k in (1, 2, 3) if r[f'EffectTriggerSpell{k}']}
    return spells


def spell_tables(spells: dict, dbc_dir: Path) -> dict:
    """The client tables' rows these spells use: cast times, durations, ranges, radii and icons."""
    def index(table: str, key: str) -> dict:
        used = {s[key] for s in spells.values() if s.get(key)}
        return {r['ID']: r for r in dbc.read(dbc_dir / f'{table}.dbc') if r['ID'] in used}

    radii = {s[f'EffectRadiusIndex{k}'] for s in spells.values() for k in (1, 2, 3) if s[f'EffectRadiusIndex{k}']}
    return {'cast_times': index('SpellCastTimes', 'CastingTimeIndex'), 'durations': index('SpellDuration', 'DurationIndex'),
            'ranges': index('SpellRange', 'RangeIndex'),
            'radii': {r['ID']: r for r in dbc.read(dbc_dir / 'SpellRadius.dbc') if r['ID'] in radii},
            'icons': index('SpellIcon', 'SpellIconID')}


def build(dump: Path = DEFAULT_DUMP, dbc_dir: Path = DBC_DIR,
          progression: Path = REPO / 'data/progression.json', core_spells: Path = CORE_SPELLS,
          equipment_path: Path = EQUIPMENT, manifests=CLASS_MANIFESTS) -> dict:
    # The progression artifact supplies chains that SQL alone misses (feral DBC
    # forward links), for every configured class and every acquisition route.
    book = json.loads(Path(progression).read_text())
    ranks = {row['spell']: {k: row[k] for k in ('chain', 'rank', 'required_level', 'skill', 'passive')}
             for definition in book['classes'].values()
             for row in definition['spells'] + definition.get('excluded', [])}
    sql = tables(dump, {name: None for name in (
        'item_template', 'spell_template', 'spell_affect', 'spell_proc_event', 'spell_facing',
        'creature_template_classlevelstats', 'player_levelstats', 'player_classlevelstats', 'db_version',
        'spell_bonus_data', 'spell_chain')},
        filters={'player_levelstats': lambda r: r['race'] in PLAYABLE.get(r['class'], ()),
                 'player_classlevelstats': lambda r: r['class'] in PLAYABLE,
                 'creature_template_classlevelstats': lambda r: r['Class'] == 1})
    creature_path = REPO / 'data/creature_reference.json'
    creatures = json.loads(creature_path.read_text())['creatures']
    for r in sql['creature_template_classlevelstats']:
        creatures[str(r['Level'])] = {'armor': r['BaseArmor'],
                                     'str': r['Strength'] if 'Strength' in r else creatures[str(r['Level'])]['str']}
    item_rows = {r['entry']: r for r in sql['item_template']}
    spell_rows = {r['Id']: r for r in sql['spell_template']}
    core = {r['Id']: r for r in tables(core_spells, {'spell_template': ('Id',) + COEFFICIENTS})['spell_template']}
    bonuses = {r['entry']: r for r in sql['spell_bonus_data']}
    previous = {r['SupercededBySpell']: r['Spell'] for r in dbc.read(dbc_dir / 'SkillLineAbility.dbc')
                if r['SupercededBySpell']}
    roots = {r['spell_id']: r['first_spell'] for r in sql['spell_chain']}
    roots.update({sid: row['chain'] for sid, row in ranks.items()})

    def first(sid):
        seen = set()
        while sid not in roots and sid in previous:
            if sid in seen:
                raise ValueError(f'cyclic DBC rank chain at {sid}')
            seen.add(sid)
            sid = previous[sid]
        return roots.get(sid, sid)
    items, sets, equipment_sets = equipment(equipment_path, dbc.read(dbc_dir / 'ItemSet.dbc'), item_rows)
    # Include use and chance-on-hit descriptions/triggers as well as equip effects.
    item_spells = {it[f'spellid_{k}'] for it in items for k in range(1, 6) if it[f'spellid_{k}']}
    set_spells = {sp for s in sets for sp in s['SetSpellID'] if sp}

    # Close over triggered spells.
    talent_spells = {sid for t in dbc.read(dbc_dir / 'Talent.dbc') for sid in t['SpellRank'] if sid}
    class_spells = {int(sid) for path in manifests for sid in json.loads(Path(path).read_text())['spells']}
    spells = closure(set(SLICE_SPELLS.values()) | set(RACIAL_SPELLS.values()) | set(RACIAL_LINKED_SPELLS) | set(ranks) |
                     set(LINKED_SPELLS) | set(TALENT_LINKED_SPELLS) | talent_spells | item_spells | set_spells |
                     PLAYER_POISON_PAYLOADS | class_spells, spell_rows, core, bonuses, first)
    visual = dbc.by_id(dbc.read(dbc_dir / 'SpellVisual.dbc'))
    kits = dbc.by_id(dbc.read(dbc_dir / 'SpellVisualKit.dbc'))
    effect_names = dbc.by_id(dbc.read(dbc_dir / 'SpellVisualEffectName.dbc'))
    visuals = {}
    for s in spells.values():
        v = visual.get(s['SpellVisual'])
        if not v:
            continue
        kit_ids = {v[k] for k in ('PrecastKit', 'CastKit', 'ImpactKit', 'StateKit', 'ChannelKit') if v.get(k)}
        visuals[s['SpellVisual']] = {
            'visual': v,
            'kits': {k: kits[k] for k in kit_ids if k in kits},
            'effects': {e: effect_names[e] for k in kit_ids if k in kits for e in
                        [kits[k][f] for f in ('HeadEffect', 'ChestEffect', 'BaseEffect', 'LeftHandEffect', 'RightHandEffect', 'BreathEffect', 'WorldEffect')]
                        + list(kits[k]['SpecialEffect']) + [v.get('MissileModel', 0)] if e and e in effect_names},
        }

    return {
        'provenance': {
            'dump_sha256': sha256(dump),
            'core_spells_sha256': sha256(core_spells),
            'creature_reference_sha256': sha256(creature_path),
            'progression_sha256': sha256(progression),
            'equipment_sha256': sha256(equipment_path),
            'manifest_sha256': {Path(path).name: sha256(path) for path in manifests},
            'client_build': '1.12.1.5875',
            'world_db': sql['db_version'][0]['version'],
            'dbc_sha256': {t: sha256(dbc_dir / f'{t}.dbc') for t in ('Talent', 'ItemSet', 'SkillLineAbility', 'SpellCastTimes', 'SpellDuration', 'SpellRange', 'SpellRadius', 'SpellIcon', 'SpellVisual', 'SpellVisualKit', 'SpellVisualEffectName')},
        },
        'slice': SLICE_SPELLS,
        'racials': RACIAL_SPELLS,
        'ranks': ranks,
        'spells': spells,
        'poison_payloads': sorted(PLAYER_POISON_PAYLOADS),
        'spell_affect': [r for r in sql['spell_affect'] if r['entry'] in spells],
        'spell_proc_event': [r for r in sql['spell_proc_event'] if r['entry'] in spells],
        'spell_facing': [r for r in sql['spell_facing'] if r['entry'] in spells],
        **spell_tables(spells, dbc_dir),
        'visuals': visuals,
        'items': {it['entry']: it for it in items},
        'equipment_sets': equipment_sets,
        # What the spells consume (Rebirth's Ironwood Seed, Gift of the Wild's Wild Thornroot): not gear.
        'reagents': {sid: {k: item_rows[sid][k] for k in
                          ('entry', 'name', 'displayid', 'class', 'subclass', 'stackable', 'BuyPrice', 'SellPrice', 'Quality')}
                     for sid in sorted({s[f'Reagent{k}'] for s in spells.values() for k in range(1, 9)
                                        if s[f'Reagent{k}'] > 0})},
        'item_sets': {s['ID']: s for s in sets},
        # Skyrim actors are creatures of their level with CMaNGOS's class 1 (warrior, as beasts) base stats.
        'creatures': creatures,
        'character': character_stats(sql['player_levelstats'], sql['player_classlevelstats']),
    }


def character_stats(levelstats: list[dict], classlevelstats: list[dict]) -> dict:
    """The catalog's character: the Druid's keys as they always were (her Night Elf and Tauren rows, the Night Elf's
    level 60), and the class-aware tables for every playable class (PLAYABLE): levelstats_by_class (class -> race ->
    level -> player_levelstats row) and classlevelstats_by_class (class -> level -> player_classlevelstats row: basehp,
    basemana). Every class and race must have each level from 1 to 60 exactly once."""
    def complete(rows, what):
        if sorted(r['level'] for r in rows) != list(range(1, MAX_LEVEL + 1)):
            raise ValueError(f'{what}: levels 1-{MAX_LEVEL} incomplete or repeated')
        return {r['level']: r for r in rows}
    by_class = {cls: {race: complete([r for r in levelstats if r['class'] == cls and r['race'] == race],
                                     f'player_levelstats class {cls} race {race}') for race in races}
                for cls, races in PLAYABLE.items()}
    class_by_class = {cls: complete([r for r in classlevelstats if r['class'] == cls], f'player_classlevelstats class {cls}')
                      for cls in PLAYABLE}
    own, own_class = by_class[CHARACTER['class']], class_by_class[CHARACTER['class']]
    return {
        **CHARACTER,
        'levelstats_by_race': {race: own[race] for race in PLAYABLE[CHARACTER['class']]},
        'levelstats_by_level': own[CHARACTER['race']],
        'classlevelstats_by_level': own_class,
        'levelstats': own[CHARACTER['race']][CHARACTER['level']],
        'classlevelstats': own_class[CHARACTER['level']],
        'levelstats_by_class': by_class,
        'classlevelstats_by_class': class_by_class,
    }


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(REPO / 'build/catalog.json'))
    ap.add_argument('--dump', type=Path, default=DEFAULT_DUMP)
    ap.add_argument('--dbc-dir', type=Path, default=DBC_DIR)
    ap.add_argument('--defs', type=Path, default=dbc.DEFS)
    ap.add_argument('--progression', type=Path, default=REPO / 'data/progression.json')
    ap.add_argument('--core-spells', type=Path, default=CORE_SPELLS)
    ap.add_argument('--equipment', type=Path, default=EQUIPMENT)
    ap.add_argument('--character', help='explicitly refresh character snapshot from the live realm')
    ap.add_argument('--talent-only', action='store_true', help='write code-linked talent spells missing from a progression catalog')
    ap.add_argument('--keep-character', action='store_true', help='keep build/character.json (the realm lacks the player)')
    args = ap.parse_args()
    dbc.DEFS = args.defs
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    cat = build(args.dump, args.dbc_dir, args.progression, args.core_spells, args.equipment)
    if args.character and not args.keep_character:
        who = character(args.character)
        missing = {e['entry'] for e in who['equipment']} - set(cat['items'])
        if missing:
            raise SystemExit(f'{args.character} wears items the catalog leaves out: {sorted(missing)}')
        (out.parent / 'character.json').write_text(json.dumps(who, indent=1))
    if args.talent_only:
        # Progression catalogs already contain every talent rank and DBC trigger.
        # The runtime supplement also carries racials for older generated catalogs.
        wanted = set(TALENT_LINKED_SPELLS) | set(RACIAL_SPELLS.values()) | set(RACIAL_LINKED_SPELLS)
        while True:
            added = {cat['spells'][sid][f'EffectTriggerSpell{e}'] for sid in wanted for e in (1, 2, 3)
                     if cat['spells'][sid][f'EffectTriggerSpell{e}']}
            if added <= wanted:
                break
            wanted |= added
        cat = {**cat, 'spells': {sid: row for sid, row in cat['spells'].items() if sid in wanted}}
        for name in ('spell_affect', 'spell_proc_event', 'spell_facing'):
            cat[name] = [r for r in cat[name] if r['entry'] in wanted]
        for name, field in (('cast_times', 'CastingTimeIndex'), ('durations', 'DurationIndex'),
                            ('ranges', 'RangeIndex'), ('icons', 'SpellIconID')):
            used = {s[field] for s in cat['spells'].values() if s[field]}
            cat[name] = {key: value for key, value in cat[name].items() if key in used}
        used = {s[f'EffectRadiusIndex{e}'] for s in cat['spells'].values() for e in (1, 2, 3)}
        cat['radii'] = {key: value for key, value in cat['radii'].items() if key in used}
        cat = {k: cat[k] for k in ('provenance', 'spells', 'spell_affect', 'spell_proc_event', 'spell_facing',
                                  'cast_times', 'durations', 'ranges', 'radii', 'icons')}
    out.write_text(json.dumps(cat, indent=1, sort_keys=True, default=str))
    print(f'{out}: {len(cat["spells"])} spells')
