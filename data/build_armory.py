#!/usr/bin/env python3
"""WoWBridgeArmory.esp: every other 1.12 item a player could own, as Skyrim gear the player finds and buys.

The 95 items of WoWBridge.esp stay there (no saved ID changes); this regular plugin, loaded after it (its master),
holds the rest, built as data/build_esp.py builds those: the Skyrim armour or damage of the item's level and quality,
native bonuses within vanilla enchantments (abilities shared by every item with the same ones), tempering by its tier,
and a Skyrim look by its class, slot and tier where no reviewed look of its WoW display exists. armory.json gives the
plugin (skse/src/Armory.cpp) their WoW identity and where its leveled lists join Skyrim's; armory_catalog.json gives
the service their item_template rows and spells. Object IDs are permanent save-game identities (data/armory_forms.json).
"""
import argparse
import collections
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from data import build_esp as B, dbc, esm   # noqa: E402
from data.build_catalog import CORE_SPELLS, DBC_DIR, DEFAULT_DUMP, COEFFICIENTS, closure, spell_tables   # noqa: E402
from data.sql_dump import tables   # noqa: E402

REPO = B.REPO
PLUGIN, FILE = 'WoWBridgeArmory.esp', 2   # Skyrim.esm is master 0, WoWBridge.esp 1
LOOT = ('entry', 'item', 'mincountOrRef')   # what a loot table row says of its item, or of the table it refers to
# What a player could own: what drops (creatures, objects, containers, fishing, pockets, mail; loot references
# followed), what a vendor sells, what a quest rewards, what a learnable recipe makes, and what a new character wears
# (CharStartOutfit.dbc: the realm's playercreateinfo_item is empty).
DROPS = ('gameobject_loot_template', 'item_loot_template', 'fishing_loot_template', 'mail_loot_template')
# Rows the data marks as never meant for players: GM and test items, placeholders and deprecated or unused copies.
UNOBTAINABLE = re.compile(r'^Monster -|^OLD|\bTEST\b|\(Test\)|^Test |Deprecated|DEPRECATED|\bDEP$|^Unused|\[PH\]|\[UNUSED\]|'
                          r'QATest|JEFFTEST', re.I)
ITEM_FLAG_DEPRECATED = 0x10
TOOLS = (14, 20)   # miscellaneous weapons (mining picks, blacksmith hammers, skinning knives) and fishing poles
# item_template columns whose default is -1, not 0 (rows keep only what differs from a column's default)
MINUS_ONE = {'AllowableClass', 'AllowableRace', *(f'spell{k}cooldown_{n}' for k in ('', 'category') for n in range(1, 6))}
KEEP = {'entry', 'name', 'class', 'subclass', 'InventoryType', 'displayid', 'Quality'}
SLOT_NAMES = {1: 'head', 2: 'neck', 3: 'shoulder', 4: 'shirt', 5: 'chest', 6: 'waist', 7: 'legs', 8: 'feet', 9: 'wrist',
              10: 'hands', 11: 'finger', 12: 'trinket', 13: 'one-hand', 14: 'shield', 15: 'ranged', 16: 'back',
              17: 'two-hand', 19: 'tabard', 20: 'robe', 21: 'main hand', 22: 'off hand', 23: 'held in off-hand',
              25: 'thrown', 26: 'ranged right', 28: 'relic'}
CLASS_NAMES = {(2, 0): 'axe', (2, 1): 'two-handed axe', (2, 2): 'bow', (2, 3): 'gun', (2, 4): 'mace', (2, 5): 'two-handed mace',
               (2, 6): 'polearm', (2, 7): 'sword', (2, 8): 'two-handed sword', (2, 10): 'staff', (2, 13): 'fist weapon',
               (2, 15): 'dagger', (2, 16): 'thrown', (2, 18): 'crossbow', (2, 19): 'wand', (4, 0): 'miscellaneous',
               (4, 1): 'cloth', (4, 2): 'leather', (4, 3): 'mail', (4, 4): 'plate', (4, 6): 'shield', (4, 7): 'libram',
               (4, 8): 'idol', (4, 9): 'totem'}

# Tempering and perks by the tier an item is measured against (vanilla's own recipes: Daedric with ebony, the dragon
# armours with bone and scales): (material keyword, ingredient, perk).
TIER_MATERIALS = {'Iron': ('Iron', 'IngotIron', None), 'Steel': ('Steel', 'IngotSteel', 'SteelSmithing'),
                  'Dwarven': ('Dwarven', 'IngotDwarven', 'DwarvenSmithing'),
                  'SteelPlate': ('SteelPlate', 'IngotCorundum', 'AdvancedArmors'),
                  'Orcish': ('Orcish', 'IngotOrichalcum', 'OrcishSmithing'), 'Ebony': ('Ebony', 'IngotEbony', 'EbonySmithing'),
                  'Dragonplate': ('Dragonplate', 'DragonBone', 'DragonArmor'), 'Daedric': ('Daedric', 'IngotEbony', 'DaedricSmithing'),
                  'Hide': ('Hide', 'Leather01', None), 'Leather': ('Leather', 'Leather01', None),
                  'Elven': ('Elven', 'IngotIMoonstone', 'ElvenSmithing'), 'Scaled': ('Scaled', 'IngotCorundum', 'AdvancedArmors'),
                  'ElvenGilded': ('ElvenGilded', 'IngotIMoonstone', 'ElvenSmithing'), 'Glass': ('Glass', 'IngotMalachite', 'GlassSmithing'),
                  'Dragonscale': ('Dragonscale', 'DragonScales', 'DragonArmor')}
# Where her items are found and sold (skse/src/Distribution.cpp joins each list at data load, never replacing one):
# vanilla lists (form, count, entries: a list choosing among its entries chooses this one that much more often) and
# vendor factions' merchant chests (form, count). Pools mix qualities by weight in bands of Skyrim level, each band at
# its highest item level, so nothing comes above her level: greens and blues in ordinary chests and on humanoid
# corpses, blues and epics in boss chests and dragons, legendaries rarely there from levels 40 (bosses) and 48
# (dragons); whites to blues at blacksmiths (weapons, mail, plate, shields, leather), Radiant Raiment (cloth, shirts,
# tabards, cloaks) and Solitude's fletcher. No list an NPC's inventory or outfit reaches (data/tests checks), so no NPC
# is outfitted with her gear.
POOLS = {
    'WoWArmoryLoot': {'mix': {2: 3, 3: 1}, 'lists': ((0x10B2C0, 1, 3),     # LItemSpecialLoot10: ordinary chests' magic roll
                                                     (0x1031D0, 1, 3))},   # LItemSpecialLoot100: boss chests'
    'WoWArmoryDeath': {'mix': {2: 3, 3: 1}, 'chance_none': 96, 'lists': tuple((form, 1, 1) for form in (
        0xC3C9B, 0xC3C9E, 0x3AD7F, 0x10FACC, 0x3AD84, 0x10F7F2, 0x8601A, 0x3ADA0, 0x10FAD9))},   # DeathItem: bandits ..
    'WoWArmoryBoss': {'mix': {3: 1, 4: 1}, 'legendary': 40, 'lists': tuple((form, 1, 1) for form in (
        0x348FD, 0x34901, 0x3509B,   # Loot{Bandit,Draugr,Warlock}ChestBossMagicItem25
        0x71623, 0x71646, 0x70C1F, 0x1ED1E, 0x6A3CC, 0x7144B, 0x7103B, 0x70DB4))},   # the others' ChestBossBase
    'WoWArmoryDragon': {'mix': {3: 1, 4: 2}, 'legendary': 48, 'lists': ((0xF77F3, 1, 2), (0xF77F6, 1, 1))},   # LootDragon*25
    'WoWArmoryBlacksmith': {'mix': {1: 2, 2: 2, 3: 1}, 'kind': 'smith', 'vendors': tuple((form, 2) for form in (
        0x10FDE8, 0x7277B, 0x647A1, 0xC6E40, 0xC646A, 0xC6465, 0xC6460, 0xC645B, 0xC6455, 0xA6C03, 0xA3F1A, 0xA31C9,
        0xA31C2, 0x9E46C, 0x9E12C, 0x9DA3E, 0x9CAFF, 0x94390, 0x5A666))},
    'WoWArmoryOutfitter': {'mix': {1: 2, 2: 2, 3: 1}, 'kind': 'cloth', 'vendors': ((0xA6C00, 3),)},   # Radiant Raiment
    'WoWArmoryFletcher': {'mix': {1: 2, 2: 2, 3: 1}, 'kind': 'ranged', 'vendors': ((0xB2037, 3),)},
}
LEGENDARY_CHANCE_NONE = 75
BAND = 16   # items a band holds at least, so a level's pick is never down to a handful


def sources(sql, starting):
    """Each item entry's ways into a player's hands."""
    found = collections.defaultdict(set)
    refs = collections.defaultdict(list)
    for r in sql['reference_loot_template']:
        refs[r['entry']].append(r)

    def items(rows, seen):
        for r in rows:
            if r['mincountOrRef'] < 0:
                ref = -r['mincountOrRef']
                if ref not in seen:
                    seen.add(ref)
                    yield from items(refs.get(ref, ()), seen)
            else:
                yield r['item']

    def loot(table, used, tag):
        grouped = collections.defaultdict(list)
        for r in sql[table]:
            if used is None or r['entry'] in used:
                grouped[r['entry']].append(r)
        for rows in grouped.values():
            for entry in items(rows, set()):
                found[entry].add(tag)
    creatures = sql['creature_template']
    loot('creature_loot_template', {c['LootId'] for c in creatures if c['LootId']}, 'drop')
    loot('pickpocketing_loot_template', {c['PickpocketLootId'] for c in creatures if c['PickpocketLootId']}, 'drop')
    for table in DROPS:
        loot(table, None, 'drop')
    for table in ('npc_vendor', 'npc_vendor_template'):
        for r in sql[table]:
            found[r['item']].add('vendor')
    for q in sql['quest_template']:
        for key in [f'RewChoiceItemId{n}' for n in range(1, 7)] + [f'RewItemId{n}' for n in range(1, 5)]:
            if q[key]:
                found[q[key]].add('quest')
    for entry in starting:
        found[entry].add('start')
    # A recipe is learned from a trainer, or from an item that teaches it (its spell triggering the craft) found above.
    spells = {r['Id']: r for r in sql['spell_template']}

    def taught(sid):
        s = spells.get(sid)
        return {s[f'EffectTriggerSpell{e}'] for e in (1, 2, 3) if s and s[f'Effect{e}'] == 36 and s[f'EffectTriggerSpell{e}']}
    learned = {r['spell'] for t in ('npc_trainer', 'npc_trainer_template') for r in sql[t]}
    learned |= {t for sid in list(learned) for t in taught(sid)}
    for r in sql['item_template']:
        if r['class'] == 9 and found.get(r['entry']):
            learned |= {t for n in range(1, 6) for t in taught(r[f'spellid_{n}'])}
    for s in spells.values():
        for e in (1, 2, 3):
            if s['Id'] in learned and s[f'Effect{e}'] == 24 and s[f'EffectItemType{e}']:
                found[s[f'EffectItemType{e}']].add('crafted')
    return found


def select(sql, light, starting=()):
    """The Armory's items (item_template rows by entry) and why every other equippable row stays out."""
    found = sources(sql, starting)
    chosen, excluded = {}, collections.Counter()
    for r in sql['item_template']:
        if r['class'] not in (2, 4) or not r['InventoryType']:
            continue
        why = ('profession tool' if r['class'] == 2 and r['subclass'] in TOOLS else
               'no Skyrim slot' if r['class'] == 4 and r['InventoryType'] not in B.SLOTS or
                                   r['class'] == 2 and r['subclass'] not in B.WEAPONS else
               'already in WoWBridge.esp' if r['entry'] in light else
               'no display' if not r['displayid'] else
               'test or deprecated' if r['Flags'] & ITEM_FLAG_DEPRECATED or UNOBTAINABLE.search(r['name']) or r['Quality'] > 5 else
               'expires' if r['Duration'] else
               'no source' if not found.get(r['entry']) else None)
        if why:
            excluded[why] += 1
        else:
            chosen[r['entry']] = r
    return chosen, dict(sorted(excluded.items()))


def compact(row, defaults):
    """An item_template row without what its columns default to (the plugin's Lua and the service fill those back)."""
    triggers = {f'spelltrigger_{n}' for n in range(1, 6) if row.get(f'spellid_{n}')}
    return {k: v for k, v in sorted(row.items()) if k in KEEP or k in triggers or v != defaults[k]}


def column_defaults(row):
    """What item_template's columns default to: '', 0, or -1 (classes, races, cooldowns)."""
    return {k: '' if isinstance(v, str) else -1 if k in MINUS_ONE else 0 for k, v in row.items()}


def material(item, stats):
    """Its material keyword, temper ingredient and perk: a weapon its tier's alloy (or wood), armour its tier's."""
    if item['class'] == 2:
        return B.crafting_material(item)
    return TIER_MATERIALS[stats['tier']] if stats['tier'] else None


def signature(effects):
    """Equal bonuses, one ability: its effects and magnitudes, rounded past what Skyrim shows."""
    return tuple((f'{e:08X}' if isinstance(e, int) else e, round(float(v), 4)) for e, v in effects)


def slot_group(item):
    """What she can wear at once: one item per Skyrim body slot, a weapon in each hand."""
    return 'hands' if item['class'] == 2 else B.SLOTS.get(int(item['InventoryType']))


def vendor_kind(item):
    """Which vendor sells it: a blacksmith (weapons, mail, plate, shields, leather), an outfitter (cloth, shirts,
    tabards, cloaks) or a fletcher (bows, guns, crossbows, thrown)."""
    if item['class'] == 2:
        return 'ranged' if item['subclass'] in (2, 3, 16, 18) else None if item['subclass'] in (10, 19) else 'smith'
    if item['subclass'] in (2, 3, 4, 6):
        return 'smith'
    return 'cloth' if item['subclass'] == 1 or int(item['InventoryType']) in (4, 19) else None


def bands(levels):
    """Consecutive Skyrim levels grouped to at least BAND items: [(band's level, its levels)], the band at its highest."""
    out, levels_in, count = [], [], 0
    for level, n in sorted(collections.Counter(levels).items()):
        levels_in.append(level)
        count += n
        if count >= BAND:
            out.append((level, set(levels_in)))
            levels_in, count = [], 0
    if levels_in:
        out.append((levels_in[-1], set(levels_in)))
    return out


def pools(pool_items, forms, records):
    """The plugin's leveled lists: a top list per pool and its bands' lists, at most 255 entries each (a band too large
    is a list of lists). Returns the distribution rows for armory.json."""
    def chunked(name, entries, flags):
        if len(entries) <= 255:
            local = forms.get(f'list:{name}')
            records.append(B.leveled(local, name, entries, flags, FILE))
            return local
        count = -(-len(entries) // 255)   # interleaved, so every part spans the band's levels
        return chunked(name, [(1, FILE << 24 | chunked(f'{name}Part{n + 1}', entries[n::count], flags)) for n in range(count)], 0x03)
    legendary_forms = sorted(form for form, level, quality, kind in pool_items if quality == 5)
    legendary = forms.get('list:WoWArmoryLegendary')
    records.append(B.leveled(legendary, 'WoWArmoryLegendary', [(1, f) for f in legendary_forms], 0x03, FILE, LEGENDARY_CHANCE_NONE))
    rows = []
    for name, pool in POOLS.items():
        members = [(form, level, quality) for form, level, quality, kind in pool_items
                   if quality in pool['mix'] and (pool.get('kind') is None or kind == pool['kind'])]
        top = []
        for at, levels in bands(level for _, level, _ in members):
            for quality, weight in sorted(pool['mix'].items()):
                band = sorted((level, form) for form, level, q in members if q == quality and level in levels)
                if band:
                    local = chunked(f'{name}Q{quality}L{at}', band, 0x03)
                    top += [(at, FILE << 24 | local)] * weight
            if pool.get('legendary') and at >= pool['legendary'] and legendary_forms:
                top.append((at, FILE << 24 | legendary))
        if len(top) > 255:
            raise ValueError(f'{name}: {len(top)} entries; raise BAND')
        local = forms.get(f'list:{name}')
        records.append(B.leveled(local, name, top, 0x02, FILE, pool.get('chance_none', 0)))
        rows.append({'list': local, 'name': name,
                     'lists': [{'form': f, 'count': n, 'entries': w} for f, n, w in pool.get('lists', ())],
                     'vendors': [{'form': f, 'count': n} for f, n in pool.get('vendors', ())]})
    return rows


def read_inputs(dump, master):
    """The dump's tables the Armory reads (only the columns it needs where tables are large), the master's templates
    and its armour addons' biped masks."""
    wanted = {'item_template': None, 'spell_template': None, 'spell_affect': None, 'spell_proc_event': None,
              'spell_facing': None, 'spell_bonus_data': None,
              'quest_template': tuple(f'Rew{kind}ItemId{n}' for kind, last in (('Choice', 6), ('', 4)) for n in range(1, last + 1)),
              'creature_template': ('Entry', 'LootId', 'PickpocketLootId'), 'npc_vendor': ('item',),
              'npc_vendor_template': ('item',), 'npc_trainer': ('spell',), 'npc_trainer_template': ('spell',),
              **{t: LOOT for t in ('creature_loot_template', 'reference_loot_template', 'pickpocketing_loot_template') + DROPS}}
    sql = tables(dump, wanted)
    templates, addons = B.read_master(master, {'ARMO', 'WEAP', 'ENCH', 'MGEF', 'SPEL', 'KYWD', 'MISC', 'COBJ', 'PERK'})
    return sql, templates, addons


def build(sql, templates, addons, spell_dbc, dbc_dir, core_spells, gear_path, looks_path, forms_path, output):
    light = json.loads(Path(gear_path).read_text())
    starting = {e for r in dbc.read(Path(dbc_dir) / 'CharStartOutfit.dbc') for e in r['ItemID'] if e > 0}
    chosen, excluded = select(sql, {int(e) for e in light['items']}, starting)
    texts = dbc.by_id(dbc.read(Path(spell_dbc)))
    spell_rows = {r['Id']: r for r in sql['spell_template']}
    spells = {str(k): v for k, v in spell_rows.items()}
    item_rows = {r['entry']: r for r in sql['item_template']}
    defaults = column_defaults(next(iter(chosen.values())))
    curated = B.reviewed_looks(looks_path)
    forms = B.Forms(forms_path, last=0xFFFFFF)
    caps, variation = B.enchantment_caps(templates), B.variations({str(e): r for e, r in chosen.items()})
    records, items, abilities, groups, wearers, told = [], {}, {}, collections.defaultdict(list), {}, set()
    for entry, item in sorted(chosen.items()):
        item = {**item, 'RequiredLevel': 0}
        stats = B.skyrim_stats(item, templates, variation[str(entry)])
        effects, described = B.effects_for(item, {'spells': spells}, texts, strict=False)
        effects = B.capped(effects, stats['level'], caps)
        for sid, amount, text in described:
            if sid not in told:
                told.add(sid)
                records.append(B.equip_effect(templates, forms.get(f'description:{sid}'), sid, texts[sid]['Name_lang'], text, FILE))
            effects.append((f'description:{sid}', amount))
        made = material(item, stats)
        model = B.look(item, stats, templates, curated)
        if item['class'] == 2:
            kind = B.WEAPONS[int(item['subclass'])].removeprefix('Iron').removeprefix('Hunting')
            # Its tier's weapon (keywords, sounds, weight and model); a staff keeps the compatible melee placeholder.
            base = templates.get(f"{stats['tier']}{kind}") if int(item['subclass']) != 10 else None
            record = B.weapon(item, templates, forms.get(f'item:{entry}'), stats, FILE, made, base)
        else:
            base = None
            record = B.armor(item, templates, forms.get(f'item:{entry}'), stats, FILE, made or ('Leather', 'Leather01', None))
        if model is not None and model is not base:
            B.dress(record, model, addons)
        records.append(record)
        if item['class'] == 2 or stats['armor']:   # what has no armour rating is not tempered
            records.append(B.temper(item, templates, forms.get(f'temper:{entry}'), record.form, FILE, made))
        row = {'form': record.form & 0xFFFFFF, 'kind': record.type, 'wow': compact(item, defaults),
               'skyrim': {k: v for k, v in stats.items() if k != 'speed'}}
        from service.equipment import use_spells, use_cooldown
        uses = [use_cooldown(item, n, spell_rows[sid]) for n, sid in use_spells(item)]
        if uses:
            row['use'] = {**uses[0], 'spells': uses}
        if effects:
            key = signature(effects)
            if key not in abilities:
                abilities[key] = effects
            groups[key].append(entry)
            wearers[entry] = slot_group(item)
        if item['itemset']:
            row['set'] = item['itemset']
        items[str(entry)] = row
    # Shared abilities: one per bonus, with a copy for each item that could be worn with it (one per body slot, one
    # per hand), so equal bonuses still add up (skse/src/Gear.cpp gives the worn their copies).
    native = []
    for index, (key, effects) in enumerate(sorted(abilities.items(), key=lambda p: min(groups[p[0]]))):
        # One one-handed entry can be worn twice, even when no other item shares its bonus.
        slots = collections.Counter()
        for e in groups[key]:
            slots[wearers[e]] += 2 if chosen[e]['class'] == 2 and chosen[e]['InventoryType'] in (13, 21, 22) else 1
        copies = sum(min(n, 2 if slot == 'hands' else 1) for slot, n in slots.items())
        digest = hashlib.sha1(repr(key).encode()).hexdigest()[:16]
        resolved = [(FILE << 24 | forms.get(e) if isinstance(e, str) else e, v) for e, v in effects]
        name = item_rows[min(groups[key])]['name'] if len(groups[key]) == 1 else 'WoW equipment'
        native.append([])
        for copy in range(copies):
            local = forms.get(f'ability:{digest}:{copy}')
            records.append(B.magic(templates['AbVampire01b'], local, f'WoWArmoryAbility{index}_{copy}', name, resolved, FILE))
            native[-1].append(local)
        for e in groups[key]:
            items[str(e)]['ability'] = index
    # Sets: every set an Armory item belongs to (none share one with WoWBridge.esp's), its bonuses as abilities.
    sets_dbc = {s['ID']: s for s in dbc.read(Path(dbc_dir) / 'ItemSet.dbc')}
    sets, set_rows, bonused = {}, {}, set()
    for set_id in sorted({r['itemset'] for r in chosen.values() if r['itemset']}):
        row = sets_dbc[set_id]
        members = [e for e in row['ItemID'] if e]
        group = {'id': set_id, 'name': row['Name_lang'], 'items': [{'entry': e, 'name': item_rows[e]['name']} for e in members],
                 'bonuses': [], 'level': max(items[str(e)]['skyrim']['level'] for e in members if str(e) in items),
                 'quality': max(chosen[e]['Quality'] for e in members if e in chosen)}
        for sid, threshold in sorted(zip(row['SetSpellID'], row['SetThreshold']), key=lambda p: p[1]):
            if not sid or not threshold:
                continue
            effects, _ = B.effects_for({'entry': 0, 'spellid_1': sid, 'spelltrigger_1': 1}, {'spells': spells}, texts, describe=False)
            bonus = {'spell': sid, 'threshold': threshold}
            if adaptation := B.SET_ADAPTATIONS.get(str(sid)):
                effects.extend(adaptation['effects'])
                bonus['adaptation'] = adaptation['reason']
            if effects:
                # Keep the first bonus's permanent ID; additional sets/thresholds earn independent copies.
                key = f'set_ability:{sid}' if sid not in bonused else f'set_ability:{set_id}:{threshold}:{sid}'
                bonused.add(sid)
                local = forms.get(key)
                records.append(B.magic(templates['AbVampire01b'], local, f'WoWArmorySetAbility{set_id}_{threshold}_{sid}',
                                       texts[sid]['Name_lang'], effects, FILE))
                bonus['ability'] = local
            group['bonuses'].append(bonus)
        sets[str(set_id)] = group
        set_rows[str(set_id)] = row
    # The lists: hers and WoWBridge.esp's items (its tier sets and iconic weapons are boss and dragon loot) by level,
    # quality and vendor.
    pool_items = [(FILE << 24 | row['form'], row['skyrim']['level'], chosen[int(e)]['Quality'], vendor_kind(chosen[int(e)]))
                  for e, row in items.items()]
    pool_items += [(0x01000000 | row['form'], row['skyrim']['level'], row['quality'], vendor_kind(row['wow']))
                   for row in light['items'].values()]
    distribution = pools(sorted(pool_items), forms, records)
    counts = {'slot': collections.Counter(SLOT_NAMES[r['InventoryType']] for r in chosen.values()),
              'class': collections.Counter(CLASS_NAMES[(r['class'], r['subclass'])] for r in chosen.values()),
              'quality': collections.Counter(str(r['Quality']) for r in chosen.values())}
    counts = {k: dict(sorted(v.items())) for k, v in counts.items()}
    output = Path(output)
    order = {'MGEF': 0, 'SPEL': 1, 'ARMO': 2, 'WEAP': 3, 'COBJ': 4, 'LVLI': 5}
    records.sort(key=lambda r: (order[r.type], r.form))
    esm.write(output / 'esp' / PLUGIN, records, ['Skyrim.esm', 'WoWBridge.esp'], 'WoWBridge', light=False)
    (output / 'armory.json').write_text(json.dumps({
        'plugin': PLUGIN, 'items': items, 'abilities': native, 'sets': sets, 'distribution': distribution,
        'selection': {'items': len(items), 'excluded': excluded, 'counts': counts}}, sort_keys=True, separators=(',', ':')) + '\n')
    # The service: their rows, sets and every spell they use or trigger, as build/catalog.json has them.
    core = {r['Id']: r for r in tables(core_spells, {'spell_template': ('Id',) + COEFFICIENTS})['spell_template']}
    wanted = {r[f'spellid_{n}'] for r in chosen.values() for n in range(1, 6) if r[f'spellid_{n}']}
    wanted |= {sid for row in set_rows.values() for sid in row['SetSpellID'] if sid}
    service_spells = closure(wanted, spell_rows, core, {r['entry']: r for r in sql['spell_bonus_data']})
    (output / 'armory_catalog.json').write_text(json.dumps({
        'items': {str(e): compact(r, defaults) for e, r in sorted(chosen.items())}, 'item_defaults': defaults,
        'item_sets': set_rows, 'spells': service_spells, **spell_tables(service_spells, Path(dbc_dir)),
        **{t: [r for r in sql[t] if r['entry'] in service_spells] for t in ('spell_affect', 'spell_proc_event', 'spell_facing')}},
        sort_keys=True, separators=(',', ':'), default=str) + '\n')
    forms.save()
    return records, items, {'excluded': excluded, 'counts': counts}


def use_inventory(cat):
    """Reproducible docs inventory from the same complete item/spell rows the service uses."""
    from service import equipment as E
    effect_names = {3: 'scripted effect', 5: 'teleport', 24: 'create item', 34: 'change item',
                    41: 'wild summon', 42: 'guardian summon', 50: 'world portal', 56: 'pet summon',
                    63: 'threat', 64: 'trigger spell', 89: 'totem summon', 96: 'charge',
                    97: 'critter summon', 108: 'mechanic dispel'}
    aura_names = {4: 'scripted aura', 10: 'threat modifier', 14: 'flat damage taken',
                  42: 'proc trigger', 56: 'transformation', 58: 'swim speed',
                  60: 'pacify/silence', 64: 'periodic mana leech', 73: 'flat spell cost',
                  74: 'school reflection', 77: 'mechanic immunity', 79: 'damage percent',
                  82: 'water breathing', 85: 'power regeneration', 101: 'resistance percent',
                  105: 'slow fall', 107: 'flat spell modifier', 108: 'percent spell modifier',
                  123: 'spell penetration', 136: 'healing percent', 72: 'percent spell cost'}
    rows, classes = [], collections.Counter()
    for entry in sorted(cat.armory):
        item = cat.item(entry)
        for _, sid in E.use_spells(item):
            s, labels = cat.spell(sid), []
            for e in (1, 2, 3):
                effect, aura = s[f'Effect{e}'], s[f'EffectApplyAuraName{e}']
                if not effect:
                    continue
                if effect in (6, 35):
                    label = ('periodic dispel' if aura == 23 and not E.use_support(cat, sid) else
                             'periodic trigger' if aura == 23 else
                             E.USE_AURAS.get(aura, aura_names.get(aura, f'aura {aura}')))
                    if aura in (22, 143):
                        label = 'armor' if s[f'EffectMiscValue{e}'] == 1 else 'resistance'
                    elif aura == 24:
                        label = 'periodic ' + {0: 'mana', 1: 'rage', 3: 'energy'}.get(s[f'EffectMiscValue{e}'], 'power') + ' restore'
                    label += f' (aura {aura})'
                else:
                    label = E.USE_EFFECTS.get(effect, effect_names.get(effect, f'effect {effect}'))
                    if effect == 30:
                        label = {0: 'mana', 1: 'rage', 3: 'energy'}.get(s[f'EffectMiscValue{e}'], 'power') + ' restore'
                    label += f' (effect {effect})'
                if label not in labels:
                    labels.append(label)
            classes.update(labels)
            reasons = E.item_use_support(cat, item)
            for kind, names in (('effect', effect_names), ('aura', aura_names)):
                reasons = [re.sub(r'unsupported ' + kind + r' (\d+)',
                                  lambda m: names.get(int(m[1]), m[0]), reason) for reason in reasons]
            rows.append((entry, item['name'], sid, s['SpellName'], labels, reasons))
    supported = [r for r in rows if not r[-1]]
    lines = ['## Use spell inventory (1.12.1.5875)', '',
             f'{len(rows)} item/spell pairs on {len({r[0] for r in rows})} Armory items; '
             f'{len({r[2] for r in rows})} distinct spells. '
             f'**{len(supported)} supported pairs ({len({r[2] for r in supported})} distinct spells), '
             f'{len(rows) - len(supported)} unsupported pairs.** Every effect of a use must be supported; '
             'a rejected compound spell applies nothing and starts no cooldown.', '',
             'Effect classes below overlap (an item can heal and buff). The per-item table records every populated '
             '`spellid_N` with `spelltrigger_N = 0`, including items whose use has no cooldown.', '',
             '| Effect class | Item/spell pairs |', '| --- | --- |']
    lines += [f'| {label} | {count} |' for label, count in sorted(classes.items())]
    lines += ['', '| Item entry / name | Spell ID / name | Effect classes | Support / rejection reason |',
              '| --- | --- | --- | --- |']
    lines += [f'| {entry} {name} | {sid} {spell} | {"; ".join(labels)} | '
              f'{"No: " + "; ".join(reasons) if reasons else "Yes"} |'
              for entry, name, sid, spell, labels, reasons in rows]
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dump', type=Path, default=DEFAULT_DUMP)
    ap.add_argument('--master', type=Path, default=B.MASTER)
    ap.add_argument('--spell-dbc', type=Path, default=REPO / 'build/dbc/DBFilesClient/Spell.dbc')
    ap.add_argument('--dbc-dir', type=Path, default=DBC_DIR)
    ap.add_argument('--core-spells', type=Path, default=CORE_SPELLS)
    ap.add_argument('--gear', type=Path, default=REPO / 'build/gear.json', help="data/build_esp.py's output")
    ap.add_argument('--looks', type=Path, default=REPO / 'data/skyrim_gear_looks.json')
    ap.add_argument('--forms', type=Path, default=REPO / 'data/armory_forms.json')
    ap.add_argument('--out', type=Path, default=REPO / 'build')
    ap.add_argument('--use-inventory', type=Path, metavar='CATALOG',
                    help='print the use-spell Markdown inventory from catalog.json + adjacent armory_catalog.json; no build')
    args = ap.parse_args()
    if args.use_inventory:
        from service.catalog import Catalog
        print(use_inventory(Catalog(args.use_inventory)), end='')
        return
    sql, templates, addons = read_inputs(args.dump, args.master)
    records, items, report = build(sql, templates, addons, args.spell_dbc, args.dbc_dir, args.core_spells, args.gear, args.looks,
                                   args.forms, args.out)
    print(f"{args.out}/esp/{PLUGIN}: {len(items)} items, {len(records)} records; armory.json and armory_catalog.json written")
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
