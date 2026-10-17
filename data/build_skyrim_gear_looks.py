#!/usr/bin/env python3
"""Offline, deterministic Skyrim gear -> WoW 1.12 ItemDisplayInfo approximation.

No runtime forms or imported Skyrim meshes: owner-local forms survive load-order
changes. Tune FAMILIES, PALETTES, SCORE, or entry overrides, then rebuild and review
tools/skyrim_gear_contact_sheets.py. Names/colors are metadata heuristics, not a
claim that these unrelated games have identical geometry.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from data import dbc, esm, sql_dump
from data.skyrim_magic import DEFAULT_DATA, Strings, identity, integer, plugin_header

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SQL = Path(__file__).resolve().parents[2] / 'WoW/cmangos/classic-db/Full_DB/ClassicDB_1_12_1_z2815.sql.gz'
BASE_PLUGINS = ('Skyrim.esm', 'Update.esm', 'Dawnguard.esm', 'HearthFires.esm', 'Dragonborn.esm')

# First match wins. Selectors inspect worn model paths, then resolved material
# keywords, then editor/display names. (id, selectors, light-armor subclass, palette,
# target item level, WoW style hints). Heavy stays plate; clothing stays cloth.
# Hints deliberately reference client texture/model families and ClassicDB names.
FAMILIES = (
    ('golden_saint', ('goldensaint', 'staada'), 3, 'gold', 60, ('glorious', 'gold', 'ornate')),
    ('dark_seducer', ('darkseducer',), 2, 'purple', 60, ('shadow', 'purple', 'dark', 'ebon')),
    ('snow_elf', ('ivory', 'snowelf',), 3, 'silver', 50, ('ivory', 'white', 'silver')),
    ('bone_crown', ('bonecrown',), 3, 'tan', 55, ('bone', 'ivory', 'skeletal')),
    ('dragon_priest', ('dragonpriest',), 1, 'bronze', 55, ('gold', 'wizard', 'bone')),
    ('madness', ('madness',), 3, 'bronze', 65, ('dark iron', 'dreadnaught', 'obsidian')),
    ('amber', ('amber',), 3, 'gold', 60, ('glorious', 'ornate', 'elegant')),
    ('stahlrim', ('stalhrim', 'stahlrim'), 3, 'blue', 65, ('frost', 'ice', 'runic', 'blue')),
    ('nordic', ('nordic', 'nordhero'), 3, 'silver', 55, ('imperial plate', 'ornate', 'heavy')),
    ('chitin', ('chitin',), 2, 'red', 45, ('blood', 'red', 'tribal', 'bone')),
    ('bonemold', ('bonemold',), 3, 'tan', 40, ('bone', 'ivory', 'sand', 'khaki')),
    ('dragonplate', ('dragonplate', 'armordragonbone'), 3, 'tan', 65, ('bone', 'ivory', 'valorous')),
    ('dragonscale', ('dragonscale',), 3, 'brown', 65, ('dragon', 'scale', 'black dragonscale')),
    ('dragonbone', ('dragonbone',), 3, 'tan', 65, ('bone', 'ivory', 'skeletal')),
    ('daedric', ('daedric', 'dremora'), 3, 'black', 65, ('deathbone', 'doom', 'dread', 'black', 'demon')),
    ('ebony', ('ebony', 'boethiah'), 3, 'black', 60, ('ebon', 'black', 'deathbone', 'obsidian')),
    ('glass', ('glass',), 3, 'green', 55, ('jade', 'emerald', 'green', 'warstrike')),
    ('elven', ('elven', 'auriel'), 3, 'gold', 45, ('glorious', 'ornate', 'gold', 'elven')),
    ('dwarven', ('dwarven', 'dwarv', 'dwemer', 'keening'), 3, 'bronze', 40, ('bronze', 'gold', 'dwarven', 'ornate')),
    ('orcish', ('orcish',), 3, 'green', 40, ('green', 'jade', 'orc', 'brutish')),
    ('falmer', ('falmer',), 2, 'brown', 40, ('bone', 'chitin', 'brown', 'tribal')),
    ('draugr', ('draugr', 'ancientnord',), 3, 'brown', 25, ('rust', 'worn', 'bone', 'brown')),
    ('silver', ('silver',), 3, 'silver', 30, ('silver', 'silvered', 'white')),
    ('steel_plate', ('steelplate',), 3, 'silver', 50, ('imperial plate', 'silver', 'ornate')),
    ('steel', ('steel', 'blades'), 3, 'silver', 30, ('steel', 'silver', 'polished', 'brigade')),
    ('iron', ('iron',), 3, 'grey', 15, ('iron', 'gray', 'grey', 'worn')),
    ('scaled', ('scaled', 'scale'), 3, 'brown', 30, ('scale', 'brown', 'rough')),
    ('chain', ('chain',), 3, 'grey', 25, ('chain', 'gray', 'grey')),
    ('imperial', ('imperial', 'penitus'), 2, 'red', 30, ('scarlet', 'red', 'brigade', 'imperial')),
    ('stormcloak', ('stormcloak',), 2, 'blue', 25, ('blue', 'battleforge', 'banded')),
    ('guard', ('guard',), 3, 'yellow', 25, ('yellow', 'gold', 'banded')),
    ('forsworn', ('forsworn',), 2, 'brown', 20, ('tribal', 'barbaric', 'brown', 'bone')),
    ('vampire', ('vampire',), 2, 'red', 50, ('blood', 'red', 'night', 'shadow')),
    ('dawnguard', ('dawnguard',), 3, 'brown', 45, ('brown', 'brigade', 'banded', 'iron')),
    ('nightingale', ('nightingale',), 2, 'black', 60, ('nightscape', 'dark', 'black', 'shadow')),
    ('dark_brotherhood', ('darkbrotherhood', 'shrouded', 'dbarmor'), 2, 'black', 45, ('shadow', 'black', 'blood')),
    ('thieves_guild', ('thievesguild', 'guildmaster',), 2, 'brown', 40, ('dark leather', 'brown', 'nightscape')),
    ('wolf', ('wolf',), 3, 'grey', 40, ('gray', 'grey', 'silver', 'wolf')),
    ('fur', ('fur',), 2, 'brown', 10, ('fur', 'brown', 'rugged', 'tribal')),
    ('hide', ('hide', 'studded'), 2, 'brown', 15, ('hide', 'brown', 'rough', 'studded')),
    ('leather', ('leather',), 2, 'brown', 20, ('leather', 'brown', 'dark leather')),
    ('robes', ('robe', 'archmage', 'monk', 'priest'), 1, 'brown', 35, ('robe', 'brown', 'wizard')),
    ('fine_clothes', ('fineclothes', 'jarl', 'yarl', 'wedding', 'emperor'), 1, 'blue', 35, ('royal', 'blue', 'elegant')),
    ('clothes', ('clothes', 'clothing', 'prisoner'), 1, 'brown', 5, ('brown', 'simple', 'linen')),
    ('wood', ('wood', 'hunting', 'longbow'), 2, 'brown', 10, ('wood', 'brown', 'short', 'simple')),
    ('bound', ('bound', 'summoned'), 3, 'blue', 60, ('blue', 'arcane', 'glow', 'spectral')),
    ('generic', (), 2, 'grey', 25, ('gray', 'grey', 'simple')),
)
PALETTES = {
    'black': ('black', 'dark', 'ebon', 'obsidian'),
    'grey': ('grey', 'gray', 'silver', 'steel'),
    'silver': ('silver', 'white', 'steel'),
    'bronze': ('bronze', 'copper', 'gold', 'orange'),
    'gold': ('gold', 'yellow', 'glorious'),
    'yellow': ('yellow', 'gold'),
    'tan': ('tan', 'ivory', 'bone', 'khaki', 'sand', 'beige'),
    'brown': ('brown', 'rust'),
    'green': ('green', 'jade', 'emerald', 'teal'),
    'blue': ('blue', 'azure', 'frost', 'ice', 'teal'),
    'red': ('red', 'crimson', 'scarlet', 'blood'),
    'purple': ('purple', 'violet', 'amethyst'),
}
SCORE = {'style': 70, 'asset_color': 120, 'name_color': 25,
         'wrong_asset_color': 90, 'head_shape': 50, 'wrong_head_shape': 140,
         'level_distance': 1, 'quality_distance': 8}
# Reviewed per-form exceptions, using *item entries*, so type/display validation
# is never bypassed. E.g. 'Skyrim.esm:012345': 1234. Empty until visually reviewed.
ENTRY_OVERRIDES = {}
# Visually inspected family/profile exceptions: (family, kind, class, subclass).
# The Dwarven head should be a closed brass mask, not an open mail headdress.
# See build/skyrim_gear_looks/dwarven-candidates.png and the documented review.
FAMILY_ENTRY_OVERRIDES = {('dwarven', 'head', 4, 4): 16731}  # Helm of Valor, display 31284

WEAPON_KINDS = {
    0: ('unarmed', 13, (13, 21, 22)), 1: ('sword', 7, (13, 21)),
    2: ('dagger', 15, (13, 21)), 3: ('axe', 0, (13, 21)),
    4: ('mace', 4, (13, 21)), 5: ('greatsword', 8, (17,)),
    6: ('battleaxe', 1, (17,)), 7: ('bow', 2, (15, 26)),
    8: ('staff', 10, (17,)), 9: ('crossbow', 18, (26,)),
}
# Priority makes cuirasses covering hands/feet one chest, and hooded robes one
# robe. Secondary Skyrim slots are recorded; WoW cannot reproduce all of them.
ARMOR_SLOTS = (
    (39, 'shield', (14,)), (32, 'chest', (5,)), (33, 'hands', (10,)),
    (37, 'feet', (8,)), (35, 'neck', (2,)), (36, 'ring', (11,)),
    (47, 'back', (16,)), (42, 'head', (1,)), (30, 'head', (1,)),
    (31, 'head', (1,)), (41, 'head', (1,)), (34, 'wrists', (9,)),
    (38, 'feet', (8,)), (49, 'legs', (7,)), (53, 'legs', (7,)),
    (54, 'legs', (7,)), (57, 'shoulders', (3,)), (58, 'wrists', (9,)),
    (59, 'wrists', (9,)), (46, 'chest', (5,)), (56, 'chest', (5,)),
)


def compact(text):
    return re.sub(r'[^a-z0-9]', '', text.casefold())


def text(raw):
    return raw.rstrip(b'\0').decode('utf-8', 'replace') if raw else ''


def biped_mask(record):
    """An ARMO's or ARMA's biped slots (bit n: slot 30 + n): BOD2's, or legacy BODT's (Skyrim.esm's addons have BODT)."""
    return integer(record.get('BOD2') or record.get('BODT'))


def inventory(data, language='english', plugins=None):
    """Apply masters/overrides, then resolve keywords, addons and templates."""
    data = Path(data)
    available = {p.name.casefold(): p for p in data.iterdir()
                 if p.suffix.casefold() in ('.esm', '.esl', '.esp')}
    requested = plugins if plugins is not None else list(BASE_PLUGINS) + sorted(
        p.name for p in available.values() if p.name.casefold().startswith('cc'))
    strings, records, order, headers = Strings(data, language), {}, [], {}

    def load(name):
        path = available.get(name.casefold())
        if path is None:
            raise FileNotFoundError(f'missing plugin {name} in {data}')
        if path.name in headers:
            return
        flags, masters = plugin_header(path)
        # Mark before recursion to detect invalid master cycles explicitly.
        headers[path.name] = None
        for master in masters:
            master_path = available.get(master.casefold())
            if master_path and master_path.name in headers and headers[master_path.name] is None:
                raise ValueError(f'cyclic masters: {path.name} -> {master}')
            load(master)
        headers[path.name] = (flags, masters)
        order.append(path.name)
        files = masters + [path.name]

        def resolve(form):
            if not form:
                return None
            index = form >> 24
            if index >= len(files):
                raise ValueError(f'invalid file-relative form {form:08X} in {path.name}')
            return identity(available[files[index].casefold()].name, form & 0xffffff)

        for group in esm.read(path, {'ARMO', 'WEAP', 'KYWD', 'ARMA'}).values():
            for record in group:
                key = resolve(record.form)
                if record.flags & 0x1000:  # ignored overrides never replace a record
                    continue
                if record.flags & 0x20:
                    records.pop(key, None)
                    continue
                kw = record.get('KWDA') or b''
                if len(kw) % 4:
                    raise ValueError(f'truncated KWDA: {key}')
                bod = record.get('BOD2') or record.get('BODT')
                # Legacy BODT can omit its final armor-type word (8 bytes).
                if bod and len(bod) < 8:
                    raise ValueError(f'truncated biped data: {key}')
                model_tags = ('MOD2', 'MOD3', 'MOD4', 'MOD5') if record.type in ('ARMA', 'ARMO') else ('MODL',)
                raw_dnam = record.get('DNAM')
                if record.type == 'WEAP' and not raw_dnam:
                    raise ValueError(f'missing WEAP DNAM animation type: {key}')
                records[key] = {
                    'key': key, 'plugin': key.rsplit(':', 1)[0],
                    'local_form_id': int(key.rsplit(':', 1)[1], 16),
                    'source_plugin': path.name, 'record_type': record.type,
                    'edid': record.edid,
                    'name': strings.text(record, 'FULL', path.name, flags & 0x80)
                            if record.type in ('ARMO', 'WEAP') else '',
                    'keywords': [resolve(n) for n in struct.unpack('<' + 'I' * (len(kw) // 4), kw)],
                    'models': [text(raw) for tag in model_tags for raw in record.all(tag)],
                    'addons': [resolve(integer(raw)) for raw in record.all('MODL')]
                              if record.type == 'ARMO' else [],
                    # WEAP TNAM is attack-fail sound; CNAM is its template.
                    'template': resolve(integer(record.get('CNAM' if record.type == 'WEAP' else 'TNAM')))
                                if record.type in ('WEAP', 'ARMO') else None,
                    'biped_mask': biped_mask(record),
                    'armor_type': integer(bod, 4 if record.get('BOD2') else 8, default=2),
                    'animation_type': raw_dnam[0] if record.type == 'WEAP' else None,
                }

    for name in requested:
        load(name)

    def inherited(row, trail=()):
        if row['key'] in trail:
            raise ValueError(f"cyclic gear template: {row['key']}")
        parent = records.get(row['template'])
        if row['template'] and not parent:
            raise ValueError(f"unresolved gear template {row['template']} in {row['key']}")
        models, keywords = list(row['models']), list(row['keywords'])
        if parent:
            pm, pk = inherited(parent, trail + (row['key'],))
            # A variant's explicit visual data wins; inherited keywords supplement it.
            models = models or pm
            keywords += pk
        worn_models = []
        for addon in row['addons']:
            if addon not in records:
                raise ValueError(f"unresolved armor addon {addon} in {row['key']}")
            worn_models += records[addon]['models']
        # ARMO's ground mesh can be an unrelated placeholder (e.g. Jarl gloves
        # use a Nightingale ground mesh). ARMA is the worn geometry authority.
        models = worn_models or models
        return sorted(set(models)), sorted(set(k for k in keywords if k))

    rows, excluded = [], []
    for row in sorted(records.values(), key=lambda r: (r['plugin'].casefold(), r['local_form_id'])):
        if row['record_type'] not in ('ARMO', 'WEAP'):
            continue
        if row['record_type'] == 'ARMO' and not row['biped_mask']:
            excluded.append(row['key'])
            continue
        row = dict(row)  # Leave referenced templates' raw form references intact.
        models, keywords = inherited(row)
        missing = [k for k in keywords if k not in records]
        if missing:
            raise ValueError(f"unresolved keywords in {row['key']}: {missing}")
        row['models'] = models
        row['keywords'] = [records[k]['edid'] for k in keywords]
        row['biped_slots'] = [30 + bit for bit in range(32) if row['biped_mask'] & (1 << bit)]
        rows.append(row)
    return {'plugins': order, 'forms': rows, 'excluded_zero_slot_armors': excluded}


def family_for(row):
    # Worn geometry wins over gameplay material keywords: Draugr armor carries
    # ArmorMaterialDaedric and the Execution Hood carries ArmorMaterialDwarven.
    material = ' '.join(k for k in row['keywords'] if 'material' in k.casefold())
    def match(source):
        for family in FAMILIES[:-1]:
            if any(selector in source for selector in family[1]):
                return family
        return None
    geometry = match(compact(' '.join(row['models'])))
    keyword = match(compact(material))
    # Shared directory names are ambiguous: leather uses Armor/Studded, while
    # scaled/studded pieces use Armor/Hide. Material keywords disambiguate them.
    if geometry and geometry[0] == 'hide' and keyword and keyword[0] in ('leather', 'scaled', 'hide', 'imperial'):
        return keyword
    for family in (geometry, keyword, match(compact(row['edid'] + ' ' + row['name']))):
        if family:
            return family
    return FAMILIES[-1]


def profile_for(row, family):
    warnings = []
    if row['record_type'] == 'WEAP':
        anim = row['animation_type']
        if anim not in WEAPON_KINDS:
            raise ValueError(f"unknown weapon animation type {anim}: {row['key']}")
        kind, subclass, types = WEAPON_KINDS[anim]
        hints = compact(' '.join(row['keywords'] + row['models'] + [row['edid'], row['name']]))
        # Skyrim uses animation type 6 for BOTH battleaxes and warhammers.
        if anim == 6 and ('warhammer' in hints or 'weaptypewarhammer' in hints):
            kind, subclass = 'warhammer', 5
        if anim == 0:
            warnings.append('Unarmed/NPC WEAP approximated by a WoW fist weapon.')
        return {'kind': kind, 'class': 2, 'subclass': subclass, 'types': types}, warnings
    slots = row['biped_slots']
    if 'skin' in row['edid'].casefold() or 'fx' in row['edid'].casefold() or 50 in slots or 51 in slots:
        warnings.append('NPC skin/effect/decapitation ARMO retained; player-wear approximation may be meaningless.')
    slot = next((r for r in ARMOR_SLOTS if r[0] in slots), None)
    if slot is None:
        # Jewelry has an actual DB display/type but no worn model in 1.12.
        slot = (None, 'accessory', (11,))
        warnings.append('No WoW worn slot for these biped bits; invisible ring surrogate.')
    _, kind, types = slot
    if kind in ('ring', 'neck', 'accessory'):
        subclass = 0
        warnings.append('WoW rings/necklaces have no visible player model.')
    elif kind == 'shield':
        subclass = 6
    else:
        armor_type = row['armor_type']
        if armor_type not in (0, 1, 2):
            raise ValueError(f"unknown armor type {armor_type}: {row['key']}")
        subclass = 4 if armor_type == 1 else 1 if armor_type == 2 else family[2]
        if subclass == 1 and kind == 'chest' and ('robe' in compact(' '.join(row['models']) + row['edid'] + row['name'])):
            types = (20,)
        if kind == 'back':
            subclass = 1  # WoW cloaks are cloth whatever Skyrim's armor type.
        if kind == 'head' and 42 in slots and 30 not in slots and row['armor_type'] == 2:
            # Cosmetic circlets may live in WoW's miscellaneous armor class.
            subclass = (0, 1)
    if 32 in slots:
        warnings.append('Skyrim torso also covers legs; one WoW chest/robe may leave legs exposed.')
        if 31 in slots or 30 in slots or 42 in slots:
            warnings.append('Hooded body item needs an additional WoW helm to reproduce its hood.')
    head_style = None
    if kind == 'head':
        shape = compact(' '.join(row['models']) + row['edid'])
        head_style = 'circlet' if 'circlet' in shape else 'hood' if 'hood' in shape else 'helmet'
    return {'kind': kind, 'class': 4, 'subclass': subclass, 'types': types, 'head_style': head_style}, warnings


def palette_for(row, family):
    # Explicit Skyrim texture/model/name color overrides a material's default.
    evidence = compact(' '.join(row['models']) + ' ' + row['edid'] + ' ' + row['name'])
    for color in ('purple', 'blue', 'red', 'green', 'black', 'yellow', 'white', 'brown', 'grey', 'gray'):
        if color in evidence:
            return {'white': 'silver', 'gray': 'grey'}.get(color, color)
    return family[3]


def load_candidates(dbc_path, sql_path):
    displays = dbc.by_id(dbc.read(dbc_path))
    columns = ('entry', 'name', 'class', 'subclass', 'displayid', 'InventoryType', 'Quality', 'ItemLevel', 'sheath', 'Material')
    items = sql_dump.tables(sql_path, {'item_template': columns},
        {'item_template': lambda r: r['class'] in (2, 4) and r['InventoryType'] != 0})['item_template']
    candidates = []
    for item in sorted(items, key=lambda i: i['entry']):
        display = displays.get(item['displayid'])
        if display is None or re.search(r'\[ph\]|deprecated|zzold|\btest\b|^old|^monster\b|^unused', item['name'], re.I):
            continue
        inv = item['InventoryType']
        # Body gear needs texture components; attached gear needs a model.
        if inv in (1, 3, 13, 14, 15, 17, 21, 22, 23, 25, 26) and not display['ModelName'][0]:
            continue
        if inv in (5, 6, 7, 8, 9, 10, 20) and not any(display['Texture']):
            continue
        if inv == 16 and not display['ModelTexture'][0]:
            continue
        assets = display['ModelName'] + display['ModelTexture'] + display['Texture']
        candidates.append(dict(item, assets=[a for a in assets if a],
                               asset_text=' '.join(assets).casefold()))
    return candidates


def compatible(candidate, profile):
    subclasses = profile['subclass'] if isinstance(profile['subclass'], tuple) else (profile['subclass'],)
    return (candidate['class'] == profile['class'] and candidate['subclass'] in subclasses
            and candidate['InventoryType'] in profile['types'])


def candidate_score(candidate, family, palette, profile=None):
    assets, name = candidate['asset_text'], candidate['name'].casefold()
    combined = name + ' ' + assets
    style_hits = [hint for hint in family[5] if hint in combined]
    asset_hits = [hint for hint in PALETTES[palette] if hint in assets]
    name_hits = [hint for hint in PALETTES[palette] if hint in name]
    # Contradictory explicit asset colors matter; vague item names do not.
    wrong_colors = [color for color in ('black', 'blue', 'red', 'green', 'purple', 'yellow', 'brown', 'white')
                    if color in assets and color not in PALETTES[palette]]
    level_distance = abs(candidate['ItemLevel'] - family[4])
    target_quality = 1 if family[4] < 20 else 2 if family[4] < 55 else 3
    score = (SCORE['style'] * bool(style_hits) + SCORE['asset_color'] * bool(asset_hits)
             + SCORE['name_color'] * bool(name_hits) - SCORE['wrong_asset_color'] * len(wrong_colors)
             - SCORE['level_distance'] * level_distance
             - SCORE['quality_distance'] * abs(candidate['Quality'] - target_quality))
    head_style = profile.get('head_style') if profile else None
    circlet = 'circlet' in combined or 'crown' in name or 'tiara' in name
    hood = 'hood' in combined or 'cowl' in name
    if head_style == 'circlet':
        score += SCORE['head_shape'] if circlet else -SCORE['wrong_head_shape']
    elif head_style == 'hood':
        score += SCORE['head_shape'] if hood else -SCORE['wrong_head_shape'] * circlet
    elif head_style == 'helmet':
        score += SCORE['head_shape'] if 'helm_plate' in assets or 'helm_mail' in assets else 0
        score -= SCORE['wrong_head_shape'] * circlet
    evidence = style_hits[:1] + asset_hits[:1] + name_hits[:1]
    return score, sorted(set(evidence))


def build(inv, candidates):
    groups = defaultdict(list)
    for candidate in candidates:
        groups[(candidate['class'], candidate['subclass'], candidate['InventoryType'])].append(candidate)
    by_entry = {c['entry']: c for c in candidates}
    cache, items, selected = {}, {}, {}
    for row in inv['forms']:
        family = family_for(row)
        profile, limitations = profile_for(row, family)
        palette = palette_for(row, family)
        def choose(profile, override=None):
            subclasses = profile['subclass'] if isinstance(profile['subclass'], tuple) else (profile['subclass'],)
            signature = (family[0], palette, profile['class'], subclasses, profile['types'], profile.get('head_style'))
            if override is not None:
                winner = by_entry.get(override)
                if winner is None or not compatible(winner, profile):
                    raise ValueError(f"incompatible/missing WoW entry override {override}: {row['key']}")
                score, evidence = candidate_score(winner, family, palette, profile)
            elif signature in cache:
                winner, score, evidence = cache[signature]
            else:
                pool = [c for sub in subclasses for invtype in profile['types']
                        for c in groups[(profile['class'], sub, invtype)]]
                if not pool:
                    raise ValueError(f"no compatible WoW display for {row['key']}: {profile}")
                # Equal scores pick the lowest display, then entry, independent of input order.
                winner = max(pool, key=lambda c: (candidate_score(c, family, palette, profile)[0], -c['displayid'], -c['entry']))
                score, evidence = candidate_score(winner, family, palette, profile)
                cache[signature] = winner, score, evidence
            return winner, score, evidence

        override = ENTRY_OVERRIDES.get(row['key'])
        if override is None:
            override = FAMILY_ENTRY_OVERRIDES.get((family[0], profile['kind'], profile['class'], profile['subclass']))
        winner, score, evidence = choose(profile, override)
        if not evidence:
            limitations.append('No style/color token match; type and tier approximation only.')
        reason = f"{family[0]} {palette} {profile['kind']}; "
        reason += ('metadata ' + '/'.join(evidence)) if evidence else 'nearest compatible tier'
        if override is not None:
            reason += '; reviewed entry override'
        items[row['key']] = {
            **{k: row[k] for k in ('plugin', 'local_form_id', 'source_plugin', 'record_type', 'edid', 'name')},
            'biped_slots': row['biped_slots'], 'armor_type': row['armor_type'] if row['record_type'] == 'ARMO' else None,
            'animation_type': row['animation_type'], 'kind': profile['kind'],
            'family': family[0], 'palette': palette,
            'display_id': winner['displayid'], 'inventory_type': winner['InventoryType'],
            'class': winner['class'], 'subclass': winner['subclass'], 'sheath': winner['sheath'],
            'wow_entry': winner['entry'], 'reason': reason, 'limitations': limitations,
        }
        if row['record_type'] == 'WEAP':
            items[row['key']]['material'] = winner.get('Material', 0)
        extras = []
        if profile['kind'] == 'chest' and 32 in row['biped_slots']:
            if winner['InventoryType'] != 20:
                extras.append(dict(profile, kind='legs', types=(7,), head_style=None))
            if any(slot in row['biped_slots'] for slot in (30, 31, 42)):
                extras.append(dict(profile, kind='head', types=(1,), head_style='hood'))
            # The runtime expands this body item; these single-display warnings no longer apply.
            limitations[:] = [w for w in limitations if not w.startswith(('Skyrim torso', 'Hooded body'))]
        additional = []
        selections = [winner]
        for extra in extras:
            selected_item, _, _ = choose(extra)
            additional.append({
                'display_id': selected_item['displayid'], 'inventory_type': selected_item['InventoryType'],
                'class': selected_item['class'], 'subclass': selected_item['subclass'],
                'sheath': selected_item['sheath'], 'wow_entry': selected_item['entry'],
            })
            selections.append(selected_item)
        if additional:
            items[row['key']]['additional'] = additional
        for chosen in selections:
            selected[str(chosen['entry'])] = {
                'name': chosen['name'], 'display_id': chosen['displayid'],
                'inventory_type': chosen['InventoryType'], 'class': chosen['class'],
                'subclass': chosen['subclass'], 'quality': chosen['Quality'],
                'item_level': chosen['ItemLevel'], 'assets': chosen['assets'],
            }
    counts = {
        'total': len(items), 'record_types': dict(sorted(Counter(r['record_type'] for r in items.values()).items())),
        'plugins': dict(sorted(Counter(r['plugin'] for r in items.values()).items())),
        'families': dict(sorted(Counter(r['family'] for r in items.values()).items())),
        'kinds': dict(sorted(Counter(r['kind'] for r in items.values()).items())),
        'unique_displays': len({i['display_id'] for r in items.values() for i in [r] + r.get('additional', [])}),
        'no_style_color_evidence': sum('nearest compatible tier' in r['reason'] for r in items.values()),
    }
    return {'schema_version': 1, 'client_build': '1.12.1.5875', 'plugins': inv['plugins'],
            'scope': 'Surviving ARMO with nonzero biped masks and all WEAP, including enchanted, NPC, quest, '
                     'test and unarmed records; masters/overrides resolved in listed plugin order.',
            'excluded_zero_slot_armors': inv['excluded_zero_slot_armors'], 'counts': counts,
            'items': items, 'wow_items': dict(sorted(selected.items(), key=lambda p: int(p[0])))}


def file_hash(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def serialize(result):
    return json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=DEFAULT_DATA)
    parser.add_argument('--language', default='english')
    parser.add_argument('--plugin', action='append', help='Repeat for explicit plugin scope; masters included automatically.')
    parser.add_argument('--dbc', type=Path, default=ROOT / 'build/dbc/DBFilesClient/ItemDisplayInfo.dbc')
    parser.add_argument('--sql', type=Path, default=DEFAULT_SQL)
    parser.add_argument('--out', type=Path, default=ROOT / 'data/skyrim_gear_looks.json')
    parser.add_argument('--check', action='store_true', help='Compare current inputs/rules to --out without writing.')
    args = parser.parse_args()
    inv = inventory(args.data, args.language, args.plugin)
    result = build(inv, load_candidates(args.dbc, args.sql))
    result['language'] = args.language
    result['input_sha256'] = {p: file_hash(args.data / p) for p in inv['plugins']}
    result['input_sha256'].update({'ItemDisplayInfo.dbc': file_hash(args.dbc), args.sql.name: file_hash(args.sql)})
    encoded = serialize(result)
    if args.check:
        if not args.out.exists() or args.out.read_text() != encoded:
            raise SystemExit(f'{args.out}: differs from current inputs/rules')
        print(f'{args.out}: reproducible')
    else:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(encoded)
    print(json.dumps(result['counts'], sort_keys=True))


if __name__ == '__main__':
    main()
