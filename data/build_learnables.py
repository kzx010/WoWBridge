#!/usr/bin/env python3
"""Export the items that teach: a book for every druid class rank and the reins of every mount in data/mounts.json,
and, under class_books, a book for every Paladin rank in data/paladin_manifest.json (not shipped yet: data/build_esp.py
reads only 'books').

A rank's book is the 1.12 item that teaches it (the manifest's item provenance: Spell.dbc's learning spell names the
rank), else the 1.12 item of its name and rank at its level (ClassicDB item_template: "Book of Moonfire II", "Book:
Gift of the Wild", "Libram: Holy Light II"), or, where 1.12 has none, a made-up one in the class's style: "Book of
<Spell> <Rank>" or "Libram: <Spell> <Rank>", the class books' display, quality, class mask and trainer-cost price.
Ranks a class starts with or takes from talents have no book. A mount's item is its 1.12 reins, whistle or horn; the
two TBC flyers keep TBC's item for their spell, named as their mount.
The rows stay 1.12's own; data/build_esp.py makes the Skyrim adaptations (level, requirements).
"""
import json
import re
from pathlib import Path

from data import dbc
from data.sql_dump import tables
from data.build_catalog import DBC_DIR, DEFAULT_DUMP, REPO, sha256

PROGRESSION, MOUNTS, MANIFEST = REPO / 'data/progression.json', REPO / 'data/mounts.json', REPO / 'data/paladin_manifest.json'
FIELDS = ('entry', 'name', 'class', 'subclass', 'displayid', 'Quality', 'Flags', 'bonding', 'AllowableClass', 'AllowableRace',
          'ItemLevel', 'RequiredLevel', 'RequiredSkill', 'RequiredSkillRank', 'requiredspell', 'maxcount', 'stackable',
          'BuyPrice', 'SellPrice', 'spellid_1', 'spelltrigger_1', 'description')
DRUID = 1 << 10   # AllowableClass bit of class 11
PALADIN = 1 << 1  # AllowableClass bit of class 2
BOOK = re.compile(r'^Book(?: of|:) (.+?)(?: ([IVX]+))?$')
LIBRAM = re.compile(r'^Libram: (.+?)(?: ([IVX]+))?$')
# Each class's 1.12 books (class 9, subclass 0): the bit they allow, their names, and a made-up book's name prefix,
# display and class mask (its own books', which every Quality 1 one shares; data/tests/test_learnables.py checks).
STYLES = {'druid': {'mask': DRUID, 'pattern': BOOK, 'prefix': 'Book of ', 'displayid': 1317, 'AllowableClass': 32256},
          'paladin': {'mask': PALADIN, 'pattern': LIBRAM, 'prefix': 'Libram: ', 'displayid': 1155, 'AllowableClass': 31234}}
# A 1.12 book of the rank's name is its book only within a trainer step of the rank's level (the druid's Remove Curse is
# 22 for a level-24 rank); further off it is a pre-release book of another spell under that name (Libram: Seal of Wisdom
# at 12 for a level-38 seal, Libram: Judgement at 40 for a level-4 one).
LEVEL_STEP = 2
ROMAN = ('I', 'II', 'III', 'IV', 'V', 'VI', 'VII', 'VIII', 'IX', 'X', 'XI', 'XII')
SYNTHETIC = 900000   # a made-up book's entry: this plus its rank's spell (no WoW item has such an entry)
# TBC's item for each flyer's spell (Mounts.cpp's synthetic 32235/32295): 25531 is Swift Green Wind Rider; 25470 taught
# 32235 as "Golden Gryphon", here named as the mount it teaches, as every TBC flyer's item is.
FLYER_ITEMS = {32235: 25470, 32295: 25531}


def number(row):
    """A rank's number as its book names it: "Rank 2" is II; an unranked spell's book has none (Dire Bear Form)."""
    return int(row['rank_name'].removeprefix('Rank ')) if row['rank_name'].startswith('Rank ') else 1


def book_name(row, prefix='Book of '):
    return f"{prefix}{row['name']}" + (f" {ROMAN[number(row) - 1]}" if number(row) > 1 else '')


def teaches(row):
    """The 1.12 learning spells' own wording: "Teaches Moonfire (Rank 1).", "Teaches Remove Curse." (Spell.dbc)."""
    ranked = row['rank_name'].startswith('Rank ')
    return f"Teaches {row['name']}" + (f" ({row['rank_name']})." if ranked else '.')


def books(rows, items, style):
    """One book for each rank a class learns from a trainer, a quest or an item (rows: progression or manifest ranks)."""
    candidates, prices = {}, {}
    for item in sorted(items.values(), key=lambda i: i['entry']):
        if item['class'] != 9 or item['subclass'] != 0 or not (item['AllowableClass'] == -1 or item['AllowableClass'] & style['mask']):
            continue
        # The class's own common books price its trainer cost: not a free one (Libram: Resurrection) nor any class's
        # (level 0's placeholder recipes, test items).
        if item['Quality'] == 1 and item['AllowableClass'] != -1 and item['AllowableClass'] & style['mask'] and item['BuyPrice']:
            prices.setdefault(item['RequiredLevel'], []).append(item['BuyPrice'])
        if m := style['pattern'].match(item['name']):
            candidates.setdefault((m[1], ROMAN.index(m[2]) + 1 if m[2] else 1), []).append(item)

    def price(level):   # the commonest class book price at its level (its trainer cost), else the nearest below (above)
        known = max((n for n in prices if n <= level), default=min(prices))
        return min(set(prices[known]), key=lambda n: (-prices[known].count(n), n))
    out = []
    for row in sorted(rows, key=lambda r: r['spell']):
        if row['source'] in ('talent', 'starting'):
            continue
        # The item whose learning spell teaches the rank (data/build_paladin.py), else its name and rank at its level.
        found = [items[i['item']] for i in row.get('provenance', {}).get('items', [])] or [
            i for i in candidates.get((row['name'], number(row)), [])
            if abs(i['RequiredLevel'] - row['required_level']) <= LEVEL_STEP]
        # Several of a rank's name: the one at its level (1.12 also has "Book of Wrath" at Wrath II's level 6).
        found = sorted(found, key=lambda i: (i['RequiredLevel'] != row['required_level'], i['entry']))
        if found:
            item = found[0]
        else:
            cost = price(row['required_level'])
            item = {'entry': SYNTHETIC + row['spell'], 'name': book_name(row, style['prefix']), 'class': 9, 'subclass': 0,
                    'displayid': style['displayid'], 'Quality': 1, 'Flags': 80, 'bonding': 0,
                    'AllowableClass': style['AllowableClass'], 'AllowableRace': -1, 'ItemLevel': row['required_level'],
                    'RequiredLevel': row['required_level'], 'RequiredSkill': 0, 'RequiredSkillRank': 0, 'requiredspell': 0,
                    'maxcount': 0, 'stackable': 1, 'BuyPrice': cost, 'SellPrice': cost // 4, 'spellid_1': 0,
                    'spelltrigger_1': 0, 'description': ''}
        out.append({**{k: row[k] for k in ('spell', 'chain', 'rank', 'rank_name', 'name', 'previous_spell', 'skill',
                                           'required_level', 'source')},
                    'real': bool(found), 'text': teaches(row), 'item': item})
    return out


def build():
    druid = json.loads(PROGRESSION.read_text())['classes']['druid']
    paladin = json.loads(MANIFEST.read_text())['ranks']
    mounts = json.loads(MOUNTS.read_text())['mounts']
    items = {r['entry']: {k: r[k] for k in FIELDS} for r in tables(DEFAULT_DUMP, {'item_template': FIELDS})['item_template']}
    druid_books = books(druid['spells'] + druid['excluded'], items, STYLES['druid'])
    icons = {r['InventoryIcon']: r['ID'] for r in sorted(dbc.read(DBC_DIR / 'ItemDisplayInfo.dbc'), key=lambda r: -r['ID'])}
    spells = dbc.by_id(dbc.read(DBC_DIR / 'Spell.dbc'))
    reins = []
    for m in (m for m in mounts if 'class' not in m):   # a class mount is a spell she learns (her librams), no reins
        found = [i for i in items.values() if i['spellid_1'] == m['spell'] and i['spelltrigger_1'] == 0 and i['class'] == 15]
        icon = m['icon'].removeprefix('Interface\\Icons\\')
        if found:
            item = min(found, key=lambda i: i['entry'])
        else:   # a TBC flyer, as TBC's swift flyers are (epic, bound, level 70 with Riding 300); 1.12's icon if it has one
            item = {'entry': FLYER_ITEMS[m['spell']], 'name': m['name'], 'class': 15, 'subclass': 0,
                    'displayid': icons.get(icon, 0), 'Quality': 4, 'Flags': 64, 'bonding': 1, 'AllowableClass': -1,
                    'AllowableRace': -1, 'ItemLevel': 70, 'RequiredLevel': 70, 'RequiredSkill': 762, 'RequiredSkillRank': 300,
                    'requiredspell': 0, 'maxcount': 0, 'stackable': 1, 'BuyPrice': 2000000, 'SellPrice': 0,
                    'spellid_1': m['spell'], 'spelltrigger_1': 0, 'description': ''}
        reins.append({'key': m['key'], 'spell': m['spell'], 'flying': m.get('flying', False), 'real': bool(found),
                      # its Use: line, the mount spell's description (a flyer's from data/mounts.json, as the bags show it)
                      'text': spells[m['spell']]['Description_lang'] if m['spell'] in spells else m['description'],
                      # no 1.12 item display shows this icon: the bags take it as it is
                      'icon': None if item['displayid'] else m['icon'], 'item': item})
    return {'version': 1, 'source': {'classicdb_sha256': sha256(DEFAULT_DUMP), 'progression_sha256': sha256(PROGRESSION),
                                     'mounts_sha256': sha256(MOUNTS), 'paladin_manifest_sha256': sha256(MANIFEST)},
            'books': druid_books, 'mounts': reins, 'class_books': {'paladin': books(paladin, items, STYLES['paladin'])}}


if __name__ == '__main__':
    out = REPO / 'data/learnables.json'
    data = build()
    out.write_text(json.dumps(data, indent=1) + '\n')
    print(f"{out}: {len(data['books'])} books ({sum(b['real'] for b in data['books'])} 1.12), {len(data['mounts'])} mount items; "
          + ', '.join(f"{key} {len(rows)} books ({sum(b['real'] for b in rows)} 1.12)" for key, rows in data['class_books'].items()))
