#!/usr/bin/env python3
"""Inventory installed Skyrim magic, resolving localized names and master-relative forms.

Includes a conservative superset: NPC/quest/test records are retained and labelled,
since an effect on the player need not originate from a player-learnable spell.
Only unused MGEFs, deleted records, and effectless consumables are omitted.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from data import esm

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DATA = Path(__file__).resolve().parents[2] / 'steamapps/common/Skyrim Special Edition/Data'
TYPES = {'SPEL', 'SHOU', 'ALCH', 'INGR', 'SCRL', 'ENCH', 'MGEF'}
SCHOOLS = {18: 'Alteration', 19: 'Conjuration', 20: 'Destruction', 21: 'Illusion', 22: 'Restoration'}
ACTOR_VALUES = dict(enumerate(('Aggression Confidence Energy Morality Mood Assistance OneHanded TwoHanded Archery '
    'Block Smithing HeavyArmor LightArmor Pickpocket Lockpicking Sneak Alchemy Speech Alteration Conjuration '
    'Destruction Illusion Restoration Enchanting Health Magicka Stamina HealRate MagickaRate StaminaRate '
    'SpeedMult InventoryWeight CarryWeight CriticalChance MeleeDamage UnarmedDamage Mass VoicePoints VoiceRate '
    'DamageResist PoisonResist ResistFire ResistShock ResistFrost ResistMagic ResistDisease').split()))
ACTOR_VALUES.update({53: 'Paralysis', 54: 'Invisibility', 55: 'NightEye', 56: 'DetectLifeRange', 57: 'WaterBreathing',
    58: 'WaterWalking', 63: 'WardPower', 67: 'WardDeflection', 83: 'AbsorbChance', 86: 'ShoutRecoveryMult',
    155: 'HealRateMult', 156: 'MagickaRateMult', 157: 'StaminaRateMult', -1: 'None'})
for offset, names in [(46, 'PerceptionCondition EnduranceCondition LeftAttackCondition RightAttackCondition '
                      'LeftMobilityCondition RightMobilityCondition BrainCondition'),
                      (59, 'IgnoreCrippledLimbs Fame Infamy JumpingBonus WardPower RightItemCharge ArmorPerks ShieldPerks WardDeflection'),
                      (78, 'BowSpeedBonus FavorActive FavorsPerDay FavorsPerDayTimer LeftItemCharge AbsorbChance Blindness '
                       'WeaponSpeedMult ShoutRecoveryMult BowStaggerBonus Telekinesis FavorPointsBonus LastBribedIntimidated '
                       'LastFlattered MovementNoiseMult BypassVendorStolenCheck BypassVendorKeywordCheck WaitingForPlayer'),
                      (132, 'LeftWeaponSpeedMultiply DragonSouls CombatHealthRegenMultiply'),
                      (153, 'DragonRend AttackDamageMult HealRateMult MagickaRateMult StaminaRateMult'),
                      (158, 'WerewolfPerks VampirePerks GrabActorOffset Grabbed Deprecated05 ReflectDamage')]:
    ACTOR_VALUES.update({offset + i: name for i, name in enumerate(names.split())})
ACTOR_VALUES.update({68 + i: f'Variable{i + 1:02}' for i in range(10)})
SKILLS = ('OneHanded TwoHanded Marksman Block Smithing HeavyArmor LightArmor Pickpocket Lockpicking Sneaking '
          'Alchemy Speechcraft Alteration Conjuration Destruction Illusion Restoration Enchanting').split()
for offset, suffix in [(96, 'Modifier'), (114, 'SkillAdvance'), (135, 'PowerModifier')]:
    ACTOR_VALUES.update({offset + i: name + suffix for i, name in enumerate(SKILLS)})
ARCHETYPES = ('ValueModifier Script Dispel CureDisease Absorb DualValueModifier Calm Demoralize Frenzy Disarm '
    'CommandSummoned Invisibility Light Darkness NightEye Lock Open BoundWeapon SummonCreature DetectLife '
    'Telekinesis Paralysis Reanimate SoulTrap TurnUndead Guide WerewolfFeed CureParalysis CureAddiction CurePoison '
    'Concussion ValueAndParts AccumulateMagnitude Stagger PeakValueModifier Cloak Werewolf SlowTime Rally '
    'EnhanceWeapon SpawnHazard Etherealize Banish SpawnScriptedRef Disguise GrabActor VampireLord').split()
SPELL_TYPES = {0: 'Spell', 1: 'Disease', 2: 'Power', 3: 'LesserPower', 4: 'Ability', 5: 'Poison',
               6: 'Enchantment', 7: 'Potion', 8: 'Wortcraft', 10: 'Voice', 11: 'StaffEnchantment', 12: 'Scroll'}
TIERS = {0: 'novice', 25: 'apprentice', 50: 'adept', 75: 'expert', 100: 'master'}
COST_SETTINGS = {'fMagicCasterPCSkillCostBase', 'fMagicCasterPCSkillCostMult', 'fMagicPCSkillCostScale'}


def integer(data, offset=0, signed=False, default=0):
    return struct.unpack_from('<i' if signed else '<I', data, offset)[0] if data and len(data) >= offset + 4 else default


class Bsa:
    """Index only the directory of SSE (105) / Skyrim (104) BSAs; read requested files lazily."""
    def __init__(self, path):
        self.path, self.files = path, {}
        with path.open('rb') as f:
            magic, self.version, offset, self.flags, folders, count, _, name_size, _ = struct.unpack('<4s8I', f.read(36))
            if magic != b'BSA\0' or self.version not in (104, 105) or self.flags & 3 != 3:
                raise ValueError(f'unsupported BSA directory: {path}')
            f.seek(offset)
            counts = []
            for _ in range(folders):
                row = f.read(24 if self.version == 105 else 16)
                counts.append(integer(row, 8))
            entries = []
            for n in counts:
                folder = f.read(f.read(1)[0]).rstrip(b'\0').decode('utf-8')
                for _ in range(n):
                    _, size, at = struct.unpack('<QII', f.read(16))
                    entries.append((folder, size, at))
            names = f.read(name_size).rstrip(b'\0').split(b'\0')
            if len(entries) != count or len(names) != count:
                raise ValueError(f'invalid BSA directory: {path}')
            for (folder, size, at), name in zip(entries, names):
                key = (folder + '/' + name.decode('utf-8')).replace('\\', '/').casefold()
                self.files[key] = (size, at)

    def read(self, name):
        size, at = self.files[name.casefold()]
        compressed = bool(self.flags & 4) != bool(size & 0x40000000)
        with self.path.open('rb') as f:
            f.seek(at)
            raw = f.read(size & 0x3fffffff)
        if self.flags & 0x100:
            raw = raw[1 + raw[0]:]
        if compressed:
            expected, raw = integer(raw), raw[4:]
            if self.version == 105:
                import lz4.frame
                raw = lz4.frame.decompress(raw)
            else:
                raw = zlib.decompress(raw)
            if len(raw) != expected:
                raise ValueError(f'bad uncompressed size: {self.path}:{name}')
        return raw


class Strings:
    def __init__(self, data, language):
        self.data, self.language, self.cache = data, language, {}
        # Reading directories is cheap; do not load entire texture archives.
        self.archives = [Bsa(p) for p in sorted(data.glob('*.bsa'))
                         if p.name == 'Skyrim - Interface.bsa' or p.stem.casefold().startswith('cc')]

    def table(self, plugin, extension):
        key = (plugin, extension)
        if key not in self.cache:
            name = f'Strings/{Path(plugin).stem}_{self.language}.{extension}'.casefold()
            loose = next((p for p in (self.data / 'Strings').glob('*') if f'Strings/{p.name}'.casefold() == name), None)
            raw = loose.read_bytes() if loose else next((b.read(name) for b in reversed(self.archives) if name in b.files), None)
            table = {}
            if raw is not None:
                count, size = struct.unpack_from('<II', raw)
                base = 8 + count * 8
                if base + size > len(raw):
                    raise ValueError(f'truncated {name}')
                for i in range(count):
                    sid, off = struct.unpack_from('<II', raw, 8 + i * 8)
                    at = base + off
                    if extension == 'strings':
                        end = raw.index(b'\0', at)
                        text = raw[at:end]
                    else:
                        length = integer(raw, at)
                        text = raw[at + 4:at + 4 + length].rstrip(b'\0')
                    table[sid] = text.decode('utf-8', 'replace')
            self.cache[key] = table
        return self.cache[key]

    def text(self, record, sub, plugin, localized):
        raw = record.get(sub)
        if not raw:
            return ''
        if not localized:
            return raw.rstrip(b'\0').decode('utf-8', 'replace')
        sid = integer(raw)
        if not sid:
            return ''
        extension = 'strings' if sub in ('FULL', 'DNAM') else 'dlstrings'
        table = self.table(plugin, extension)
        if sid not in table:
            raise ValueError(f'missing {self.language} {extension} string {sid:08X} for {plugin}:{record.edid}:{sub}')
        return table[sid]


def identity(plugin, local):
    return f'{plugin}:{local:06X}'


def plugin_header(path):
    with path.open('rb') as f:
        header = f.read(24)
        subs = esm._subrecords(f.read(integer(header, 4)))
    return integer(header, 8), [d.rstrip(b'\0').decode('utf-8') for t, d in subs if t == 'MAST']


def inventory(data, language='english'):
    available = {p.name.casefold(): p for p in data.iterdir() if p.suffix.casefold() in ('.esm', '.esl')}
    requested = ['Skyrim.esm', 'Update.esm', 'Dawnguard.esm', 'HearthFires.esm', 'Dragonborn.esm']
    requested += sorted(p.name for p in available.values() if p.name.casefold().startswith('cc'))
    strings, records, headers, order, settings = Strings(data, language), {}, {}, [], {}

    def load(name):
        path = available.get(name.casefold())
        if not path:
            raise FileNotFoundError(f'missing master {name} in {data}')
        if path.name in headers:
            return
        flags, masters = plugin_header(path)
        for master in masters:
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

        # WOOP is included solely for shout word names, never counted as an icon item.
        for group in esm.read(path, TYPES | {'WOOP', 'GMST'}).values():
            for r in group:
                if r.type == 'GMST':
                    if r.edid in COST_SETTINGS:
                        settings[r.edid] = {'value': struct.unpack('<f', r.get('DATA'))[0], 'source_plugin': path.name}
                    continue
                key = resolve(r.form)
                if r.flags & 0x20:
                    records.pop(key, None)
                    continue
                if r.flags & 0x1000:  # ignored overrides do not replace the previous record
                    continue
                owner, local = key.rsplit(':', 1)
                effects, current = [], None
                for tag, raw in r.subs:
                    if tag == 'EFID':
                        current = {'form': resolve(integer(raw)), 'magnitude': 0, 'area': 0, 'duration': 0, 'conditions': []}
                        effects.append(current)
                    elif tag == 'EFIT' and current:
                        current.update(zip(('magnitude', 'area', 'duration'), struct.unpack('<fII', raw)))
                    elif tag == 'CTDA' and current:
                        current['conditions'].append(raw.hex())
                row = {'key': key, 'plugin': owner, 'local_form_id': int(local, 16), 'form_id_hex': local,
                       'source_plugin': path.name, 'record_type': r.type, 'edid': r.edid,
                       'name': strings.text(r, 'FULL', path.name, flags & 0x80), 'effects': effects}
                if r.type == 'MGEF':
                    raw = r.get('DATA')
                    if not raw or len(raw) < 96:
                        raise ValueError(f'missing MGEF DATA: {key}')
                    # Disk DATA has 32-bit form references, unlike CommonLib's 64-bit pointers.
                    effect_flags, school, resist = integer(raw), integer(raw, 12, True), integer(raw, 16, True)
                    archetype, av, secondary = integer(raw, 64, True), integer(raw, 68, True), integer(raw, 88, True)
                    row.update(type='MagicEffect', flags=effect_flags, hostile=bool(effect_flags & 1), detrimental=bool(effect_flags & 4),
                               hidden_in_ui=bool(effect_flags & 0x8000), school=SCHOOLS.get(school, 'None'), school_id=school,
                               actor_value=ACTOR_VALUES.get(av, f'ActorValue{av}'), actor_value_id=av,
                               secondary_actor_value_id=secondary, resist_value=ACTOR_VALUES.get(resist, f'ActorValue{resist}'),
                               resist_value_id=resist, archetype=ARCHETYPES[archetype] if 0 <= archetype < len(ARCHETYPES) else str(archetype),
                               archetype_id=archetype, minimum_skill=integer(raw, 40, True),
                               base_cost=struct.unpack_from('<f', raw, 4)[0],
                               description=strings.text(r, 'DNAM', path.name, flags & 0x80),
                               associated_form=resolve(integer(raw, 8)), equip_ability=resolve(integer(raw, 128)))
                elif r.type in ('SPEL', 'SCRL'):
                    raw = r.get('SPIT')
                    row.update(type=SPELL_TYPES.get(integer(raw, 8), str(integer(raw, 8))),
                               casting_type=integer(raw, 16), delivery=integer(raw, 20), base_cost=integer(raw),
                               spell_flags=integer(raw, 4), manual_cost=bool(integer(raw, 4) & 1),
                               half_cost_perk=resolve(integer(raw, 32)))
                elif r.type == 'ALCH':
                    alch_flags = integer(r.get('ENIT'), 4)
                    row['type'] = 'Food' if alch_flags & 2 else 'Poison' if alch_flags & 0x20000 else 'Potion'
                elif r.type == 'SHOU':
                    row['words'] = [{'word': resolve(integer(v)), 'spell': resolve(integer(v, 4)),
                                     'recovery': struct.unpack_from('<f', v, 8)[0]} for v in r.all('SNAM')]
                    row['type'] = 'Shout'
                else:
                    row['type'] = {'INGR': 'Ingredient', 'ENCH': 'Enchantment', 'WOOP': 'Word'}.get(r.type, r.type)
                records[key] = row

    for name in requested:
        load(name)
    selected = {k: r for k, r in records.items() if r['record_type'] in TYPES - {'MGEF'}
                and (r['record_type'] in ('SPEL', 'SHOU') or r['effects'])}
    for row in list(selected.values()):
        if row['record_type'] == 'SHOU':
            for word in row['words']:
                word['name'] = records.get(word['word'], {}).get('name', '')
                if word['spell'] is None:  # some NPC shouts leave higher word slots empty
                    continue
                if word['spell'] not in records:
                    raise ValueError(f"unresolved shout spell {word['spell']} in {row['key']}")
                row['effects'].extend(dict(e, shout_spell=word['spell']) for e in records[word['spell']]['effects'])
    used = {e['form'] for r in selected.values() for e in r['effects']}
    for key in used:
        if key not in records or records[key]['record_type'] != 'MGEF':
            raise ValueError(f'unresolved MGEF {key}')
        selected[key] = records[key]
    for row in selected.values():
        visible = [selected[e['form']] for e in row['effects'] if not selected[e['form']]['hidden_in_ui']]
        row['display_name'] = row['name'] or next((e['name'] for e in visible if e['name']), row['edid'] or row['key'])
        row['visible_effect'] = bool(visible) if row['record_type'] != 'MGEF' else not row['hidden_in_ui']
        if row['record_type'] in ('SPEL', 'SCRL'):
            main = max(visible, key=lambda e: e['minimum_skill'], default=None)
            row['minimum_skill'] = main['minimum_skill'] if main else 0
            row['school'] = main['school'] if main else 'None'
            row['tier'] = TIERS.get(row['minimum_skill'], 'nonstandard')
    rows = sorted(selected.values(), key=lambda r: (r['plugin'].casefold(), r['local_form_id']))
    return {'schema_version': 2, 'language': language, 'plugins': order, 'cost_settings': settings,
            'scope': 'All surviving magic items (including NPC/quest/test records) and their referenced MGEFs; '
                     'effectless consumables and unused MGEFs omitted. Final overrides resolved in listed plugin order.',
            'counts': dict(sorted(Counter(r['record_type'] for r in rows).items())), 'forms': rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=DEFAULT_DATA)
    parser.add_argument('--language', default='english')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/skyrim_icons/inventory.json')
    args = parser.parse_args()
    result = inventory(args.data, args.language)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + '\n')
    print(json.dumps(result['counts'], sort_keys=True), f"total={len(result['forms'])}")


if __name__ == '__main__':
    main()
