"""The WoW character the service computes for: base stats from CMaNGOS player_levelstats/classlevelstats,
equipped items (stats + on-equip spells) and item-set bonuses, reduced to the aura totals the formulas need."""
from dataclasses import dataclass, field

from service import formulas as F

# SpellAuraDefines.h
A_PERIODIC_DAMAGE, A_PERIODIC_HEAL, A_MOD_DAMAGE_DONE, A_MOD_ROOT, A_MOD_STAT = 3, 8, 13, 26, 29
A_MOD_SPELL_HIT, A_MOD_SPELL_CRIT, A_MOD_SPELL_CRIT_SCHOOL, A_MOD_POWER_REGEN = 55, 57, 71, 85
A_ADD_FLAT_MOD, A_ADD_PCT_MOD, A_OVERRIDE_CLASS_SCRIPTS, A_MOD_MANA_REGEN_INTERRUPT, A_MOD_HEALING_DONE = 107, 108, 112, 134, 135
A_MOD_CRIT_PERCENT, A_MOD_ATTACK_POWER = 52, 99
A_MOD_PARRY_PERCENT, A_MOD_DODGE_PERCENT, A_MOD_BLOCK_PERCENT, A_MOD_SKILL_TALENT = 47, 49, 51, 98
A_MOD_HEALTH_REGEN_PERCENT, A_MOD_REGEN_DURING_COMBAT, A_MOD_RANGED_ATTACK_POWER = 88, 116, 124
A_MOD_TOTAL_STAT_PERCENTAGE, A_MOD_PERCENT_STAT = 137, 80
A_MOD_SHIELD_BLOCKVALUE_PCT, A_MOD_SHIELD_BLOCKVALUE, A_MOD_HEALTH_REGEN_IN_COMBAT = 150, 158, 161
INVTYPE_WEAPONS = (13, 17, 21)   # one-hand, two-hand, main hand
INVTYPE_SHIELD = 14
# Item::GetSkill (Item.cpp:560) item_weapon_skills: a weapon's skill line by item_template subclass.
WEAPON_SKILLS = (44, 172, 45, 46, 54, 160, 229, 43, 55, 0, 136, 0, 0, 162, 0, 173, 176, 253, 226, 228, 356)
SKILL_UNARMED = 162
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
    base_health: int = 0
    item_health: int = 0
    auras: list = field(default_factory=list)   # {aura, amount, misc, spell, eff}
    mods: list = field(default_factory=F.SpellMods)   # spell modifiers for formulas.apply_spell_mod
    items: list = field(default_factory=list)
    custom_rules: bool = True     # labelled WoWBridge rules for content CMaNGOS leaves unimplemented (oracle turns off)
    # The main-hand weapon (item_template dmg_min1/dmg_max1/delay); bare hands as Player::InitStats sets them.
    weapon: dict = field(default_factory=lambda: {'min': 1.0, 'max': 2.0, 'delay': 2000})
    weapon_entry: int = 0
    weapon_skill: str = 'OneHanded'
    weapon_multiplier: float = 1.0
    weapon_damage: float = 0.0    # the main hand's native base damage (gear.json skyrim, its ESP record), Skyrim units
    weapon_unarmed: bool = False  # bare hands: the plugin sent her race's UnarmedDamage as weapon_damage
    weapon_temper: float = 1.0
    magicka_regen_percent: float = 0.0
    actor_values: dict | None = None
    native_health_bonus: float = 0.0
    equipment_modifiers: dict = field(default_factory=dict)
    form: str = 'none'
    race: int = 4
    item_resist: dict = field(default_factory=dict)
    talents: dict = field(default_factory=dict)
    talent_spells: set = field(default_factory=set)
    indoors: bool = False
    talent_mode: bool = False
    health_bonus_scales: dict = field(default_factory=dict)  # Skyrim rank floor applies to base power only
    native_mode: bool = False
    skyrim_skills: dict = field(default_factory=dict)
    feral_base_references: dict = field(default_factory=dict)
    feral_weapon_references: tuple = ()
    weapon_skill_line: int = SKILL_UNARMED   # Unit::GetWeaponSkillValue: the main hand's skill, unarmed without one
    shield_entry: int = 0                    # an equipped off-hand with a block value (Unit::CanBlockInCombat)
    block_flat: float = 0.0                  # SHIELD_BLOCK_VALUE FLAT_MOD from worn items' Block (Player.cpp:7097)
    # A non-Druid's stat scaled by a percentage aura: (create + item, buffs) before stat_total applied the percentages,
    # so one aura's own share can be measured (racials.passives: The Human Spirit).
    stat_parts: dict = field(default_factory=dict)

    def feral_weapon_floor(self, roll, low, high):
        """Only the naked claw roll gets the floor; gear/talent additions do not."""
        if not self.feral_weapon_references:
            return 0
        from service.skyrim_balance import FERAL_BASE_FLOOR
        position = max(0, min(1, (roll - low) / (high - low))) if high > low else .5
        own, top = self.feral_weapon_references
        base = own[0] + (own[1] - own[0]) * position
        reference = top[0] + (top[1] - top[0]) * position
        return FERAL_BASE_FLOOR * max(0, reference - base)

    def multiplier(self, aura, mask=None):
        value = 1.0
        for a in self.auras:
            if a['aura'] == aura and (self.talent_mode or a.get('item_use')) and (mask is None or a['misc'] & mask):
                value *= 1 + a['amount'] / 100.0
        return value

    def total(self, aura: int, misc_mask: int | None = None) -> int:
        # A row tagged custom_rule (paladin_talents' labelled WoWBridge rules) counts only while custom rules are on.
        return sum(a['amount'] for a in self.auras if a['aura'] == aura and (misc_mask is None or a['misc'] & misc_mask)
                   and (self.custom_rules or 'custom_rule' not in a))

    @property
    def max_mana(self) -> int:
        """Unit::UpdateMaxPower (StatSystem.cpp:191)."""
        return round(self.base_mana + self.item_mana + F.mana_from_intellect(self.stats['int']))

    @property
    def max_health(self) -> int:
        from service.racials import health_multiplier
        stamina = self.stats['sta']
        health = self.base_health + self.item_health + min(stamina, 20) + max(0, stamina - 20) * 10 + self.total(34)
        return int(health * health_multiplier(self) + .5)

    def spell_damage(self, school_mask: int) -> int:
        return self.total(A_MOD_DAMAGE_DONE, school_mask)       # Unit::SpellBaseDamageBonusDone

    def healing_done(self, school_mask: int) -> int:
        return self.total(A_MOD_HEALING_DONE, school_mask)      # Unit::SpellBaseHealingBonusDone

    def spell_crit(self, school: int) -> float:
        """Player::UpdateSpellCritChance (StatSystem.cpp:505)."""
        return (F.spell_crit_from_int(self.cls, self.level, self.stats['int']) + self.total(A_MOD_SPELL_CRIT)
                + self.total(A_MOD_SPELL_CRIT_SCHOOL, 1 << school))

    def attack_power(self) -> float:
        """Player::UpdateAttackPowerAndDamage (StatSystem.cpp:203): the class's base (a Druid or a Paladin: level x 3
        + strength x 2 - 20; Cat Form adds agility) plus SPELL_AURA_MOD_ATTACK_POWER. The Druid's sum is not clamped,
        as before; for other classes Unit::GetTotalAttackPowerValue (Unit.cpp:9175) never goes below zero."""
        value = (F.attack_power_base(self.cls, self.level, self.stats['str'], self.stats['agi'])
                 + self.total(A_MOD_ATTACK_POWER) + (self.stats['agi'] if self.form == 'cat' else 0))
        return value if self.cls == F.CLASS_DRUID else max(0.0, value)

    def ranged_attack_power(self) -> float:
        """Player::UpdateAttackPowerAndDamage(ranged) (StatSystem.cpp:217) plus SPELL_AURA_MOD_RANGED_ATTACK_POWER,
        never below zero (Unit::GetTotalAttackPowerValue)."""
        base = F.ranged_attack_power_base(self.cls, self.level, self.stats['agi'],
                                          self.form in ('cat', 'bear', 'dire_bear'))
        return max(0.0, base + self.total(A_MOD_RANGED_ATTACK_POWER))

    def weapon_skill_value(self) -> int:
        """Unit::GetWeaponSkillValue (Unit.cpp:4050): the main hand's skill at its maximum for the level (as melee.py
        has always rolled) plus SPELL_AURA_MOD_SKILL_TALENT bonuses to that skill (Human Sword/Mace Specialization,
        Aura::HandleAuraModSkill -> Player::ModifySkillBonus). A form without weapons fights at the maximum."""
        cap = self.level * 5
        if self.form in ('cat', 'bear', 'dire_bear'):
            return cap
        return cap + sum(a['amount'] for a in self.auras
                         if a['aura'] == A_MOD_SKILL_TALENT and a['misc'] == self.weapon_skill_line)

    def can_block(self) -> bool:
        """Spell 107 'Block' (SPELL_EFFECT_BLOCK) is a creation spell of Warriors, Paladins and Shamans."""
        return self.cls in F.BLOCK_CLASSES

    def can_parry(self) -> bool:
        """Spell 3127 'Parry' (SPELL_EFFECT_PARRY), from the class trainer at its level."""
        return self.cls in F.PARRY_LEVEL and self.level >= F.PARRY_LEVEL[self.cls]

    def dodge(self) -> float:
        """Player::UpdateDodgePercentage (StatSystem.cpp:490): the class's base, agility's share and
        SPELL_AURA_MOD_DODGE_PERCENT (defense at its maximum adds nothing)."""
        return (F.BASE_DODGE.get(self.cls, 0.0) + F.dodge_from_agility(self.cls, self.level, self.stats['agi'])
                + self.total(A_MOD_DODGE_PERCENT))

    def parry(self) -> float:
        """Player::UpdateParryPercentage (StatSystem.cpp:454): 5% + SPELL_AURA_MOD_PARRY_PERCENT once she can parry."""
        return F.BASE_PARRY + self.total(A_MOD_PARRY_PERCENT) if self.can_parry() else 0.0

    def block_chance(self) -> float:
        """Player::UpdateBlockPercentage (StatSystem.cpp:397): 5% + SPELL_AURA_MOD_BLOCK_PERCENT once she can block;
        an attack is blocked only with a shield equipped (Unit::CanBlockInCombat, Unit.cpp:3090)."""
        return F.BASE_BLOCK + self.total(A_MOD_BLOCK_PERCENT) if self.can_block() else 0.0

    def block_value(self) -> int:
        """Player::GetShieldBlockValue (Player.cpp:4884): worn Block plus SPELL_AURA_MOD_SHIELD_BLOCKVALUE, strength
        / 20 - 1, times SPELL_AURA_MOD_SHIELD_BLOCKVALUE_PCT (Player::HandleBaseModValue multiplies (100 + x) / 100)."""
        pct = 1.0
        for a in self.auras:
            if a['aura'] == A_MOD_SHIELD_BLOCKVALUE_PCT:
                pct *= (100.0 + (-200.0 if a['amount'] <= -100 else a['amount'])) / 100.0
        return F.shield_block_value(self.block_flat + self.total(A_MOD_SHIELD_BLOCKVALUE), self.stats['str'], pct)

    def health_regen(self) -> dict:
        """Player::RegenerateHealth (Player.cpp:2377): Spirit's health a second (Unit::OCTRegenHPPerSpirit) out of
        combat, times SPELL_AURA_MOD_HEALTH_REGEN_PERCENT; in combat only SPELL_AURA_MOD_REGEN_DURING_COMBAT's percent
        of it; SPELL_AURA_MOD_HEALTH_REGEN_IN_COMBAT always adds its flat amount. Sitting multiplies Spirit's by 1.5."""
        spirit = F.health_regen_per_second(self.cls, self.stats['spi'])
        resting = spirit
        for a in self.auras:
            if a['aura'] == A_MOD_HEALTH_REGEN_PERCENT:
                resting *= (100.0 + a['amount']) / 100.0
        combat = spirit * self.total(A_MOD_REGEN_DURING_COMBAT) / 100.0
        flat = self.total(A_MOD_HEALTH_REGEN_IN_COMBAT)
        return {'per_sec': max(0.0, resting + flat), 'per_sec_combat': max(0.0, combat + flat),
                'sitting_multiplier': 1.5, 'tick_ms': 2000}

    def melee_weapon(self) -> dict:
        """StatSystem.cpp:Player::CalculateMinMaxDamage; Player.cpp:InitDataForForm.

        Travel/Aquatic do not override attack speed and retain equipped damage.
        """
        if self.form not in ('cat', 'bear', 'dire_bear'):
            return self.weapon
        speed = 1.0 if self.form == 'cat' else 2.5
        return {'min': min(self.level, 60) * .85 * speed,
                'max': min(self.level, 60) * 1.25 * speed, 'delay': int(speed * 1000)}

    def native_weapon_average(self) -> float:
        """A plain hit of her weapon in Skyrim: its base damage at her skill, tempered (0: no native weapon - pure
        WoW, a form's claws, or a plugin that does not say)."""
        if not self.native_mode or not self.weapon_damage or self.form in ('cat', 'bear', 'dire_bear'):
            return 0.0
        from service.skyrim import SKILLS, weapon_skill_multiplier
        return self.weapon_damage * weapon_skill_multiplier(self.skyrim_skills.get(SKILLS[self.weapon_skill], 50)) * \
            self.weapon_temper

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

    def cast_speed(self) -> float:
        # Aura 65 is MOD_CASTING_SPEED_NOT_STACK (Unit::ApplyCastTimePercentMod).
        return max(.01, 1 + max((a['amount'] for a in self.auras if a['aura'] == 65), default=0) / 100)

    def resistance(self, school: int, attacker_level: int | None = None, dispel: int = 0) -> float:
        """Flat school resistance plus the strongest exclusive aura (StatSystem.cpp)."""
        mask = 1 << school
        exclusive = max((a['amount'] for a in self.auras if a['aura'] == 143 and a['misc'] & mask), default=0)
        value = max(0, self.item_resist.get(school, 0) + self.total(22, mask) + exclusive)
        if self.actor_values is not None:
            from service import native_values
            value += native_values.resistance(self.actor_values, school, attacker_level or self.level, self.level, dispel)
        return value

    def regen(self) -> dict:
        """Player::UpdateManaRegen (StatSystem.cpp:547): mana per second normally and inside the 5-second rule."""
        spirit = F.mana_regen_per_second(self.cls, self.stats['spi']) * max(0, 1 + self.magicka_regen_percent / 100)
        if self.actor_values is not None:
            from service import native_values
            spirit *= native_values.regen_multipliers(self.actor_values)['magicka']
        # Player::UpdateManaRegen (StatSystem.cpp:552): Innervate multiplies spirit regen,
        # not flat mp5; its other aura restores spirit regen inside the five-second rule.
        for a in self.auras:
            if a['aura'] == 110 and a['misc'] == 0:
                spirit *= 1 + a['amount'] / 100.0
        mp5 = self.total(A_MOD_POWER_REGEN) / 5.0
        interrupt = min(self.total(A_MOD_MANA_REGEN_INTERRUPT), 100)
        # per_sec_casting_spirit: the spirit regen that continues inside the rule (Reflection, Innervate, gear), which
        # Skyrim's own regeneration, paused after a cast, never gives.
        return {'per_sec': mp5 + spirit, 'per_sec_casting': mp5 + spirit * interrupt / 100.0,
                'per_sec_casting_spirit': spirit * interrupt / 100.0, 'tick_ms': 2000, 'fsr_ms': 5000}

    def class_scripts(self, script_id: int) -> list:
        return [a for a in self.auras if a['aura'] == A_OVERRIDE_CLASS_SCRIPTS and a['misc'] == script_id]


def _apply_spell(ch: Character, cat, spell_id: int) -> None:
    s = cat.spell(spell_id)
    from service.paladin import groups
    party_aura = spell_id in groups()['Aura'][1]   # her Paladin aura holds its party effects on her too (AreaAura)
    for eff in (1, 2, 3):
        if s[f'Effect{eff}'] != APPLY_AURA and not ((ch.talent_mode or party_aura) and s[f'Effect{eff}'] == 35):
            continue
        aura = s[f'EffectApplyAuraName{eff}']
        amount = int(F.effect_value(s, eff, ch.level, None, maximum=True,
                                    mods=ch.mods if ch.talent_mode else None))
        misc = s[f'EffectMiscValue{eff}']
        item_use = spell_id in cat.item_use_spells
        ch.auras.append({'aura': aura, 'amount': amount, 'misc': misc, 'spell': spell_id, 'eff': eff,
                         **({'item_use': True} if item_use else {})})
        if aura in (A_ADD_FLAT_MOD, A_ADD_PCT_MOD):
            # SpellModifier ctor (Player.cpp:340): mask from spell_affect else EffectItemType (SpellMgr.h:1951)
            ch.mods.append({'op': misc, 'type': aura, 'value': amount, 'family': s['SpellFamilyName'],
                            'mask': cat.affect_mask(spell_id, eff - 1), 'spell': spell_id,
                            'class_driven': ch.talent_mode})
        if aura == A_MOD_STAT:
            for k, stat in {0: 'str', 1: 'agi', 2: 'sta', 3: 'int', 4: 'spi'}.items():
                if misc in (k, -1):
                    ch.stats[stat] += amount


def class_id(value) -> int:
    """A request's class: a ChrClasses ID or its name (CLASS_IDS); a Druid (11) when absent."""
    if value is None:
        return F.CLASS_DRUID
    if isinstance(value, str) and value.lower() in F.CLASS_IDS:
        return F.CLASS_IDS[value.lower()]
    if isinstance(value, int) and not isinstance(value, bool) and value in F.CLASS_IDS.values():
        return value
    raise ValueError('caster.class must be a playable class ID or name')


def default_race(c: dict, cls: int) -> int:
    """The race a request without one has: the catalog character's for its own class (the Night Elf Druid), else
    the class's first race with CMaNGOS level stats (the Paladin's Human)."""
    from service import racials
    if cls == c.get('class', F.CLASS_DRUID):
        return c.get('race', 4)
    races = racials.races(cls)
    return min(races) if races else c.get('race', 4)


def base_rows(c: dict, cls: int, race: int, level: int) -> tuple[dict, dict]:
    """Player::InitStatsForLevel (Player.cpp): ObjectMgr::GetPlayerLevelInfo(race, class, level) - the
    player_levelstats row (base stats) - and GetPlayerClassLevelInfo(class, level) - the player_classlevelstats row
    (base health and mana) - from the catalog's levelstats_by_class and classlevelstats_by_class.

    The catalog character's own class (the Druid) keeps its rows as before: a race it cannot be takes the catalog
    character's own row (a Night Elf's); past level 60 her race's level-60 row (a Tauren's stays a Tauren's). A class without tables - every
    class but the playable ones, or any class in a catalog built before the class tables - computes with the Druid's
    rows, as every class did before. A playable class clamps its level to 1-60 and falls back to its first race."""
    from service import racials
    own = c.get('class', F.CLASS_DRUID)
    base = (c.get('classlevelstats_by_class') or {}).get(str(cls))
    tables = c.get('levelstats_by_class') or racials.data().get('levelstats_by_class', {})
    if cls == own or base is None or str(cls) not in tables:
        rows = tables.get(str(own), {}).get(str(race), {})
        # Past level 60 (Skyrim's level) her own race's level-60 row, not the catalog character's (a Night Elf's).
        top = max((int(n) for n in rows), default=60)
        ls = rows.get(str(level)) or (rows.get(str(top)) if int(level) > top else None) or \
            c.get('levelstats_by_level', {}).get(str(level), c['levelstats'])
        return ls, c.get('classlevelstats_by_level', {}).get(str(level), c['classlevelstats'])
    by_race = tables[str(cls)]
    rows = by_race.get(str(race)) or by_race[str(min(int(r) for r in by_race))]
    lv = str(min(max(int(level), 1), max(int(n) for n in rows)))
    return rows[lv], base[lv]


def build(cat, items: list[int], level: int = 60, cls: int = 11, auras: list[int] = (), form: str = 'none', race: int | None = None,
          talents: dict | None = None, indoors: bool = False, equipment_modifiers: dict | None = None,
          skyrim: bool | dict = False, actor_values: dict | None = None) -> Character:
    c = cat.raw['character']
    from service import racials
    race = default_race(c, cls) if race is None else race
    ls, cs = base_rows(c, cls, race, level)
    ch = Character(level=level, cls=cls, race=race, base_mana=cs['basemana'], base_health=cs['basehp'],
                   stats={'str': ls['str'], 'agi': ls['agi'], 'sta': ls['sta'], 'int': ls['inte'], 'spi': ls['spi']},
                   mods=F.SpellMods(family=F.CLASS_SPELL_FAMILY.get(cls, F.SPELLFAMILY_DRUID)))
    ch.native_mode = isinstance(skyrim, dict) or bool(skyrim)
    if actor_values is not None:
        from service import native_values
        ch.actor_values = native_values.snapshot(actor_values)
    ch.skyrim_skills = skyrim if isinstance(skyrim, dict) else {}
    if ch.native_mode:
        ch.health_bonus_scales = {sid: rule.bonus_health_factor / rule.health_factor
                                  for sid, rule in cat.balance.items() if rule.bonus_health_factor is not None}
        from service.skyrim_balance import SPELLS
        anchors = {cat.chain(sid): cat.spell(sid) for sid, rule in SPELLS.items()
                   if any(key == 'iron_sword' for key, _ in rule.health)}
        # Pounce's bleed is code-triggered, outside the ordinary rank table.
        for sid, row in cat.ranks.items():
            if cat.chain(sid) == cat.chain(9827):
                anchors[cat.spell(sid)['EffectTriggerSpell2']] = cat.spell(cat.spell(9827)['EffectTriggerSpell2'])
        ch.feral_base_references = {sid: anchors[cat.chain(sid)] for sid in cat.spells
                                    if cat.chain(sid) in anchors}
    from service import feral
    ch.form = feral.form_name({'form': form, **({'talents': talents} if talents is not None else {})})
    if ch.form != 'none' and cls != F.CLASS_DRUID:
        raise ValueError('caster.form: only a Druid shapeshifts')
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
            ch.weapon_skill_line = WEAPON_SKILLS[it['subclass']] if 0 <= it['subclass'] < len(WEAPON_SKILLS) else 0
    item_stats = dict.fromkeys(ch.stats, 0)   # BASE_VALUE stat mods (Player::_ApplyItemBonuses)
    for it in equipped:
        if it.get('block'):
            ch.block_flat += it['block']
            if it['InventoryType'] == INVTYPE_SHIELD and not ch.shield_entry:
                ch.shield_entry = it['entry']
        for school, name in ((1, 'holy'), (2, 'fire'), (3, 'nature'), (4, 'frost'), (5, 'shadow'), (6, 'arcane')):
            ch.item_resist[school] = ch.item_resist.get(school, 0) + it.get(name + '_res', 0)
        for k in range(1, 11):
            st, val = it[f'stat_type{k}'], it[f'stat_value{k}']
            name = STAT_TYPES.get(st)
            if not val or not name:
                continue
            if name == 'mana':
                ch.item_mana += val
            elif name == 'health':
                ch.item_health += val
            elif name in ch.stats:
                ch.stats[name] += val
                item_stats[name] += val
        for k in range(1, 6):
            if it[f'spellid_{k}'] and it[f'spelltrigger_{k}'] == 1:      # ITEM_SPELLTRIGGER_ON_EQUIP
                _apply_spell(ch, cat, it[f'spellid_{k}'])
    for s in cat.raw['item_sets'].values():                              # Player::AddItemsSetItem
        count = sum(1 for entry in s['ItemID'] if entry and entry in ch.items)
        # Each set earns its own bonus, even when another set uses the same spell.
        for spell_id, threshold in zip(s['SetSpellID'], s['SetThreshold']):
            if spell_id and threshold and count >= threshold:
                _apply_spell(ch, cat, spell_id)
    racial = set(racials.passive_spells(ch.cls, ch.race))
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
    # SpellStacking.cpp: positive family siblings with a similar aura effect never stack; an older save may carry both
    # Mark and Gift of the Wild. Keep the stronger (IsSimilarExistingAuraStronger), the higher total on a tie.
    from service import spells
    def strength(sid):
        s = cat.spell(sid)
        return {f"{s[f'Effect{e}']}:{s[f'EffectApplyAuraName{e}']}": abs(int(F.effect_value(s, e, level, None, maximum=True)))
                for e in spells._effects(s) if s[f'EffectApplyAuraName{e}']}
    from service.paladin import seals
    if len(set(auras) & seals()) > 1:   # spell_group 5 Seal is UNIQUE: a new seal replaces the old (paladin._seal)
        raise ValueError('caster.auras: one seal at a time')
    kept = []
    for sid in sorted(set(auras) & cat.spells.keys(), key=lambda sid: -sum(strength(sid).values())):
        key = spells.stack_group(cat.spell(sid))
        if not key or not any(spells.stack_group(cat.spell(k)) == key and strength(k).keys() & strength(sid).keys() for k in kept):
            kept.append(sid)
    auras = (set(auras) - cat.spells.keys()) | set(kept)
    for spell_id in sorted(((set(auras) - racials.ALL_PASSIVES) | (racial & cat.spells.keys())) - passive):
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
    # Player::UpdateStats / Unit::GetTotalStatValue: recompute each stat from
    # its base and flat buffs, then apply percentage groups in core float32.
    stats = ('str', 'agi', 'sta', 'int', 'spi') if cls != F.CLASS_DRUID or ch.talent_mode else ()
    for index, stat in enumerate(stats):
        pcts = {aura: [a['amount'] for a in ch.auras if a['aura'] == aura and a['misc'] in (index, -1)]
                for aura in (A_MOD_PERCENT_STAT, A_MOD_TOTAL_STAT_PERCENTAGE)}
        if pcts[A_MOD_PERCENT_STAT] or pcts[A_MOD_TOTAL_STAT_PERCENTAGE]:
            base = ls['inte' if stat == 'int' else stat] + item_stats[stat]
            ch.stat_parts[stat] = (base, ch.stats[stat] - base)
            ch.stats[stat] = F.stat_total(base, ch.stats[stat] - base, pcts[A_MOD_PERCENT_STAT],
                                          pcts[A_MOD_TOTAL_STAT_PERCENTAGE])
    if equipment_modifiers:
        from service import crafting
        crafting.apply(ch, equipment_modifiers)
    if cls != F.CLASS_DRUID:
        # Weapon-dependent talents and the Paladin's labelled custom rules (paladin_talents.build).
        from service import paladin_talents
        paladin_talents.build(cat, ch)
    if ch.native_mode and ch.form in ('cat', 'bear', 'dire_bear') and ch.level < 60:
        # Separate the naked roll (including innate AP) from equipment and
        # talent additions. These WoW controls never recurse into native tuning.
        bare = build(cat, [], ch.level, cls=cls, race=ch.race, form=ch.form)
        top = build(cat, [], 60, cls=cls, race=ch.race, form=ch.form)
        ch.feral_weapon_references = (bare.weapon_damage_range(), top.weapon_damage_range())
    return ch
