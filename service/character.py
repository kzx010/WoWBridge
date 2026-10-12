"""The WoW character the service computes for: base stats from CMaNGOS player_levelstats/classlevelstats,
equipped items (stats + on-equip spells) and item-set bonuses, reduced to the aura totals the formulas need."""
from dataclasses import dataclass, field

from service import formulas as F

# SpellAuraDefines.h
A_PERIODIC_DAMAGE, A_PERIODIC_HEAL, A_MOD_DAMAGE_DONE, A_MOD_ROOT, A_MOD_STAT = 3, 8, 13, 26, 29
A_MOD_SPELL_HIT, A_MOD_SPELL_CRIT, A_MOD_SPELL_CRIT_SCHOOL, A_MOD_POWER_REGEN = 55, 57, 71, 85
A_ADD_FLAT_MOD, A_ADD_PCT_MOD, A_OVERRIDE_CLASS_SCRIPTS, A_MOD_MANA_REGEN_INTERRUPT, A_MOD_HEALING_DONE = 107, 108, 112, 134, 135
A_MOD_CRIT_PERCENT, A_MOD_ATTACK_POWER = 52, 99
INVTYPE_WEAPONS = (13, 17, 21)   # one-hand, two-hand, main hand
# item_template stat_type (ItemPrototype.h)
STAT_TYPES = {0: 'mana', 1: 'health', 3: 'agi', 4: 'str', 5: 'int', 6: 'spi', 7: 'sta'}
APPLY_AURA = 6


@dataclass
class Character:
    level: int
    cls: int
    stats: dict                   # str agi sta int spi
    base_mana: int
    item_mana: int = 0
    auras: list = field(default_factory=list)   # {aura, amount, misc, spell, eff}
    mods: list = field(default_factory=list)    # spell modifiers for formulas.apply_spell_mod
    items: list = field(default_factory=list)
    custom_rules: bool = True     # labelled WoWBridge rules for content CMaNGOS leaves unimplemented (oracle turns off)
    # The main-hand weapon (item_template dmg_min1/dmg_max1/delay); bare hands as Player::InitStats sets them.
    weapon: dict = field(default_factory=lambda: {'min': 1.0, 'max': 2.0, 'delay': 2000})
    weapon_entry: int = 0
    weapon_skill: str = 'OneHanded'
    weapon_multiplier: float = 1.0
    magicka_regen_percent: float = 0.0
    native_health_bonus: float = 0.0
    equipment_modifiers: dict = field(default_factory=dict)
    form: str = 'none'
    race: int = 4
    item_resist: dict = field(default_factory=dict)
    talents: dict = field(default_factory=dict)
    talent_spells: set = field(default_factory=set)
    indoors: bool = False
    talent_mode: bool = False

    def multiplier(self, aura, mask=None):
        if not self.talent_mode:
            return 1.0
        value = 1.0
        for a in self.auras:
            if a['aura'] == aura and (mask is None or a['misc'] & mask):
                value *= 1 + a['amount'] / 100.0
        return value

    def total(self, aura: int, misc_mask: int | None = None) -> int:
        return sum(a['amount'] for a in self.auras if a['aura'] == aura and (misc_mask is None or a['misc'] & misc_mask))

    @property
    def max_mana(self) -> int:
        """Unit::UpdateMaxPower (StatSystem.cpp:191)."""
        return round(self.base_mana + self.item_mana + F.mana_from_intellect(self.stats['int']))

    def spell_damage(self, school_mask: int) -> int:
        return self.total(A_MOD_DAMAGE_DONE, school_mask)       # Unit::SpellBaseDamageBonusDone

    def healing_done(self, school_mask: int) -> int:
        return self.total(A_MOD_HEALING_DONE, school_mask)      # Unit::SpellBaseHealingBonusDone

    def spell_crit(self, school: int) -> float:
        """Player::UpdateSpellCritChance (StatSystem.cpp:505)."""
        return (F.spell_crit_from_int(self.cls, self.level, self.stats['int']) + self.total(A_MOD_SPELL_CRIT)
                + self.total(A_MOD_SPELL_CRIT_SCHOOL, 1 << school))

    def attack_power(self) -> float:
        """Player::UpdateAttackPowerAndDamage (StatSystem.cpp), a druid out of form: level x 3 + strength x 2 - 20."""
        return ((self.level * 3.0 + self.stats['str'] * 2.0 - 20.0 + self.total(A_MOD_ATTACK_POWER)
                 + (self.stats['agi'] if self.form == 'cat' else 0)) * self.multiplier(124))

    def melee_weapon(self) -> dict:
        """StatSystem.cpp:Player::CalculateMinMaxDamage; Player.cpp:InitDataForForm.

        Travel/Aquatic do not override attack speed and retain equipped damage.
        """
        if self.form not in ('cat', 'bear', 'dire_bear'):
            return self.weapon
        speed = 1.0 if self.form == 'cat' else 2.5
        return {'min': min(self.level, 60) * .85 * speed,
                'max': min(self.level, 60) * 1.25 * speed, 'delay': int(speed * 1000)}

    def weapon_damage_range(self) -> tuple[float, float]:
        w = self.melee_weapon()
        bonus = self.attack_power() / 14 * w['delay'] / 1000 + self.total(A_MOD_DAMAGE_DONE, 1)
        multiplier = self.multiplier(79, 1) * (self.weapon_multiplier if self.form not in ('cat', 'bear', 'dire_bear') else 1)
        return (w['min'] + bonus) * multiplier, (w['max'] + bonus) * multiplier

    def melee_crit(self) -> float:
        """Player::UpdateAllCritPercentages: agility's share plus SPELL_AURA_MOD_CRIT_PERCENT."""
        return F.melee_crit_from_agility(self.cls, self.level, self.stats['agi']) + self.total(A_MOD_CRIT_PERCENT)

    def spell_hit(self) -> float:
        return float(self.total(A_MOD_SPELL_HIT))

    def resistance(self, school: int) -> int:
        """Flat school resistance plus the strongest exclusive aura (StatSystem.cpp)."""
        mask = 1 << school
        exclusive = max((a['amount'] for a in self.auras if a['aura'] == 143 and a['misc'] & mask), default=0)
        return max(0, self.item_resist.get(school, 0) + self.total(22, mask) + exclusive)

    def regen(self) -> dict:
        """Player::UpdateManaRegen (StatSystem.cpp:547): mana per second normally and inside the 5-second rule."""
        spirit = F.mana_regen_per_second(self.cls, self.stats['spi']) * max(0, 1 + self.magicka_regen_percent / 100)
        # Player::UpdateManaRegen (StatSystem.cpp:552): Innervate multiplies spirit regen,
        # not flat mp5; its other aura restores spirit regen inside the five-second rule.
        for a in self.auras:
            if a['aura'] == 110 and a['misc'] == 0:
                spirit *= 1 + a['amount'] / 100.0
        mp5 = self.total(A_MOD_POWER_REGEN) / 5.0
        interrupt = min(self.total(A_MOD_MANA_REGEN_INTERRUPT), 100)
        return {'per_sec': mp5 + spirit, 'per_sec_casting': mp5 + spirit * interrupt / 100.0, 'tick_ms': 2000, 'fsr_ms': 5000}

    def class_scripts(self, script_id: int) -> list:
        return [a for a in self.auras if a['aura'] == A_OVERRIDE_CLASS_SCRIPTS and a['misc'] == script_id]


def _apply_spell(ch: Character, cat, spell_id: int) -> None:
    s = cat.spell(spell_id)
    for eff in (1, 2, 3):
        if s[f'Effect{eff}'] != APPLY_AURA and not (ch.talent_mode and s[f'Effect{eff}'] == 35):
            continue
        aura = s[f'EffectApplyAuraName{eff}']
        amount = int(F.effect_value(s, eff, ch.level, None, maximum=True,
                                    mods=ch.mods if ch.talent_mode else None))
        misc = s[f'EffectMiscValue{eff}']
        ch.auras.append({'aura': aura, 'amount': amount, 'misc': misc, 'spell': spell_id, 'eff': eff})
        if aura in (A_ADD_FLAT_MOD, A_ADD_PCT_MOD):
            # SpellModifier ctor (Player.cpp:340): mask from spell_affect else EffectItemType (SpellMgr.h:1951)
            ch.mods.append({'op': misc, 'type': aura, 'value': amount, 'family': s['SpellFamilyName'],
                            'mask': cat.affect_mask(spell_id, eff - 1), 'spell': spell_id,
                            'class_driven': ch.talent_mode})
        if aura == A_MOD_STAT:
            for k, stat in {0: 'str', 1: 'agi', 2: 'sta', 3: 'int', 4: 'spi'}.items():
                if misc in (k, -1):
                    ch.stats[stat] += amount


def build(cat, items: list[int], level: int = 60, cls: int = 11, auras: list[int] = (), form: str = 'none', race: int | None = None,
          talents: dict | None = None, indoors: bool = False, equipment_modifiers: dict | None = None) -> Character:
    c = cat.raw['character']
    ls = c.get('levelstats_by_level', {}).get(str(level), c['levelstats'])
    cs = c.get('classlevelstats_by_level', {}).get(str(level), c['classlevelstats'])
    ch = Character(level=level, cls=cls, race=c.get('race', 4) if race is None else race, base_mana=cs['basemana'],
                   stats={'str': ls['str'], 'agi': ls['agi'], 'sta': ls['sta'], 'int': ls['inte'], 'spi': ls['spi']})
    from service import feral
    ch.form = feral.form_name({'form': form, **({'talents': talents} if talents is not None else {})})
    ch.indoors = indoors
    ch.talent_mode = talents is not None
    if talents:
        from service.talents import book
        _, definition = book().class_definition(cls)
        ch.talents = book().allocation(definition, talents, level)
        ch.talent_spells = book().learned_spells(definition, ch.talents)
    if ch.form == 'moonkin' and 24858 not in ch.talent_spells:
        raise ValueError('caster.form moonkin requires its learned talent')
    equipped = [cat.item(i) for i in items if cat.item(i)]
    ch.items = [it['entry'] for it in equipped]
    for it in equipped:
        selected = equipment_modifiers and 'weapon_entry' in equipment_modifiers
        if (selected and it['class'] == 2 and equipment_modifiers['weapon_entry'] == it['entry']) or (
                not selected and it['InventoryType'] in INVTYPE_WEAPONS):
            ch.weapon = {'min': float(it['dmg_min1']), 'max': float(it['dmg_max1']), 'delay': int(it['delay'])}
            ch.weapon_entry = it['entry']
            ch.weapon_skill = ('TwoHanded' if it['InventoryType'] == 17 else 'OneHanded')
    for it in equipped:
        for school, name in ((1, 'holy'), (2, 'fire'), (3, 'nature'), (4, 'frost'), (5, 'shadow'), (6, 'arcane')):
            ch.item_resist[school] = ch.item_resist.get(school, 0) + it.get(name + '_res', 0)
        for k in range(1, 11):
            st, val = it[f'stat_type{k}'], it[f'stat_value{k}']
            name = STAT_TYPES.get(st)
            if not val or not name:
                continue
            if name == 'mana':
                ch.item_mana += val
            elif name in ch.stats:
                ch.stats[name] += val
        for k in range(1, 6):
            if it[f'spellid_{k}'] and it[f'spelltrigger_{k}'] == 1:      # ITEM_SPELLTRIGGER_ON_EQUIP
                _apply_spell(ch, cat, it[f'spellid_{k}'])
    for s in cat.raw['item_sets'].values():                              # Player::AddItemsSetItem
        count = sum(1 for it in equipped if it['entry'] in s['ItemID'])
        for spell_id, threshold in zip(s['SetSpellID'], s['SetThreshold']):
            if spell_id and threshold and count >= threshold:
                _apply_spell(ch, cat, spell_id)
    from service.racials import PASSIVES
    racial = set(PASSIVES) if ch.race == 4 else set()
    passive = {sid for sid in ch.talent_spells if cat.spell(sid)['Attributes'] & 0x40}
    form_mask = feral.form_mask(ch.form)
    for sid in sorted(passive):
        s = cat.spell(sid)
        if s['SpellName'] == 'Feline Swiftness':
            if ch.form != 'cat' or indoors:
                continue
        elif s['Stances'] and not s['Stances'] & form_mask:
            continue
        _apply_spell(ch, cat, sid)
    for spell_id in sorted((set(auras) | (racial & cat.spells.keys())) - passive):
        _apply_spell(ch, cat, spell_id)
    # Linked form passives are not EffectTriggerSpell rows in the catalog.
    # SpellAuras.cpp:Aura::HandleShapeshiftBoosts applies them exactly once.
    for spell, eff, amount in feral.boosts(cat, ch, form):
        ch.auras.append({'aura': spell[f'EffectApplyAuraName{eff}'], 'amount': amount,
                         'misc': spell[f'EffectMiscValue{eff}'], 'spell': spell['Id'], 'eff': eff})
    # CMaNGOS's code-linked form talent auras.
    for sid in ch.talent_spells:
        s = cat.spell(sid)
        if s['SpellIconID'] == 240 and s['EffectApplyAuraName1'] == 137 and ch.form in ('cat', 'bear', 'dire_bear'):
            amount = int(F.effect_value(s, 1, ch.level, None, maximum=True))
            linked = 24900 if ch.form == 'cat' else 24899
            ch.auras.append({'aura': 137, 'amount': amount, 'misc': 0 if ch.form == 'cat' else 2,
                             'spell': linked, 'eff': 1})
    if 17007 in ch.talent_spells and ch.form in ('cat', 'bear', 'dire_bear'):
        _apply_spell(ch, cat, 24932)
    if ch.form == 'moonkin':
        _apply_spell(ch, cat, 24907)
    # Client Feline Swiftness references these separate dodge passives. Unlike
    # its movement aura, dodge is active indoors too (2/4 percentage points).
    if ch.form == 'cat' and ch.talents.get(807):
        _apply_spell(ch, cat, 24867 if ch.talents[807] == 1 else 24864)
    for index, stat in enumerate(('str', 'agi', 'sta', 'int', 'spi') if ch.talent_mode else ()):
        for aura in (80, 137):
            base = ls['inte' if stat == 'int' else stat] if aura == 80 else ch.stats[stat]
            value = base
            for a in ch.auras:
                if a['aura'] == aura and a['misc'] in (index, -1):
                    value *= 1 + a['amount'] / 100
            ch.stats[stat] = int(ch.stats[stat] + value - base) if aura == 80 else int(value)
    if equipment_modifiers:
        from service import crafting
        crafting.apply(ch, equipment_modifiers)
    return ch
