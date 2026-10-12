#!/usr/bin/env python3
"""Generate inventory gear from the catalog; object IDs are permanent save-game identities.

gear.json uses entry-keyed items: kind, local form ID (resolve against plugin),
WoW entry, name, and a local equip ability ID. Skyrim owns the converted
bonuses; equip-spell effects without an equivalent remain in the WoW service.
"""
import argparse
import copy
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from data import dbc, esm
from service.skyrim_effects import STAT, RESISTS, RESIST_DIVISOR, ARMOR_DIVISORS

REPO = Path(__file__).resolve().parent.parent
SET_ADAPTATIONS = json.loads((REPO / 'data/equipment.json').read_text()).get('set_adaptations', {})
MASTER = Path(__file__).resolve().parents[2] / 'steamapps/common/Skyrim Special Edition/Data/Skyrim.esm'
SLOTS = {1: 30, 2: 35, 3: 57, 4: 56, 5: 32, 6: 49, 7: 53, 8: 37,
         9: 34, 10: 33, 11: 36, 12: 60, 14: 39, 15: 61, 16: 47,
         19: 56, 20: 32, 23: 39, 25: 61, 26: 61, 28: 61}
WEAPONS = {0: 'IronWarAxe', 1: 'IronBattleaxe', 2: 'HuntingBow', 3: 'HuntingBow',
           4: 'IronMace', 5: 'IronWarhammer', 6: 'IronGreatsword', 7: 'IronSword',
           8: 'IronGreatsword', 10: 'IronWarhammer', 13: 'IronMace', 15: 'IronDagger',
           16: 'IronDagger', 18: 'HuntingBow', 19: 'IronMace', 20: 'HuntingBow'}


def u32(n):
    return struct.pack('<I', n)


class Forms:
    def __init__(self, path):
        self.path = Path(path)
        self.ids = json.loads(self.path.read_text()) if self.path.exists() else {}
        values = list(self.ids.values())
        if len(set(values)) != len(values) or any(not 0x800 <= n <= 0xFFF for n in values):
            raise ValueError('esp_forms.json contains duplicate or out-of-range object IDs')

    def get(self, key):
        if key not in self.ids:
            # Never recycle removed entries: old saves may still reference them.
            n = max(self.ids.values(), default=0x7FF) + 1
            if n > 0xFFF:
                raise ValueError('WoWBridge exceeds the 2048-object ESL budget')
            self.ids[key] = n
        return self.ids[key]

    def save(self):
        self.path.write_text(json.dumps(self.ids, indent=2, sort_keys=True) + '\n')


def clone(template, form, edid, name):
    r = copy.deepcopy(template)
    r.form = 0x01000000 | form  # Skyrim.esm is master 0; this plugin is file 1.
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


def effects_for(item, catalog, texts, describe=True):
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


def magic(template, form, edid, name, effects):
    r = clone(template, form, edid, name)
    r.drop('EFID', 'EFIT')
    if r.type == 'ENCH':
        r.set('ENIT', struct.pack('<9I', 0, 1, 0, 0, 0, 6, 0, 0, 0))
    else:
        r.set('SPIT', struct.pack('<3If2If2I', 0, 0, 4, 0, 0, 0, 0, 0, 0))
    for effect, magnitude in effects:
        r.subs.extend([('EFID', u32(effect)), ('EFIT', struct.pack('<fII', magnitude, 0, 0))])
    return r


def crafting_material(item):
    """Keep soft/wooden gear soft; use level-appropriate alloys for metal gear."""
    level = int(item.get('ItemLevel', 0))
    if item['class'] == 2:
        if int(item.get('Material', 0)) != 1:
            return 'Wood', 'LeatherStrips', None
        if level >= 70:
            return 'Ebony', 'IngotEbony', 'EbonySmithing'
        if level >= 60:
            return 'Dwarven', 'IngotDwarven', 'DwarvenSmithing'
        return 'Steel', 'IngotSteel', 'SteelSmithing'
    if item['subclass'] in (3, 4, 6):
        if item['subclass'] == 4 and level >= 60:
            return 'SteelPlate', 'IngotCorundum', 'AdvancedArmors'
        if level >= 70:
            return 'Orcish', 'IngotOrichalcum', 'OrcishSmithing'
        return 'Steel', 'IngotSteel', 'SteelSmithing'
    return 'Leather', 'Leather01', None


def temper(item, templates, form, created):
    material, ingredient, perk = crafting_material(item)
    # Vanilla's pair of OR conditions: unenchanted OR Arcane Blacksmith.
    template = templates['TemperWeaponIronSword' if item['class'] == 2 else 'TemperArmorLeatherCuirass']
    r = copy.deepcopy(template)
    r.form, r.flags = 0x01000000 | form, 0
    r.set('EDID', esm.zstring(f"WoWTemper{item['entry']}"))
    r.set('CNTO', struct.pack('<II', templates[ingredient].form, 1))
    r.set('CNAM', u32(created))
    if perk:
        # HasPerk == 1, run on Subject, no OR flag; material perks also double
        # improvement through their vanilla entry points and matching keywords.
        condition = struct.pack('<B3xfH2xIIIII', 0, 1., 448, templates[perk].form, 0, 0, 0, 0xFFFFFFFF)
        insert_before(r, 'CNAM', [('CTDA', condition)])
    return r


def armor(item, templates, form):
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
    r = clone(templates[edid], form, f"WoWItem{item['entry']}", item['name'])
    r.drop('MODL', 'BODT')
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
          templates['ArmorMaterial' + crafting_material(item)[0]].form]
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
    r.set('DATA', struct.pack('<If', max(1, int(item.get('SellPrice', 0)) // 40), weight))
    rating = round(float(item.get('armor', 0)) / ARMOR_DIVISORS[category])
    r.set('DNAM', u32(rating * 100))
    return r


def weapon(item, templates, form):
    subclass = int(item['subclass'])
    if subclass not in WEAPONS:
        raise ValueError(f"item {item['entry']}: unsupported weapon subclass {subclass}")
    r = clone(templates[WEAPONS[subclass]], form, f"WoWItem{item['entry']}", item['name'])
    # Equip bonuses live in an ability, leaving the native enchantment slot free.
    keywords = list(struct.unpack('<' + 'I' * (len(r.get('KWDA')) // 4), r.get('KWDA')))
    keywords = [k for k in keywords if not any(k == t.form and t.edid.startswith('WeapMaterial') for t in templates.values())]
    set_keywords(r, keywords + [templates['WeapMaterial' + crafting_material(item)[0]].form])
    if subclass == 10:
        r.set('MODL', esm.zstring(r'Weapons\Staff01\Staff01.nif'))
        r.drop('MODT')
    damage = round((float(item.get('dmg_min1', 0)) + float(item.get('dmg_max1', 0))) / 2 * .25)
    r.set('DATA', struct.pack('<IfH', max(1, int(item.get('SellPrice', 0)) // 40),
                              struct.unpack_from('<f', r.get('DATA'), 4)[0], max(0, min(65535, damage))))
    dnam = bytearray(r.get('DNAM'))
    delay = float(item.get('delay', 0))
    struct.pack_into('<f', dnam, 4, 2.6 / (delay / 1000) if delay > 0 else 1)
    r.set('DNAM', bytes(dnam))
    # Do not retain the template's unrelated critical hit damage.
    crit = bytearray(r.get('CRDT'))
    struct.pack_into('<H', crit, 0, round(damage / 2))
    r.set('CRDT', bytes(crit))
    return r


def build(catalog, master, spell_dbc, forms_path, output):
    templates = {r.edid: r for rs in esm.read(Path(master), {'ARMO', 'WEAP', 'ENCH', 'MGEF', 'SPEL', 'KYWD', 'MISC', 'COBJ', 'PERK'}).values() for r in rs}
    texts = dbc.by_id(dbc.read(Path(spell_dbc)))
    forms, records, gear = Forms(forms_path), [], {}
    for entry, item in sorted(catalog['items'].items(), key=lambda p: int(p[0])):
        # Skyrim gear has no level gate, including exports from older catalogs.
        item = {**item, 'RequiredLevel': 0}
        form = forms.get(f'item:{entry}')
        effects, descriptions = effects_for(item, catalog, texts)
        for sid, amount, text in descriptions:
            key = f'description:{sid}'
            local = forms.get(key)
            if not any(r.form == 0x01000000 | local for r in records):
                r = clone(templates['EnchFortifyHealthConstantSelf'], local, f'WoWEquipEffect{sid}', texts[sid]['Name_lang'])
                r.set('DNAM', esm.zstring(text))
                data = bytearray(r.get('DATA'))
                struct.pack_into('<i', data, 12, -1)
                struct.pack_into('<I', data, 72, 77)
                r.set('DATA', bytes(data))
                records.append(r)
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
        record = weapon(item, templates, form) if is_weapon else armor(item, templates, form)
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
                if spell['Effect1'] != 34 or str(spell['EffectItemType1']) not in catalog['items']:
                    raise ValueError(f'item {entry}: unsupported use spell {sid}')
                gear[entry]['use'] = {'spell': sid, 'entry': spell['EffectItemType1'],
                                      'cooldown_ms': max(0, item[f'spellcooldown_{n}'], item[f'spellcategorycooldown_{n}'])}
        if magic_form:
            gear[entry]['ability'] = magic_form
        material, ingredient, perk = crafting_material(item)
        gear[entry]['crafting'] = {'material': material, 'ingredient': ingredient, 'perk': perk}
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
        r.set('DATA', struct.pack('<If', max(1, int(item.get('SellPrice', 0)) // 40), .1))
        records.append(r)
        reagents[entry] = {'entry': int(entry), 'name': item['name'], 'form': form,
                           'displayid': int(item.get('displayid', 0)), 'quality': int(item.get('Quality', 1)), 'wow': item}
    esm.write(output / 'esp/WoWBridge.esp', records, ['Skyrim.esm'], 'WoWBridge')
    (output / 'gear.json').write_text(json.dumps({'plugin': 'WoWBridge.esp', 'items': gear, 'reagents': reagents,
                                                 'sets': groups},
                                                 indent=2, sort_keys=True) + '\n')
    forms.save()
    return records, gear


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--catalog', type=Path, default=REPO / 'build/catalog.json')
    ap.add_argument('--master', type=Path, default=MASTER)
    ap.add_argument('--spell-dbc', type=Path, default=REPO / 'build/dbc/DBFilesClient/Spell.dbc')
    ap.add_argument('--forms', type=Path, default=REPO / 'data/esp_forms.json')
    ap.add_argument('--out', type=Path, default=REPO / 'build')
    args = ap.parse_args()
    records, gear = build(json.loads(args.catalog.read_text()), args.master, args.spell_dbc, args.forms, args.out)
    print(f'{args.out}/esp/WoWBridge.esp: {len(gear)} items, {len(records)} records; gear.json written')


if __name__ == '__main__':
    main()
