#!/usr/bin/env python3
"""Generate inventory gear from the catalog; object IDs are permanent save-game identities.

gear.json uses entry-keyed items: kind, local form ID (resolve against plugin),
WoW entry, name, and a local equip ability ID. Skyrim owns the converted
bonuses; equip-spell effects without an equivalent remain in the WoW service.
"""
import argparse
import collections
import copy
import json
import re
import statistics
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from data import dbc, esm
from data.build_skyrim_gear_looks import biped_mask
from service.skyrim_effects import STAT, RESISTS, RESIST_DIVISOR
from service.progression import skill_value

REPO = Path(__file__).resolve().parent.parent
SET_ADAPTATIONS = json.loads((REPO / 'data/equipment.json').read_text()).get('set_adaptations', {})
MASTER = Path(__file__).resolve().parents[2] / 'steamapps/common/Skyrim Special Edition/Data/Skyrim.esm'
LOOKS = REPO / 'data/skyrim_gear_looks.json'   # data/build_skyrim_gear_looks.py: Skyrim records by the WoW display they look like
SLOTS = {1: 30, 2: 35, 3: 57, 4: 56, 5: 32, 6: 49, 7: 53, 8: 37,
         9: 34, 10: 33, 11: 36, 12: 60, 14: 39, 15: 61, 16: 47,
         19: 56, 20: 32, 23: 39, 25: 61, 26: 61, 28: 61}
WEAPONS = {0: 'IronWarAxe', 1: 'IronBattleaxe', 2: 'HuntingBow', 3: 'HuntingBow',
           4: 'IronMace', 5: 'IronWarhammer', 6: 'IronGreatsword', 7: 'IronSword',
           8: 'IronGreatsword', 10: 'IronWarhammer', 13: 'IronMace', 15: 'IronDagger',
           16: 'IronDagger', 18: 'HuntingBow', 19: 'IronMace', 20: 'HuntingBow'}

# A WoW item is measured against what Skyrim gives at the level its ItemLevel matches (interpolated between rows):
# "as good or a little better than the Skyrim equipment they would have available to them to obtain at their level".
LEVELS = ((5, 1), (20, 8), (35, 18), (50, 27), (60, 36), (66, 40), (76, 48), (80, 52))
# Vanilla's tiers at the level its leveled lists first give them (LItemArmor*Special, LItemWeapon*, LItemEnchArmorHeavy*);
# ratings, damage, speed, values and enchantment magnitudes come from the records themselves.
HEAVY = ((1, 'Iron'), (6, 'Steel'), (12, 'Dwarven'), (18, 'SteelPlate'), (25, 'Orcish'), (32, 'Ebony'),
         (40, 'Dragonplate'), (48, 'Daedric'))
LIGHT = ((1, 'Hide'), (6, 'Leather'), (12, 'Elven'), (19, 'Scaled'), (27, 'ElvenGilded'), (36, 'Glass'), (46, 'Dragonscale'))
ARMS = ((1, 'Iron'), (2, 'Steel'), (6, 'Orcish'), (12, 'Dwarven'), (19, 'Elven'), (27, 'Glass'), (36, 'Ebony'), (46, 'Daedric'))
ENCHANTMENTS = ((1, 1), (4, 2), (6, 3), (17, 4), (30, 5), (47, 6))   # EnchArmorFortifyHealth01..06 and their kin
PIECES = {1: 'Helmet', 5: 'Cuirass', 20: 'Cuirass', 10: 'Gauntlets', 8: 'Boots', 14: 'Shield'}
# Skyrim looks where no reviewed one exists, by the item's light tier (Hide, Leather | Elven, Scaled | Gilded, Glass,
# Dragonscale): cloth as robes, hoods, gloves and shoes; necklaces and rings as Skyrim's; trinkets and things held in
# the off hand as gems; shirts and tabards as fine clothes.
CLOTH = {'Cuirass': ('ClothesMonkRobes', 'ClothesWarlockRobes', 'ClothesPsiijicRobes'),
         'Helmet': ('ClothesCollegeHood', 'ClothesRedguardHood', 'ClothesPsiijicHood'),
         'Gauntlets': ('ClothesFarmGloves03', 'ClothesMythicDawnGloves', 'ClothesThalmorGloves'),
         'Boots': ('ClothesBoots01', 'ClothesMGBoots', 'ClothesMGBootsArchmage')}
ACCESSORIES = {2: ('JewelryNecklaceSilver', 'JewelryNecklaceGold', 'JewelryNecklaceGoldDiamond'),
               11: ('JewelryRingSilver', 'JewelryRingGold', 'JewelryRingGoldDiamond'),
               12: ('GemAmethyst', 'GemSapphireFlawless', 'GemDiamondFlawless'),
               23: ('GemAmethyst', 'GemSapphireFlawless', 'GemDiamondFlawless'),
               4: ('ClothesFineClothes01',) * 3, 19: ('ClothesRadiantRaimentFineClothes01',) * 3}
# The Skyrim piece whose look a WoW slot takes: shoulders, waist and legs a cuirass's, wrists a gauntlets'.
PARTS = {1: 'Helmet', 3: 'Cuirass', 5: 'Cuirass', 20: 'Cuirass', 6: 'Cuirass', 7: 'Cuirass', 16: 'Cuirass',
         8: 'Boots', 9: 'Gauntlets', 10: 'Gauntlets', 14: 'Shield'}
MODELS = ('MOD2', 'MO2T', 'MO2S', 'MOD4', 'MO4T', 'MO4S')
# WoW's own armour shares of a chest (Tier 1 plate: 608/749 head, 562 shoulders...; a cloak about three-fifths). The
# slots Skyrim lacks (shoulders, waist, legs, wrists, back) share 12.5% of the four-piece total by these: "a full WoW
# set should [be] a little strong, but not overwhelmingly so", 1.10x a Skyrim set of its tier in green to 1.25x in
# epic/legendary, while a WoW chest worn with Skyrim pieces is a fair chest.
SHARES = {1: .8125, 3: .75, 5: 1, 20: 1, 6: .5625, 7: .875, 8: .6875, 9: .4375, 10: .625, 16: .6}
EXTRA_SLOTS, EXTRA_BUDGET = (3, 6, 7, 9, 16), .125
CLASS_FACTORS = {1: .6, 2: 1, 3: .9, 4: 1, 6: 1}   # cloth, leather (light tiers); mail, plate, shield (heavy tiers)
QUALITY_FACTORS = (.85, .925, 1, 1.05, 1.1, 1.11)   # grey .. legendary; never more than 15% above the tier
ALLOYS = {'Iron': ('IngotIron', None), 'Steel': ('IngotSteel', 'SteelSmithing'), 'Orcish': ('IngotOrichalcum', 'OrcishSmithing'),
          'Dwarven': ('IngotDwarven', 'DwarvenSmithing'), 'Elven': ('IngotIMoonstone', 'ElvenSmithing'),
          'Glass': ('IngotMalachite', 'GlassSmithing'), 'Ebony': ('IngotEbony', 'EbonySmithing'),
          'Daedric': ('IngotEbony', 'DaedricSmithing')}
LEARNABLES = REPO / 'data/learnables.json'   # data/build_learnables.py: the books and mount items
# A book is a spell tome of its rank's school (feral and forms read as Alteration), its value vanilla's tome at the tier
# of skill the rank asks (Flames 50 .. Expert 1300; Skyrim's 25/50/75 perk tiers on the service's curve).
TOMES = {'destruction': 'SpellTomeFlames', 'restoration': 'SpellTomeHealing', 'alteration': 'SpellTomeOakflesh',
         'one_handed': 'SpellTomeOakflesh', 'block': 'SpellTomeOakflesh', 'two_handed': 'SpellTomeOakflesh'}
TOME_VALUES = ((25, 50), (50, 300), (75, 650), (101, 1300))
# Mount items by the user's tiers: swift mounts at level 30, flyers at 46. Skyrim sells a horse for 1000 by dialogue
# but barters at 2.0-3.3 times an item's value (fBarterMin/Max), so these values ask about 5,000 and 15,000 at Speech 50.
REINS = {False: (30, 2000), True: (46, 6000)}
# The plugin's lists and where each joins at data load (skse/src/Distribution.cpp): Skyrim.esm lists, or vendor factions'
# merchant chests, (form, count). Only those no NPC's inventory, death item or outfit reaches (data/tests/test_build_esp.py
# checks), never LItemSpellTomes00<school> (bandit, draugr and warlock mages carry those).
LISTS = {'all': ('WoWSpellBooks', 'lists', ((0x10FD8C, 1),    # LItemSpellTomes00Spells: boss chests, blacksmiths
                                            (0xA297A, 3))),   # LItemSpellTomes00AllSpells: every court wizard
         'destruction': ('WoWSpellBooksDestruction', 'lists', ((0x10F650, 2),)),   # LItemSpellTomes00All<school>: the
         'restoration': ('WoWSpellBooksRestoration', 'lists', ((0x10F652, 2),)),   # College's Enthir and Faralda, Colette
         'alteration': ('WoWSpellBooksAlteration', 'lists', ((0x10F64E, 2),)),     # and Tolfdir
         # Solitude's stables (Windhelm's stables faction has no merchant chest), and the three Khajiit caravans
         'stables': ('WoWMountItemsStables', 'vendors', ((0xA6C01, 1),)),
         'caravans': ('WoWMountItemsCaravan', 'vendors', ((0x74351, 2), (0x74352, 2), (0x74354, 2))),
         # A Paladin's librams: the same treasure and court wizards, and the College's healers for Holy spells.
         'paladin_all': ('WoWPaladinBooks', 'lists', ((0x10FD8C, 1), (0xA297A, 3))),
         'paladin_restoration': ('WoWPaladinBooksRestoration', 'lists', ((0x10F652, 2),))}
# Each class's book lists roll only for that class: their chance none is a global the plugin sets for the loaded
# character (skse/src/Distribution.cpp SetClass): 0 for hers, 100 for the other class's (lists join at data load).
CLASS_BOOK_LISTS = {11: ('WoWBridgeDruidBooksNone', ('all', 'destruction', 'restoration', 'alteration')),
                    2: ('WoWBridgePaladinBooksNone', ('paladin_all', 'paladin_restoration'))}


def skyrim_level(item_level):
    """The Skyrim level whose gear a WoW ItemLevel matches."""
    item_level = float(item_level or 0)
    for (a, s), (b, t) in zip(LEVELS, LEVELS[1:]):
        if item_level <= b:
            return round(s + (t - s) * (max(a, item_level) - a) / (b - a))
    return LEVELS[-1][1]


def best(level, tiers, stats):
    """Vanilla's best by this level: the newest tier that has the piece, and each stat at its highest so far (vanilla's
    Orcish Greatsword sells for less than its Steel one). stats(material) is None where vanilla has no such piece."""
    material, top = None, None
    for at, name in tiers:
        found = stats(name) if at <= level else None
        if found:
            material, top = name, found if top is None else tuple(map(max, top, found))
    return material, top


def variations(items):
    """Each item's WoW strength within its band (same class, subclass, quality and item level), capped at +-10%:
    armour per chest share, or weapon damage per second."""
    def power(item):
        if item['class'] == 2:
            delay = float(item.get('delay') or 0)
            total = sum(float(item.get(f'dmg_min{n}', 0)) + float(item.get(f'dmg_max{n}', 0)) for n in range(1, 6))
            return total / 2 / delay if delay > 0 else 0
        return float(item.get('armor', 0)) / SHARES.get(int(item['InventoryType']), 1)
    band = lambda item: tuple(int(item.get(k) or 0) for k in ('class', 'subclass', 'Quality', 'ItemLevel'))
    bands = {}
    for item in items.values():
        bands.setdefault(band(item), []).append(power(item))
    out = {}
    for entry, item in items.items():
        middle = statistics.median(bands[band(item)])
        out[entry] = min(1.1, max(.9, power(item) / middle)) if middle > 0 else 1.
    return out


def skyrim_stats(item, templates, variation=1.):
    """Her WoW item as Skyrim gear of its level: the vanilla tier it is measured against, its armour rating or damage
    (speed-adjusted to the tier's damage per second) and value. The one source for the ESP and the tooltips."""
    level = skyrim_level(item.get('ItemLevel'))
    factor = min(1.11, QUALITY_FACTORS[min(5, int(item.get('Quality', 1)))] * variation)   # P5: outfits then stay under 1.25x
    sub, slot = int(item['subclass']), int(item['InventoryType'])
    sells = int(item.get('SellPrice', 0)) > 0   # what WoW will not buy stays worth 1
    if item['class'] == 2:
        if sub not in WEAPONS:
            raise ValueError(f"item {item.get('entry')}: unsupported weapon subclass {sub}")
        kind = WEAPONS[sub].removeprefix('Iron').removeprefix('Hunting')   # a staff swings as a warhammer

        def arm(material):
            r = templates.get(material + kind) or (templates[WEAPONS[sub]] if material == 'Iron' else None)
            if r:
                value, _, damage = struct.unpack('<IfH', r.get('DATA'))
                return damage * struct.unpack_from('<f', r.get('DNAM'), 4)[0], value
        tier, (rate, value) = best(level, ARMS, arm)
        delay = float(item.get('delay') or 0)
        speed = 2.6 / (delay / 1000) if delay > 0 else 1
        return {'level': level, 'tier': tier, 'damage': max(1, round(rate / speed * factor)), 'speed': speed,
                'value': max(1, round(value * factor)) if sells else 1}
    if not float(item.get('armor', 0)) or sub not in CLASS_FACTORS or slot not in PIECES and slot not in SHARES:
        # Jewelry, relics and the like are Skyrim's armourless kinds; they keep the WoW sell price.
        return {'level': level, 'tier': None, 'armor': 0, 'value': max(1, int(item.get('SellPrice', 0)) // 100)}
    tiers = HEAVY if sub in (3, 4, 6) else LIGHT

    def piece(part):
        def stats(material):   # Steel's pieces are A/B variants; Steel Plate, Leather and Scaled have no shield
            r = templates.get(f'Armor{material}{part}') or templates.get(f'Armor{material}{part}A')
            return r and (struct.unpack('<I', r.get('DNAM'))[0] / 100, struct.unpack('<I', r.get('DATA')[:4])[0])
        return best(level, tiers, stats)
    if slot in PIECES:
        tier, (rating, value) = piece(PIECES[slot])
    else:
        parts = [piece(part) for part in ('Cuirass', 'Helmet', 'Gauntlets', 'Boots')]
        tier = parts[0][0]
        share = EXTRA_BUDGET * SHARES[slot] / sum(SHARES[s] for s in EXTRA_SLOTS)
        rating, value = (sum(stats[n] for _, stats in parts) * share for n in (0, 1))
    factor *= CLASS_FACTORS[sub]
    return {'level': level, 'tier': tier, 'armor': max(1, round(rating * factor)),
            'value': max(1, round(value * factor)) if sells else 1}


def enchantment_caps(templates):
    """Vanilla armour enchantment magnitudes by native effect and tier (EnchArmorFortifyMagicka01..06 ...)."""
    caps = {}
    for edid, r in templates.items():
        if r.type == 'ENCH' and (m := re.fullmatch(r'EnchArmor\w+0([1-6])', edid)) and len(r.all('EFID')) == 1:
            caps.setdefault(struct.unpack('<I', r.get('EFID'))[0], {})[int(m[1])] = struct.unpack('<fII', r.get('EFIT'))[0]
    return caps


def capped(effects, level, caps):
    """No item's native bonus exceeds the vanilla enchantment of that effect obtainable by its level (the lowest
    vanilla has where an effect starts later, as Fortify Magicka Rate does at 03)."""
    tier = max(n for at, n in ENCHANTMENTS if at <= level)
    out = []
    for effect, magnitude in effects:
        if tiers := caps.get(effect):
            magnitude = min(magnitude, tiers[max((n for n in tiers if n <= tier), default=min(tiers))])
        out.append((effect, magnitude))
    return out


def u32(n):
    return struct.pack('<I', n)


class Forms:
    """A plugin's permanent object IDs by key: a light plugin's 0x800-0xFFF, or a regular plugin's up to 0xFFFFFF."""
    def __init__(self, path, last=0xFFF):
        self.path, self.last = Path(path), last
        self.ids = json.loads(self.path.read_text()) if self.path.exists() else {}
        values = list(self.ids.values())
        if len(set(values)) != len(values) or any(not 0x800 <= n <= last for n in values):
            raise ValueError(f'{self.path.name} contains duplicate or out-of-range object IDs')

    def get(self, key):
        if key not in self.ids:
            # Never recycle removed entries: old saves may still reference them.
            n = max(self.ids.values(), default=0x7FF) + 1
            if n > self.last:
                raise ValueError('WoWBridge exceeds the 2048-object ESL budget' if self.last == 0xFFF else
                                 f'{self.path.name} exceeds its object ID range')
            self.ids[key] = n
        return self.ids[key]

    def save(self):
        self.path.write_text(json.dumps(self.ids, indent=2, sort_keys=True) + '\n')


def clone(template, form, edid, name, file=1):
    r = copy.deepcopy(template)
    r.form = file << 24 | form  # Skyrim.esm is master 0; this plugin is file 1 (the Armory, after WoWBridge.esp, is 2).
    r.flags = 0
    r.drop('DESC', 'CTDA', 'CIS1', 'CIS2', 'VMAD', 'EITM', 'EAMT')
    r.set('EDID', esm.zstring(edid))
    r.set('FULL', esm.zstring(name))
    return r


def insert_before(r, anchor, subs):
    i = next((i for i, (t, _) in enumerate(r.subs) if t == anchor), len(r.subs))
    r.subs[i:i] = subs


def set_keywords(r, keywords):
    r.set('KSIZ', u32(len(keywords)))
    r.set('KWDA', b''.join(u32(k) for k in keywords))


def effects_for(item, catalog, texts, describe=True, strict=True):
    native, descriptive = {}, []

    def add(stat, value):
        for effect, scale in STAT.get(stat, []):
            native[effect] = native.get(effect, 0) + value * scale

    for i in range(1, 11):
        add(int(item.get(f'stat_type{i}', 0)), float(item.get(f'stat_value{i}', 0)))
    for field, effect in RESISTS.items():
        native[effect] = native.get(effect, 0) + float(item.get(field, 0)) / RESIST_DIVISOR
    for i in range(1, 6):
        sid = int(item.get(f'spellid_{i}', 0))
        if not sid or item.get(f'spelltrigger_{i}') != 1:
            continue
        spell = catalog['spells'].get(str(sid))
        if spell is None:
            raise ValueError(f"item {item['entry']}: missing equip spell {sid}")
        unsupported = []
        for e in (1, 2, 3):
            if not spell.get(f'Effect{e}'):
                continue
            amount = float(spell.get(f'EffectBasePoints{e}', 0)) + float(spell.get(f'EffectBaseDice{e}', 1))
            aura = spell.get(f'EffectApplyAuraName{e}')
            misc = spell.get(f'EffectMiscValue{e}', 0)
            if spell[f'Effect{e}'] == 6 and aura == 29 and misc in (-1, 0, 1, 2, 3, 4):
                for which, stat in enumerate((4, 3, 7, 5, 6)):
                    if misc in (-1, which):
                        add(stat, amount)
            elif spell[f'Effect{e}'] == 6 and aura == 99:
                add(38, amount)
            elif spell[f'Effect{e}'] == 6 and aura in (124, 131):
                native[0x7A0FE] = native.get(0x7A0FE, 0) + abs(amount) * .1
            elif sid == 23236 and aura == 107:  # Benediction's Holy critical bonus
                native[0x7A101] = native.get(0x7A101, 0) + abs(amount)
            elif spell[f'Effect{e}'] == 6 and aura == 85 and misc == 0:
                add(43, amount)
            elif spell[f'Effect{e}'] == 6 and aura in (30, 47, 49):
                # Defense, parry and dodge improve physical survivability:
                # one native armor point per defense skill/percentage point.
                native[0xE5F55] = native.get(0xE5F55, 0) + abs(amount)
            elif spell[f'Effect{e}'] == 6 and aura in (51, 158):
                native[0x7A0F3] = native.get(0x7A0F3, 0) + abs(amount)
            else:
                unsupported.append((e, amount))
        if unsupported and describe:
            text = texts.get(sid, {}).get('Description_lang', '')
            if not text and not strict:
                continue   # a server-side spell the client never describes: the service alone carries it
            if not text:
                raise ValueError(f'no client description for equip spell {sid}')
            # One displayed effect per spell; other placeholders use their own amounts.
            first, amount = unsupported[0]
            for e in (1, 2, 3):
                value = float(spell.get(f'EffectBasePoints{e}', 0)) + float(spell.get(f'EffectBaseDice{e}', 1))
                text = re.sub(r'\$s' + str(e) + r'\b', '<mag>' if e == first else f'{value:g}', text, flags=re.I)
            # Proc tooltips quote the triggered spell, e.g. Skullflame's $18817s1.
            def referenced(match):
                ref = catalog['spells'][match[1]]
                e = match[2]
                return f"{float(ref.get(f'EffectBasePoints{e}', 0)) + float(ref.get(f'EffectBaseDice{e}', 1)):g}"
            text = re.sub(r'\$(\d+)s([123])\b', referenced, text, flags=re.I)
            descriptive.append((sid, amount, text))
    return [(e, v) for e, v in sorted(native.items()) if v], descriptive


def equip_effect(templates, form, sid, name, text, file=1):
    """An equip spell Skyrim has no effect for: a magic effect that only says it (the client's description, its amount
    the ability's magnitude); the service carries what it does."""
    r = clone(templates['EnchFortifyHealthConstantSelf'], form, f'WoWEquipEffect{sid}', name, file)
    r.set('DNAM', esm.zstring(text))
    data = bytearray(r.get('DATA'))
    struct.pack_into('<i', data, 12, -1)
    struct.pack_into('<I', data, 72, 77)
    r.set('DATA', bytes(data))
    return r


def magic(template, form, edid, name, effects, file=1):
    r = clone(template, form, edid, name, file)
    r.drop('EFID', 'EFIT')
    if r.type == 'ENCH':
        r.set('ENIT', struct.pack('<9I', 0, 1, 0, 0, 0, 6, 0, 0, 0))
    else:
        r.set('SPIT', struct.pack('<3If2If2I', 0, 0, 4, 0, 0, 0, 0, 0, 0))
    for effect, magnitude in effects:
        r.subs.extend([('EFID', u32(effect)), ('EFIT', struct.pack('<fII', magnitude, 0, 0))])
    return r


def crafting_material(item):
    """Keep soft/wooden gear soft; use level-appropriate alloys for metal gear (a metal weapon's is its tier's)."""
    level = int(item.get('ItemLevel', 0))
    if item['class'] == 2:
        if int(item.get('Material', 0)) != 1:
            return 'Wood', 'LeatherStrips', None
        tier = max((at, name) for at, name in ARMS if at <= skyrim_level(level))[1]
        return (tier, *ALLOYS[tier])
    if item['subclass'] in (3, 4, 6):
        if item['subclass'] == 4 and level >= 60:
            return 'SteelPlate', 'IngotCorundum', 'AdvancedArmors'
        if level >= 70:
            return 'Orcish', 'IngotOrichalcum', 'OrcishSmithing'
        return 'Steel', 'IngotSteel', 'SteelSmithing'
    return 'Leather', 'Leather01', None


def temper(item, templates, form, created, file=1, material=None):
    material, ingredient, perk = material or crafting_material(item)
    # Vanilla's pair of OR conditions: unenchanted OR Arcane Blacksmith.
    template = templates['TemperWeaponIronSword' if item['class'] == 2 else 'TemperArmorLeatherCuirass']
    r = copy.deepcopy(template)
    r.form, r.flags = file << 24 | form, 0
    r.set('EDID', esm.zstring(f"WoWTemper{item['entry']}"))
    r.set('CNTO', struct.pack('<II', templates[ingredient].form, 1))
    r.set('CNAM', u32(created))
    if perk:
        # HasPerk == 1, run on Subject, no OR flag; material perks also double
        # improvement through their vanilla entry points and matching keywords.
        condition = struct.pack('<B3xfH2xIIIII', 0, 1., 448, templates[perk].form, 0, 0, 0, 0xFFFFFFFF)
        insert_before(r, 'CNAM', [('CTDA', condition)])
    return r


def armor(item, templates, form, stats, file=1, material=None):
    slot = SLOTS.get(int(item['InventoryType']))
    if slot is None:
        raise ValueError(f"item {item['entry']}: unsupported inventory type {item['InventoryType']}")
    subtype = int(item['subclass']) if item['class'] == 4 else 0
    # Keep WoW subclasses for conversion, but every ARMO trains a Skyrim armour skill.
    category = subtype if subtype in (1, 2, 3, 4) else 4 if subtype == 6 else 1
    body = {30: 'Helmet', 33: 'Gauntlets', 37: 'Boots'}.get(slot, 'Cuirass')
    prefix = {1: 'ArmorLeather', 2: 'ArmorLeather', 3: 'ArmorSteel', 4: 'ArmorSteel'}[category]
    edid = prefix + body + ('A' if category in (3, 4) else '')
    if slot in (35, 36):
        edid = 'JewelryNecklaceGold' if slot == 35 else 'JewelryRingGold'
    elif item['InventoryType'] == 14:
        edid = 'ArmorSteelShield'
    r = clone(templates[edid], form, f"WoWItem{item['entry']}", item['name'], file)
    r.drop('MODL', 'BODT')   # the template's worn look: dress() gives its own
    if int(item['InventoryType']) == 28:  # Relics have no standalone WoW geometry.
        # Keep the relic slot/stats and use native inventory/drop geometry for both sexes.
        idol = templates['StatueDibellaGold']
        r.drop('MOD2', 'MO2T', 'MO2S', 'MOD4', 'MO4T', 'MO4S')
        insert_before(r, 'BOD2', [('MOD2', idol.get('MODL')), ('MO2T', idol.get('MODT')),
                                ('MOD4', idol.get('MODL')), ('MO4T', idol.get('MODT'))])
        r.set('OBND', idol.get('OBND'))
    armor_type = 1 if category in (3, 4) else 0
    # A helm takes the head and the hair, as Skyrim's helmets do: wearing one takes the other off.
    mask = (1 << 0) | (1 << 1) if slot == 30 else 1 << (slot - 30)
    r.set('BOD2', struct.pack('<II', mask, armor_type))
    kw = [templates['ArmorHeavy' if armor_type else 'ArmorLight'].form,
          templates['ArmorMaterial' + (material or crafting_material(item))[0]].form]
    slot_kw = {30: 'ArmorHelmet', 32: 'ArmorCuirass', 33: 'ArmorGauntlets', 37: 'ArmorBoots', 39: 'ArmorShield',
               35: 'ClothingNecklace', 36: 'ClothingRing',
               34: 'ClothingHands', 60: 'ClothingRing', 61: 'ClothingRing'}.get(slot, 'ClothingBody')
    if slot_kw:
        kw.append(templates[slot_kw].form)
    vendor = 'VendorItemJewelry' if slot in (35, 36) else 'VendorItemArmor'
    kw.append(templates[vendor].form)
    set_keywords(r, kw)
    weight = {30: 2, 32: 6, 33: 2, 37: 2, 35: .5, 36: .25, 39: 4}.get(slot, 1)
    if slot not in (35, 36):
        weight *= {1: .5, 2: 1, 3: 1.5, 4: 5}[category]
    r.set('DATA', struct.pack('<If', stats['value'], weight))
    r.set('DNAM', u32(stats['armor'] * 100))
    return r


def weapon(item, templates, form, stats, file=1, material=None, base=None):
    subclass = int(item['subclass'])
    if subclass not in WEAPONS:
        raise ValueError(f"item {item['entry']}: unsupported weapon subclass {subclass}")
    r = clone(base or templates[WEAPONS[subclass]], form, f"WoWItem{item['entry']}", item['name'], file)
    # Equip bonuses live in an ability, leaving the native enchantment slot free.
    keywords = list(struct.unpack('<' + 'I' * (len(r.get('KWDA')) // 4), r.get('KWDA')))
    materials = {t.form for t in templates.values() if t.edid.startswith('WeapMaterial')}
    keywords = [k for k in keywords if k not in materials]
    set_keywords(r, keywords + [templates['WeapMaterial' + (material or crafting_material(item))[0]].form])
    if subclass == 10:
        # Staff01's Prn is WeaponStaff, absent from the humanoid skeleton. Keep the
        # melee staff's warhammer animation and a mesh whose Prn is WeaponBack.
        r.set('MODL', esm.zstring(r'Weapons\Iron\WarHammer.nif'))
        r.drop('MODT')
    damage = stats['damage']
    r.set('DATA', struct.pack('<IfH', stats['value'], struct.unpack_from('<f', r.get('DATA'), 4)[0], min(65535, damage)))
    dnam = bytearray(r.get('DNAM'))
    struct.pack_into('<f', dnam, 4, stats['speed'])
    r.set('DNAM', bytes(dnam))
    # Do not retain the template's unrelated critical hit damage.
    crit = bytearray(r.get('CRDT'))
    struct.pack_into('<H', crit, 0, round(damage / 2))
    r.set('CRDT', bytes(crit))
    return r


def light_tier(level):
    """0, 1 or 2: a light tier's rank in Skyrim's (Hide, Leather | Elven, Scaled | Elven Gilded, Glass, Dragonscale)."""
    return sum(level >= at for at, name in LIGHT if name in ('Elven', 'ElvenGilded'))


def reviewed_looks(path=LOOKS):
    """data/skyrim_gear_looks.json read backwards: its Skyrim.esm records by the WoW display they look like, in form order."""
    curated = collections.defaultdict(list)
    for row in sorted(json.loads(Path(path).read_text())['items'].values(), key=lambda r: r['local_form_id']):
        if row['plugin'] == 'Skyrim.esm':
            curated[row['display_id']].append(row)
    return curated


def look(item, stats, templates, curated):
    """The Skyrim record whose look her item has wherever WoWBridge draws no WoW model of it: a reviewed look of its
    WoW display (data/skyrim_gear_looks.json, a Skyrim.esm record of the same kind and slot), else one of its armour
    class, slot and tier, or its weapon type's tier."""
    slot = int(item['InventoryType'])
    if item['class'] == 2 and int(item['subclass']) == 10:
        return None   # keep weapon()'s compatible melee mesh, including for reviewed staff displays
    for row in curated.get(int(item['displayid']), ()):
        r = templates.get(row['edid'])
        if r and (r.type == 'WEAP' if item['class'] == 2 else r.type == 'ARMO' and r.get('MOD2')) and (
                row['subclass'] == item['subclass'] if item['class'] == 2 else
                SLOTS.get(row['inventory_type']) == SLOTS.get(slot)):
            return r
    level, sub = stats['level'], int(item['subclass'])
    if item['class'] == 2:
        kind = WEAPONS[sub].removeprefix('Iron').removeprefix('Hunting')
        return None if sub == 10 else templates.get(f"{stats['tier']}{kind}") or templates[WEAPONS[sub]]
    if slot == 28:
        return None   # relics keep armor()'s statue
    if sub in (3, 4, 6, 2) and slot in PARTS:
        tiers = HEAVY if sub in (3, 4, 6) else LIGHT
        names = [name for at, name in tiers if at <= level][::-1]
        for name in names:   # the newest tier that has the piece (Steel Plate has no shield)
            if r := templates.get(f'Armor{name}{PARTS[slot]}') or templates.get(f'Armor{name}{PARTS[slot]}A'):
                return r
        return templates['ArmorSteelShield'] if slot == 14 else None
    if slot in ACCESSORIES:
        return templates[ACCESSORIES[slot][light_tier(level)]]
    if slot in PARTS:   # cloth, and miscellaneous goggles, robes and boots
        return templates[CLOTH[PARTS[slot] if PARTS[slot] in CLOTH else 'Cuirass'][light_tier(level)]]
    return None


HEAD_SLOTS, CIRCLET, RING = 0b11, 1 << 12, 1 << 6   # biped slots 30 (head) and 31 (hair); 42 (circlet); 36 (ring)


def worn(r, model, addons):
    """The armour addons (ARMA) an NPC wears an item in (her own body is never drawn): its look's, where both claim
    the slot the addon fills (a helm also its circlet look's). A helm, chest or robe, hands, feet, shield, neck or ring is seen; shoulders, waist, legs,
    wrists, back, shirt and tabard take no addon of another slot (a cuirass's, gauntlets') to fight that slot's own
    item over, and stay unseen. addons: Skyrim.esm's addons' biped masks by form."""
    if model.type != 'ARMO':
        return []
    if biped_mask(r) & RING:   # 1.12 draws no ring on a character; addon-less, two are worn at once (Skyrim parts
        return []              # pieces at odds by their addons), as her two finger slots hold them
    mask = biped_mask(r) & biped_mask(model)
    if biped_mask(r) & HEAD_SLOTS and biped_mask(model) & CIRCLET:   # a helm whose look is a circlet wears that circlet
        mask |= CIRCLET
    return [d for d in model.all('MODL') if addons[struct.unpack('<I', d)[0]] & mask]


def dress(r, model, addons):
    """Give the item another record's world model (and its pickup bounds), an armour's or weapon's, or a misc item's;
    and an armour the addons an NPC wears it in (worn)."""
    if model is None:
        return r
    if r.type == 'WEAP':
        subs = [(t, d) for t, d in model.subs if t in ('MODL', 'MODT', 'MODS')]
        names = {'MODL', 'MODT', 'MODS'}
    else:
        subs = [(t, d) for t, d in model.subs if t in MODELS] if model.type == 'ARMO' else \
            [('MOD2', model.get('MODL')), ('MO2T', model.get('MODT')), ('MOD4', model.get('MODL')), ('MO4T', model.get('MODT'))]
        names = set(MODELS)
    at = next((i for i, (t, _) in enumerate(r.subs) if t in names), None)
    r.drop(*names)
    if at is None:
        at = next(i for i, (t, _) in enumerate(r.subs) if t in ('BOD2', 'DATA'))
    r.subs[at:at] = [(t, d) for t, d in subs if d is not None]
    if r.type == 'ARMO':
        r.drop('MODL')
        insert_before(r, 'DATA', [('MODL', d) for d in worn(r, model, addons)])   # where Skyrim's ARMO has them
        if model.type == 'ARMO' and biped_mask(r) & HEAD_SLOTS and biped_mask(model) & (HEAD_SLOTS | CIRCLET) == CIRCLET:
            # A helm whose look is a circlet alone. Worn as Skyrim's circlets are, in slot 42 alone: its addon is drawn only in a slot the armour claims, and
            # claiming the head and hair without an addon there left an NPC headless (seen in game). Gear still takes a
            # Skyrim helmet off her for it (a WoW head item claims all three).
            _, kind = struct.unpack('<II', r.get('BOD2'))
            r.set('BOD2', struct.pack('<II', CIRCLET, kind))
    if model.type == 'WEAP' and model.get('WNAM'):
        r.set('WNAM', model.get('WNAM'))   # its first-person model
    r.set('OBND', model.get('OBND'))
    return r


def book(template, form, entry, name, text, value, weight):
    """A Skyrim book that teaches nothing itself (no teach-spell or skill flag): reading it or using it from her bags
    teaches its WoW spell or mount (skse/src/Learning.cpp). Its text is the item's "Use:" line."""
    r = clone(template, form, f'WoWItem{entry}', name)
    r.drop('CNAM')
    insert_before(r, 'KSIZ', [('DESC', esm.zstring(f'{name}\n\nUse: {text}'))])
    r.set('DATA', struct.pack('<BBHIIf', 0, 0, 0, 0xFFFFFFFF, value, weight))
    return r


def leveled(form, edid, entries, flags, file=1, chance_none=0, chance_global=None):
    """A leveled item list: flags 1 counts every level up to hers, 2 rolls once per count, 4 gives every entry;
    chance_global, a GLOB whose value is its chance none (LVLG)."""
    subs = [('EDID', esm.zstring(edid)), ('OBND', bytes(12)), ('LVLD', bytes([chance_none])), ('LVLF', bytes([flags]))]
    if chance_global is not None:
        subs.append(('LVLG', struct.pack('<I', chance_global)))
    subs.append(('LLCT', bytes([len(entries)])))
    subs += [('LVLO', struct.pack('<HHIHH', level, 0, item, 1, 0)) for level, item in sorted(entries)]
    return esm.Record('LVLI', file << 24 | form, 0, subs)


def learnables(data, templates, forms, records):
    """Her spell books and mount items (data/learnables.json): BOOK records, their gear.json rows with the client's item
    rows as Skyrim has them (a Skyrim level to use them, a book needing its previous rank), and the leveled lists that
    carry them to treasure and vendors."""
    curve = json.loads((REPO / 'data/progression.json').read_text())['curve']
    books, mounts, lists = {}, {}, {key: [] for key in LISTS}
    globals_ = {}
    for cls, (edid, keys) in CLASS_BOOK_LISTS.items():   # a short, read as the list's chance none; the Druid's at 0
        form = forms.get(f'global:{edid}')
        records.append(esm.Record('GLOB', 0x01000000 | form, 0, [('EDID', esm.zstring(edid)), ('FNAM', b's'),
                                                                 ('FLTV', struct.pack('<f', 0.0 if cls == 11 else 100.0))]))
        globals_.update({key: 0x01000000 | form for key in keys})
    paladin = [{**b, 'lists': ('paladin_all', f"paladin_{b['skill']}")} for b in data.get('class_books', {}).get('paladin', [])]
    for b in [{**b, 'lists': ('all', b['skill'])} for b in data['books']] + paladin:
        level = skyrim_level(b['required_level'])
        skill = skill_value(b['required_level'], curve)
        value = next(v for below, v in TOME_VALUES if skill < below)
        form = forms.get(f"book:{b['spell']}")
        records.append(book(templates[TOMES[b['skill']]], form, b['item']['entry'], b['item']['name'], b['text'], value, 1.))
        for key in b['lists']:
            if key in lists:
                lists[key].append((level, 0x01000000 | form))
        # The Use: line is the teaching text (some 1.12 books name the wrong learning spell); the previous rank is the
        # client's own "Requires <spell>" (requiredspell), and the level Skyrim's.
        wow = {**b['item'], 'RequiredLevel': level, 'requiredspell': b['previous_spell'], 'spellid_1': 0}
        books[str(b['item']['entry'])] = {'entry': b['item']['entry'], 'name': b['item']['name'], 'kind': 'BOOK', 'form': form,
                                          'displayid': b['item']['displayid'], 'quality': b['item']['Quality'], 'wow': wow,
                                          'teaches': {'spell': b['spell'], 'previous': b['previous_spell'], 'text': b['text']},
                                          'skyrim': {'level': level, 'value': value}}
    for m in data['mounts']:
        level, value = REINS[m['flying']]
        form = forms.get(f"mount_item:{m['key']}")
        # A folded note (DA14SamNote's Note03.nif), its text the item's 1.12 Use: line (its mount spell's description).
        records.append(book(templates['DA14SamNote'], form, m['item']['entry'], m['item']['name'], m['text'], value, 0.))
        lists['stables'].append((level, 0x01000000 | form))
        lists['caravans'].append((level, 0x01000000 | form))
        # Every mount is hers to ride (a night elf's kodo too) and Skyrim has no Riding skill: no race or skill gate.
        wow = {**m['item'], 'RequiredLevel': level, 'RequiredSkill': 0, 'RequiredSkillRank': 0, 'AllowableRace': -1}
        mounts[str(m['item']['entry'])] = {'entry': m['item']['entry'], 'name': m['item']['name'], 'kind': 'BOOK', 'form': form,
                                           'displayid': m['item']['displayid'], 'quality': m['item']['Quality'], 'wow': wow,
                                           'teaches': {'mount': m['key'], 'spell': m['spell']},
                                           'skyrim': {'level': level, 'value': value}, **({'icon': m['icon']} if m['icon'] else {})}
    distribution = []
    for key, entries in lists.items():
        name, kind, targets = LISTS[key]
        form = forms.get(f'list:{name}')
        # A stable sells every mount she is old enough for; elsewhere each count rolls one item at or below her level.
        records.append(leveled(form, name, entries, 0x01 | (0x04 if key == 'stables' else 0x02), chance_global=globals_.get(key)))
        distribution.append({'list': form, 'name': name, 'lists': [], 'vendors': [],
                             kind: [{'form': f, 'count': n} for f, n in targets]})
    return books, mounts, distribution, {cls: 0x01000000 | forms.get(f'global:{edid}') for cls, (edid, _) in CLASS_BOOK_LISTS.items()}


def read_master(master, types):
    """The master's records of these types by editor ID, and its armour addons' (ARMA) biped masks by form."""
    groups = esm.read(Path(master), set(types) | {'ARMA'})
    addons = {r.form: biped_mask(r) for r in groups.pop('ARMA')}
    return {r.edid: r for rs in groups.values() for r in rs}, addons


def build(catalog, master, spell_dbc, forms_path, output, looks_path=LOOKS):
    templates, addons = read_master(master, {'ARMO', 'WEAP', 'ENCH', 'MGEF', 'SPEL', 'KYWD', 'MISC', 'COBJ', 'PERK', 'BOOK'})
    curated = reviewed_looks(looks_path)
    texts = dbc.by_id(dbc.read(Path(spell_dbc)))
    forms, records, gear = Forms(forms_path), [], {}
    caps, bands = enchantment_caps(templates), variations(catalog['items'])
    for entry, item in sorted(catalog['items'].items(), key=lambda p: int(p[0])):
        # Skyrim gear has no level gate, including exports from older catalogs.
        item = {**item, 'RequiredLevel': 0}
        form = forms.get(f'item:{entry}')
        stats = skyrim_stats(item, templates, bands[entry])
        effects, descriptions = effects_for(item, catalog, texts)
        effects = capped(effects, stats['level'], caps)
        for sid, amount, text in descriptions:
            key = f'description:{sid}'
            local = forms.get(key)
            if not any(r.form == 0x01000000 | local for r in records):
                records.append(equip_effect(templates, local, sid, texts[sid]['Name_lang'], text))
            effects.append((0x01000000 | local, amount))
        # Keep historical enchantment records resolvable in saves, but no new
        # item references them. Their intrinsic effects now use equip abilities.
        legacy = forms.ids.get(f'enchantment:{entry}')
        if legacy:
            records.append(magic(templates['EnchArmorFortifyHealthBase'], legacy,
                                 f'WoWEnchantment{entry}', item['name'], effects))
        is_weapon = item['class'] == 2
        magic_form = None
        if effects or is_weapon:
            kind = 'ability'
            magic_form = forms.get(f'{kind}:{entry}')
            template = templates['AbVampire01b']
            records.append(magic(template, magic_form, f'WoW{kind.title()}{entry}', item['name'], effects))
        record = weapon(item, templates, form, stats) if is_weapon else armor(item, templates, form, stats)
        if not is_weapon:   # its Skyrim look, as the Armory's: on the ground, and on an NPC (a follower) wearing it
            dress(record, look(item, stats, templates, curated), addons)
        records.append(record)
        records.append(temper(item, templates, forms.get(f'temper:{entry}'), record.form))
        # With what dresses her and swings it (the fields character.json gives her worn items), so anything she
        # picks up can be worn.
        gear[entry] = {'entry': int(entry), 'name': item['name'], 'kind': record.type, 'form': form,
                       **{k: int(item.get(k) or 0) for k in ('displayid', 'InventoryType', 'class', 'subclass', 'sheath',
                                                            'Material', 'delay')}, 'wow': item, 'quality': int(item.get('Quality', 1))}
        if item.get('itemset'):
            row = catalog['item_sets'][str(item['itemset'])]
            gear[entry]['set'] = {'id': row['ID'], 'name': row['Name_lang'],
                                  'items': [{'entry': e, 'name': catalog['items'][str(e)]['name']}
                                            for e in row['ItemID'] if e and str(e) in catalog['items']],
                                  'bonuses': [{'spell': s, 'threshold': n}
                                              for s, n in zip(row['SetSpellID'], row['SetThreshold']) if s and n]}
        for n in range(1, 6):
            sid = int(item.get(f'spellid_{n}', 0))
            if sid and item.get(f'spelltrigger_{n}') == 0:
                spell = catalog['spells'][str(sid)]
                from service.equipment import use_cooldown
                use = use_cooldown(item, n, spell)
                if spell['Effect1'] == 34 and str(spell['EffectItemType1']) in catalog['items']:
                    use['entry'] = spell['EffectItemType1']
                if 'use' not in gear[entry]:
                    gear[entry]['use'] = {**use, 'spells': []}
                gear[entry]['use']['spells'].append(use)
                # Character::Item's legacy transformation field is one duration.
                if 'entry' in use:
                    gear[entry]['use']['cooldown_ms'] = max(use['cooldown_ms'], use['category_cooldown_ms'])
        if magic_form:
            gear[entry]['ability'] = magic_form
        material, ingredient, perk = crafting_material(item)
        gear[entry]['crafting'] = {'material': material, 'ingredient': ingredient, 'perk': perk}
        # The tooltips' Skyrim armour and damage where Skyrim has not said its own (wowapi_bags.lua wowTooltip).
        gear[entry]['skyrim'] = {k: v for k, v in stats.items() if k != 'speed'}
    # Native set bonuses have independent save-stable abilities. Their service
    # modifiers still come from the equipped entries, never from these abilities.
    groups = copy.deepcopy(catalog.get('equipment_sets', {}))
    for key, group in groups.items():
        row = catalog['item_sets'][str(group['id'])]
        group['bonuses'] = []
        for sid, threshold in zip(row['SetSpellID'], row['SetThreshold']):
            if not sid or not threshold:
                continue
            effects, _ = effects_for({'entry': 0, 'spellid_1': sid, 'spelltrigger_1': 1}, catalog, texts, describe=False)
            bonus = {'spell': sid, 'threshold': threshold}
            if adaptation := SET_ADAPTATIONS.get(str(sid)):
                effects.extend(adaptation['effects'])
                bonus['adaptation'] = adaptation['reason']
            if effects:
                local = forms.get(f'set_ability:{sid}')
                records.append(magic(templates['AbVampire01b'], local, f'WoWSetAbility{sid}', texts[sid]['Name_lang'], effects))
                bonus['ability'] = local
            group['bonuses'].append(bonus)
    output = Path(output)
    # What her spells consume (Rebirth's Ironwood Seed, Gift of the Wild's Wild Thornroot): misc items carried in her
    # bags, a charcoal lump's look in the world (no Skyrim model is WoW's).
    reagents = {}
    for entry, item in sorted(catalog.get('reagents', {}).items(), key=lambda p: int(p[0])):
        form = forms.get(f'item:{entry}')
        r = clone(templates['Charcoal'], form, f"WoWItem{entry}", item['name'])
        set_keywords(r, [templates['VendorItemClutter'].form])
        r.set('DATA', struct.pack('<If', max(1, int(item.get('SellPrice', 0)) // 100), .1))
        records.append(r)
        reagents[entry] = {'entry': int(entry), 'name': item['name'], 'form': form,
                           'displayid': int(item.get('displayid', 0)), 'quality': int(item.get('Quality', 1)), 'wow': item}
    books, mounts, distribution, book_globals = learnables(json.loads(LEARNABLES.read_text()), templates, forms, records)
    esm.write(output / 'esp/WoWBridge.esp', records, ['Skyrim.esm'], 'WoWBridge')
    (output / 'gear.json').write_text(json.dumps({'plugin': 'WoWBridge.esp', 'items': gear, 'reagents': reagents,
                                                 'sets': groups, 'books': books, 'mounts': mounts,
                                                 'distribution': distribution,
                                                 'class_book_globals': {str(c): f for c, f in book_globals.items()}},
                                                 indent=2, sort_keys=True) + '\n')
    forms.save()
    return records, gear


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--catalog', type=Path, default=REPO / 'build/catalog.json')
    ap.add_argument('--master', type=Path, default=MASTER)
    ap.add_argument('--spell-dbc', type=Path, default=REPO / 'build/dbc/DBFilesClient/Spell.dbc')
    ap.add_argument('--forms', type=Path, default=REPO / 'data/esp_forms.json')
    ap.add_argument('--looks', type=Path, default=LOOKS)
    ap.add_argument('--out', type=Path, default=REPO / 'build')
    args = ap.parse_args()
    records, gear = build(json.loads(args.catalog.read_text()), args.master, args.spell_dbc, args.forms, args.out, args.looks)
    print(f'{args.out}/esp/WoWBridge.esp: {len(gear)} items, {len(records)} records; gear.json written')


if __name__ == '__main__':
    main()
