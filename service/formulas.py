"""WoW 1.12 combat formulas, ported from CMaNGOS Classic (mangos-classic @8ec338a, src/game/...).

Each function cites the routine it mirrors. Randomness always comes from a caller-supplied random.Random so
outcomes are reproducible from a seed. Percent chances use CMaNGOS's 0..100 scale.
"""
import math
import random
import struct
from pathlib import Path
from service import resistance

UNIT_CPP = Path(__file__).resolve().parents[2] / 'WoW/cmangos/mangos-classic/src/game/Entities/Unit.cpp'

# Spell families / classes
SPELLFAMILY_DRUID = 7
CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST = 1, 2, 3, 4, 5
CLASS_SHAMAN, CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID = 7, 8, 9, 11
# ChrClasses.dbc (1.12.1): ID -> Filename (lower case), and the class's SpellClassSet (its spells' SpellFamilyName).
CLASS_IDS = {'warrior': 1, 'paladin': 2, 'hunter': 3, 'rogue': 4, 'priest': 5, 'shaman': 7, 'mage': 8, 'warlock': 9,
             'druid': 11}
CLASS_SPELL_FAMILY = {1: 4, 2: 10, 3: 9, 4: 8, 5: 6, 7: 11, 8: 3, 9: 5, 11: 7}


def f32(value: float) -> float:
    """Round to an IEEE single, as CMaNGOS's float arithmetic stores every intermediate."""
    return struct.unpack('<f', struct.pack('<f', value))[0]
# DmgClass
DMG_NONE, DMG_MAGIC, DMG_MELEE, DMG_RANGED = 0, 1, 2, 3
# SpellModOp (SharedDefines.h)
SPELLMOD_DAMAGE, SPELLMOD_DURATION, SPELLMOD_COST, SPELLMOD_CASTING_TIME, SPELLMOD_COOLDOWN = 0, 1, 14, 10, 11
SPELLMOD_CRITICAL_CHANCE, SPELLMOD_RESIST_MISS_CHANCE, SPELLMOD_CRIT_DAMAGE_BONUS = 7, 16, 15
SPELLMOD_DOT, SPELLMOD_ALL_EFFECTS, SPELLMOD_SPELL_BONUS_DAMAGE = 22, 8, 24   # SpellDefines.h:396-420
SPELLMOD_FLAT, SPELLMOD_PCT = 107, 108   # == aura types ADD_FLAT_MODIFIER / ADD_PCT_MODIFIER


def irand(rng: random.Random, lo: int, hi: int) -> int:
    return rng.randint(lo, hi)


# Melee reach (WorldObject::GetCombinedCombatReach): both combat reaches plus BASE_MELEERANGE_OFFSET, at least
# ATTACK_DISTANCE; a player's combat reach is 1.5.
ATTACK_DISTANCE, BASE_MELEERANGE_OFFSET, DEFAULT_COMBAT_REACH = 5.0, 1.33, 1.5


def melee_reach(target_reach: float, own_reach: float = DEFAULT_COMBAT_REACH) -> float:
    return max(own_reach + target_reach + BASE_MELEERANGE_OFFSET, ATTACK_DISTANCE)


def armor_reduced(damage: float, armor: float, attacker_level: int) -> float:
    """Unit::CalcArmorReducedDamage (Unit.cpp:2337)."""
    reduction = 0.1 * max(armor, 0.0) / (8.5 * attacker_level + 40)
    reduction = min(max(reduction / (1.0 + reduction), 0.0), 0.75)
    return max(damage - damage * reduction, 1.0)


def melee_crit_from_agility(cls: int, level: int, agility: float) -> float:
    """Player::GetMeleeCritFromAgility (Player.cpp:4893): agility over a class rate between levels 1 and 60."""
    rates = {1: (3.9, 20.0), 2: (4.6, 20.0), 3: (3.5, 53.0), 4: (2.2, 29.0), 5: (11.0, 20.0), 7: (4.6, 20.0),
             8: (12.9, 20.0), 9: (8.4, 20.0), 11: (4.6, 20.0)}
    if cls not in rates:
        return 0.0
    lo, hi = rates[cls]
    return agility / (lo * (60.0 - level) / 59.0 + hi * (level - 1.0) / 59.0)


# StatSystem.cpp PLAYER_BASE_DODGE (Player::UpdateDodgePercentage): each class's static dodge percent.
BASE_DODGE = {1: 0.0, 2: 0.75, 3: 0.64, 4: 0.0, 5: 3.0, 7: 1.75, 8: 3.25, 9: 2.0, 11: 0.75}
# StatSystem.cpp Player::UpdateParryPercentage / UpdateBlockPercentage: 5% base once the unit CanParry / CanBlock.
BASE_PARRY = BASE_BLOCK = 5.0
# Unit::SetCanBlock comes from SPELL_EFFECT_BLOCK (Spell::EffectBlock, SpellEffects.cpp:4889): spell 107 'Block' is a
# creation spell (playercreateinfo_spell) of these classes only.
BLOCK_CLASSES = frozenset((1, 2, 7))
# Unit::SetCanParry comes from SPELL_EFFECT_PARRY (Spell::EffectParry, SpellEffects.cpp:4884): no class starts with
# spell 3127 'Parry'; trainers teach it (npc_trainer(_template) spell 3128) at these levels, by TrainerClass.
PARRY_LEVEL = {1: 6, 2: 8, 3: 8, 4: 12}


def dodge_from_agility(cls: int, level: int, agility: float) -> float:
    """Player::GetDodgeFromAgility (Player.cpp:4937): as the crit rate, except rogues' and hunters' own rates."""
    rates = {1: (3.9, 20.0), 2: (4.6, 20.0), 3: (1.8, 26.5), 4: (1.1, 14.5), 5: (11.0, 20.0), 7: (4.6, 20.0),
             8: (12.9, 20.0), 9: (8.4, 20.0), 11: (4.6, 20.0)}
    if cls not in rates:
        return 0.0
    lo, hi = rates[cls]
    return agility / (lo * (60.0 - level) / 59.0 + hi * (level - 1.0) / 59.0)


def attack_power_base(cls: int, level: int, strength: float, agility: float) -> float:
    """Player::UpdateAttackPowerAndDamage (StatSystem.cpp:203), melee: the class's BASE_VALUE before auras. A Druid's
    agility in Cat Form (SHAPESHIFT_FLAG_AGILITY_ATTACK_BONUS) is added by the caller."""
    level, strength, agility = float(level), float(strength), float(agility)
    if cls in (CLASS_WARRIOR, CLASS_PALADIN, CLASS_DRUID):
        return level * 3.0 + strength * 2.0 - 20.0
    if cls in (CLASS_ROGUE, CLASS_HUNTER):
        return level * 2.0 + strength + agility - 20.0
    if cls == CLASS_SHAMAN:
        return level * 2.0 + strength * 2.0 - 20.0
    if cls in (CLASS_MAGE, CLASS_PRIEST, CLASS_WARLOCK):
        return strength - 10.0
    return 0.0


def ranged_attack_power_base(cls: int, level: int, agility: float, feral_form: bool = False) -> float:
    """Player::UpdateAttackPowerAndDamage(ranged=true) (StatSystem.cpp:217): Hunters 2 x level + 2 x agility - 10,
    Rogues and Warriors level + agility - 10, a Druid in Cat/Bear/Dire Bear Form none, every other class agility - 10."""
    level, agility = float(level), float(agility)
    if cls == CLASS_HUNTER:
        return level * 2.0 + agility * 2.0 - 10.0
    if cls in (CLASS_ROGUE, CLASS_WARRIOR):
        return level + agility - 10.0
    if cls == CLASS_DRUID and feral_form:
        return 0.0
    return agility - 10.0


def shield_block_value(flat: float, strength: float, pct: float = 1.0) -> int:
    """Player::GetShieldBlockValue (Player.cpp:4884): (the shield's block + SPELL_AURA_MOD_SHIELD_BLOCKVALUE + strength
    / 20 - 1) x SPELL_AURA_MOD_SHIELD_BLOCKVALUE_PCT, never below zero, truncated."""
    return int(max(0.0, (flat + strength / 20.0 - 1.0) * pct))


def stat_total(base: float, total: float, base_pcts=(), total_pcts=()) -> int:
    """Unit::GetTotalStatValue (Unit.cpp:9063) in CMaNGOS's single precision, truncated by Player::UpdateStats:
    ((create + item stats) x BASE_PCT + buffs) x TOTAL_PCT. Each percent aura multiplies its group by
    (100 + amount) / 100 (Unit::HandleStatModifier, Unit.cpp:8995): SPELL_AURA_MOD_PERCENT_STAT the base,
    SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE the total (SpellAuras.cpp:3542)."""
    def product(amounts):
        value = 1.0
        for amount in amounts:
            amount = -200.0 if amount <= -100.0 else amount
            value = f32(value * f32(f32(100.0 + amount) / 100.0))
        return value
    value = f32(f32(f32(base) * product(base_pcts)) + f32(total))
    return int(f32(value * product(total_pcts)))


def roll_chance(rng: random.Random, pct: float) -> bool:
    """roll_chance_f: true with probability pct/100."""
    return pct > rng.random() * 100.0


# --- spell values -------------------------------------------------------------------------------

def spell_rank_level(spell: dict, caster_level: int) -> int:
    """Player::GetSpellRank (Player.cpp:5885) via the class skill (= 5 x level for trained druid skills),
    capped at maxLevel*5; WorldObject::CalculateSpellEffectValue then uses rank/5."""
    rank = caster_level * 5
    if spell['MaxLevel'] > 0 and rank >= spell['MaxLevel'] * 5:
        rank = spell['MaxLevel'] * 5
    return rank // 5


def effect_value(spell: dict, eff: int, caster_level: int, rng: random.Random | None, maximum: bool = False,
                 mods: list | None = None, base_points: int | None = None) -> float:
    """WorldObject::CalculateSpellEffectValue (Object.cpp:2916). eff is 1-based. base_points: a CastCustomSpell
    value, which stands in for EffectBasePoints + EffectBaseDice (basePoints = *effBasePoints - baseDice)."""
    base_dice = spell[f'EffectBaseDice{eff}']
    per_level = spell[f'EffectRealPointsPerLevel{eff}']
    dice_per_level = spell[f'EffectDicePerLevel{eff}']
    base = float(spell[f'EffectBasePoints{eff}'] if base_points is None else base_points - base_dice)
    rand_points = spell[f'EffectDieSides{eff}']
    if per_level != 0:
        level = spell_rank_level(spell, caster_level)
        if level > spell['MaxLevel'] and spell['MaxLevel'] > 0:
            level = spell['MaxLevel']
        elif level < spell['BaseLevel']:
            level = spell['BaseLevel']
        level -= spell['SpellLevel']
        base += level * per_level
        rand_points += int(level * dice_per_level)
    if rand_points in (0, 1):
        base += base_dice
    elif maximum:
        base += rand_points
    else:
        lo, hi = sorted((base_dice, rand_points))
        base += irand(rng, lo, hi)
    value = base
    if mods is not None:
        value = apply_spell_mod(spell, SPELLMOD_ALL_EFFECTS, value, mods, eff=eff)
    return value


def level_penalty(spell: dict) -> float:
    """Unit::CalculateLevelPenalty (Unit.cpp:2805)."""
    lvl = spell['SpellLevel']
    return 1.0 - (20.0 - lvl) * 0.0375 if 0 < lvl < 20 else 1.0


def bonus_with_coeffs(spell: dict, eff: int, total: int, benefit: int, mods: list) -> int:
    """Unit::SpellBonusWithCoeffs (Unit.cpp:6988) for a player caster (no AP coefficient on slice spells)."""
    coeff = spell[f'EffectBonusCoefficient{eff}'] if spell[f'EffectBonusCoefficient{eff}'] > 0 else 0.0
    if benefit:
        if coeff:
            coeff = apply_spell_mod(spell, SPELLMOD_SPELL_BONUS_DAMAGE, coeff * 100.0, mods) / 100.0
        total += int(benefit * coeff * level_penalty(spell))
    return total


# --- spell modifiers ----------------------------------------------------------------------------

def affects(mod: dict, spell: dict) -> bool:
    """SpellModifier::isAffectedOnSpell (Player.cpp:350): same family and family-flag mask match."""
    if 'spells' in mod:  # Skyrim perks have exact class/rank selectors, including zero-family-flag spells.
        return spell['Id'] in mod['spells']
    return mod['family'] == spell['SpellFamilyName'] and bool(int(spell['SpellFamilyFlags']) & mod['mask'])


class SpellMods(list):
    """Player::m_spellMods with its owner's class family (ChrClasses SpellClassSet): the spells apply_spell_mod's
    legacy gate lets through without class-driven mods. A plain list is the Druid's."""
    def __init__(self, mods=(), family: int = SPELLFAMILY_DRUID):
        super().__init__(mods)
        self.family = family


def apply_spell_mod(spell: dict, op: int, value: float, mods: list, eff: int | None = None) -> float:
    """Player::ApplySpellMod (Player.h:2577): sum flat and pct mods for op that affect spell."""
    if spell['SpellFamilyName'] != getattr(mods, 'family', SPELLFAMILY_DRUID) and not any(m.get('class_driven') for m in mods):
        return value
    flat, pct = 0, 100
    for m in mods:
        if m['op'] != op or not affects(m, spell):
            continue
        if 'effects' in m and eff not in m['effects'].get(spell['Id'], ()):
            continue
        if m['type'] == SPELLMOD_FLAT:
            flat += m['value']
        else:
            pct += m['value']
    return (value + flat) * pct / 100.0


# --- hit / crit / resist --------------------------------------------------------------------------

def spell_crit_from_int(cls: int, level: int, intellect: float) -> float:
    """Player::GetSpellCritFromIntellect (Player.cpp:4981): base + INT / (rate0 + rate1 * level)."""
    table = {11: (3.33, 12.41, 0.79), 8: (3.70, 14.77, 0.65), 5: (2.97, 10.03, 0.82), 7: (3.54, 11.51, 0.80),
             9: (3.18, 11.30, 0.82), 2: (3.70, 14.77, 0.65)}
    base, r0, r1 = table.get(cls, (0.0, 0.0, 10.0))
    return base + intellect / (r0 + r1 * level)


def spell_miss_chance(caster_level: int, target_level: int, target_is_player: bool, hit_bonus: float) -> float:
    """Unit::CalculateSpellMissChance (Unit.cpp:3855) with GetMissChance(school) = 4% base (Unit.cpp:3704)."""
    chance = 4.0
    diff = target_level - caster_level
    if diff > 2:
        chance += 2 + (diff - 2) * (7 if target_is_player else 11)
    else:
        chance += diff
    chance -= hit_bonus
    return max(1.0, min(chance, 100.0))


def skill_max(level: int) -> int:
    return level * 5


def effective_resist_percent(caster_level: int, target_level: int, resistance: float, binary: bool, target_is_creature: bool) -> float:
    """Unit::CalculateEffectiveMagicResistancePercent (Unit.cpp:3911), single school, no penetration."""
    if resistance >= 0:
        skill = max(skill_max(caster_level), 100)
        pct = resistance / skill * 100 * 0.75
        if not binary and target_is_creature:
            pct += 0.4 * max(skill_max(target_level) - skill, 0)
        return min(pct, 75.0)
    return resistance / max(skill_max(target_level), 100) * 100


_DISTRIBUTION: list | None = None
_RESIST_ROWS: list | None = None


def configure_resists(*, table=None, source=None):
    """Select and validate a local input before the service begins accepting requests."""
    global _RESIST_ROWS, _DISTRIBUTION
    if (table is None) == (source is None):
        raise ValueError("select exactly one resistance table or CMaNGOS source")
    rows = resistance.load(table) if table is not None else resistance.extract(source)["rows"]
    _RESIST_ROWS = rows
    _DISTRIBUTION = None


def partial_resist_distribution() -> list:
    """SPELL_PARTIAL_RESIST_DISTRIBUTION (Unit.cpp:~110-215), parsed from the C++ source and inflated exactly
    like InitSpellPartialResistDistribution (linear interpolation in 0.01% steps, rounded)."""
    global _DISTRIBUTION
    if _DISTRIBUTION is None:
        rows = _RESIST_ROWS if _RESIST_ROWS is not None else resistance.extract(UNIT_CPP)["rows"]
        out = []
        for row, src_row in enumerate(rows):
            if row + 1 == len(rows):
                out.append(src_row)
                break
            ahead = rows[row + 1]
            for i in range(100):
                out.append(tuple(max(0, int(b + _round_half_away(((a - b) * (i / 100.0))))) for b, a in zip(src_row, ahead)))
        _DISTRIBUTION = out
    return _DISTRIBUTION


def _round_half_away(x: float) -> float:
    """C ::round (half away from zero), unlike Python's banker's round()."""
    return math.floor(x + 0.5) if x >= 0 else -math.floor(-x + 0.5)


def partial_resist_multiplier(pct: float, dot: bool, binary: bool, attacker_is_player: bool, rng: random.Random,
                              melee: bool = False) -> float:
    """Unit::RollMagicResistanceMultiplierOutcomeAgainst (Unit.cpp:3964): fraction of damage resisted.
    The >54% 'no partial' bump applies only to melee damage (DIRECT_DAMAGE) or non-player attackers."""
    if pct < 0:
        return pct * 0.01
    pct *= 0.1 if (dot and binary) else 1.0
    index = int(pct * 100)
    if not index:
        return 0.0
    chances = partial_resist_distribution()[index]
    roll, acc, portion = rng.randint(1, 10000), 0, 4
    for side, c in enumerate(chances):
        acc += c
        if roll <= acc:
            portion = side
            break
    portion = min(portion, 4)
    if portion == 0 and pct > 54.0 and not binary and (melee or not attacker_is_player):
        portion += 1
    # portion / NUM_SPELL_PARTIAL_RESISTS (5), as CMaNGOS returns it - 20/40/60% though its enum names 25/50/75% and
    # retail 1.12 showed quarters: CMaNGOS is the gameplay authority and the reference realm (the oracle) runs it.
    return portion / 5.0


def crit_multiplier(dmg_class: int) -> float:
    """Unit::GetCritMultiplier (Unit.cpp:3552): 1.5 for magic/none spells (damage and healing), else 2."""
    return 1.5 if dmg_class in (DMG_NONE, DMG_MAGIC) else 2.0


# --- resources ----------------------------------------------------------------------------------

def mana_from_intellect(intellect: float) -> float:
    """Unit::GetManaBonusFromIntellect (StatSystem.cpp:157)."""
    base = min(intellect, 20.0)
    return base + (intellect - base) * 15.0


def mana_regen_per_second(cls: int, spirit: float) -> float:
    """Unit::OCTRegenMPPerSpirit (Unit.cpp:11907): per-2s tick value / 2; Warriors and Rogues none."""
    add = {11: spirit / 5 + 15, 3: spirit / 5 + 15, 8: spirit / 4 + 12.5, 2: spirit / 5 + 15,
           5: spirit / 4 + 12.5, 7: spirit / 5 + 17, 9: spirit / 5 + 15}.get(cls, 0.0)
    return add / 2.0


def health_regen_per_second(cls: int, spirit: float) -> float:
    """Unit::OCTRegenHPPerSpirit (Unit.cpp:11884): Player::RegenerateHealth (Player.cpp:2377) adds it times the
    elapsed seconds on each 2-second regeneration tick (REGEN_TIME_FULL), so it is health per second."""
    return {11: spirit * 0.11 + 1, 3: spirit * 0.43 - 5.5, 8: spirit * 0.11 + 1, 2: spirit * 0.25,
            5: spirit * 0.15 + 1.4, 4: spirit * 0.84 - 13, 7: spirit * 0.28 - 3.6, 9: spirit * 0.12 + 1.5,
            1: spirit * 1.26 - 22.6}.get(cls, 0.0)


def reward_rage(damage: int, level: int, attacker: bool) -> float:
    """Player.cpp:Player::RewardRage, default realm rate 1, in client rage units.

    The core truncates to integer tenths before ModifyPower. Damage is WoW
    damage after mitigation; resource gains must be computed before Skyrim scaling.
    """
    if damage < 0 or level < 1:
        raise ValueError('damage must be nonnegative and level must be positive')
    def f32(value):
        return struct.unpack('<f', struct.pack('<f', value))[0]
    conversion = f32(f32(0.0091107836 * level * level + 3.225598133 * level) + f32(4.2652911))
    gain = f32(f32(f32(damage) / conversion) * (7.5 if attacker else 2.5))
    return int(f32(gain * 10)) / 10


# --- crowd control --------------------------------------------------------------------------------

def damage_break_chance(damage: int, victim_level: int) -> float:
    """Unit::HandleRemoveByDamageChanceProc (UnitAuraProcHandler.cpp:1709)."""
    max_dmg = 25 * victim_level - 150 if victim_level > 8 else 50
    return damage / max_dmg * 100.0


def heartbeat(resist_chance_pct: float, duration_ms: int, dr_level: int = 0) -> tuple[float, int]:
    """SpellAuraHolder::SetHeartbeatResist (SpellAuras.cpp:6000): (chance %, interval ms)."""
    return resist_chance_pct * (1 + dr_level), max(1000, duration_ms // (2 + dr_level))
