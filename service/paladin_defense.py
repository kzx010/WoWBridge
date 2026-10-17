"""Paladin defence and control, as CMaNGOS Classic @8ec338a applies the 1.12.1 Spell.dbc rows.

Divine Protection, Divine Shield and Blessing of Protection (school immunity, pacify, Forbearance), Hammer of Justice,
Hammer of Wrath (the target's 20% health state and its ranged-class roll), Exorcism, Holy Wrath (each victim's creature
type), Turn Undead (a single-target fear on undead), Consecration (a ground area that ticks without a channel),
Righteous Fury (Holy threat), Holy Shield, and an incoming weapon hit rolled against her own defences (dodge, parry,
block, block value, Holy Shield, Redoubt, Reckoning).

spells.resolve dispatches the spells in DISPATCHED here; the server calls incoming(), check_swing()/swing_timing(),
immune_damage() and threat(). The Druid never reaches any of it: every path keys on these spell IDs, on a Paladin
caster, on auras only these spells grant, or on the optional incoming_hit 'attack' field her plugin never sends.
"""
import random

from service import formulas as F
from service import paladin, spells
from service.spells import CastError

# 1.12.1 Spell.dbc rank IDs per chain (data/paladin_manifest.json 'ranks'; tested against the catalog's chains).
DIVINE_PROTECTION = (498, 5573)
DIVINE_SHIELD = (642, 1020)
BLESSING_OF_PROTECTION = (1022, 5599, 10278)
HAMMER_OF_JUSTICE = (853, 5588, 5589, 10308)
HAMMER_OF_WRATH = (24275, 24274, 24239)
EXORCISM = (879, 5614, 5615, 10312, 10313, 10314)
HOLY_WRATH = (2812, 10318)
TURN_UNDEAD = (2878, 5627, 10326)
CONSECRATION = (26573, 20116, 20922, 20923, 20924)
RIGHTEOUS_FURY = (25780,)
HOLY_SHIELD = (20925, 20927, 20928)
REDOUBT = (20127, 20130, 20135, 20136, 20137)            # talent passives; each triggers its own block buff
REDOUBT_BUFFS = (20128, 20131, 20132, 20133, 20134)
RECKONING = (20177, 20179, 20181, 20180, 20182)          # talent passives; every rank triggers 20178
RECKONING_ATTACK = 20178
LAY_ON_HANDS = (633, 2800, 10310)
FORBEARANCE = 25771
DIVINE_INTERVENTION, DIVINE_INTERVENTION_AURA = (19752,), 19753
INVULNERABILITY = DIVINE_PROTECTION + DIVINE_SHIELD + BLESSING_OF_PROTECTION
DISPATCHED = frozenset(INVULNERABILITY + HAMMER_OF_JUSTICE + HAMMER_OF_WRATH + EXORCISM + HOLY_WRATH + TURN_UNDEAD +
                       CONSECRATION + RIGHTEOUS_FURY + HOLY_SHIELD + DIVINE_INTERVENTION)
E_SANCTUARY, E_TRIGGER_SPELL, E_INSTAKILL = 79, 64, 1
A_MOD_STUN, A_MOD_UNATTACKABLE = 12, 93
SPELL_ATTR_EX_EXCLUDE_CASTER = 0x80000

# SpellAuraDefines.h / SharedDefines.h / SpellDefines.h
A_FEAR, A_THREAT, A_PACIFY, A_MOD_SKILL, A_SCHOOL_IMMUNITY = 7, 10, 25, 30, 39
A_PROC_TRIGGER_SPELL, A_PROC_TRIGGER_DAMAGE, A_MOD_BLOCK_PERCENT, A_MECHANIC_IMMUNITY = 42, 43, 51, 77
A_MOD_SKILL_TALENT, A_MELEE_HASTE, A_MECHANIC_IMMUNITY_MASK = 98, 138, 147
A_ATTACKER_MELEE_HIT_CHANCE, A_ATTACKER_RANGED_HIT_CHANCE = 184, 185
A_ATTACKER_MELEE_CRIT_CHANCE, A_ATTACKER_RANGED_CRIT_CHANCE = 186, 187
A_MOD_CRIT_PERCENT, A_MOD_HIT_CHANCE = 52, 54
MECHANIC_INVULNERABILITY = 25
AURA_STATE_HEALTHLESS_20_PERCENT = 2
TARGET_UNIT_CASTER, TARGET_UNIT_RAID = 1, 57
SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT = 0x8000
ITEM_CLASS_ARMOR = 4
SKILL_DEFENSE = 95
SCHOOL_MASK_NORMAL, SCHOOL_MASK_MAGIC = 0x1, 0x7E
# UnitAuraProcHandler.cpp / SpellMgr.h ProcFlagsEx
PROC_EX_CRITICAL_HIT, PROC_EX_BLOCK = 0x2, 0x40
PROC_TAKE_MELEE, PROC_TAKE_RANGED, PROC_TAKE_HARMFUL_SPELL = 0x28, 0x280, 0x22000
MAX_EXTRA_ATTACKS = 5        # Spell::EffectAddExtraAttacks (SpellEffects.cpp:4877): m_extraAttacks capped at 5
CREATURE_CRIT_MULTIPLIER = 2.0   # Unit::GetCritMultiplier (Unit.cpp:3552): 2x for attacks
SPELL_CRIT_MULTIPLIER = 1.5      # the same, for a spell of damage class none or magic
SCHOOLS = ('physical', 'holy', 'fire', 'nature', 'frost', 'shadow', 'arcane')


def handles(sid: int) -> bool:
    return sid in DISPATCHED


def creature_types(mask: int) -> list:
    return [name for name, bit in spells.CREATURE_TYPES.items() if mask & bit]


# --- validation ---------------------------------------------------------------------------------------------------

def _aura_rows(cat, ch, target) -> list:
    """(aura, misc) of every aura effect on the spell's target: her own for a self cast (caster.auras, talents, gear),
    else the WoW spells the plugin lists on that unit (target.auras: spell IDs, e.g. Forbearance on a follower)."""
    if target['kind'] == 'self':
        return [(a['aura'], a['misc']) for a in ch.auras]
    ids = target.get('auras', [])
    if not isinstance(ids, list):
        raise ValueError('target.auras must be an array of spell IDs')
    rows = []
    for sid in ids:
        if not isinstance(sid, int) or isinstance(sid, bool) or sid not in cat.spells:
            raise ValueError('target.auras requires catalog spell IDs')
        s = cat.spell(sid)
        rows += [(s[f'EffectApplyAuraName{e}'], s[f'EffectMiscValue{e}']) for e in (1, 2, 3) if s[f'Effect{e}'] == 6]
    return rows


def _below_20(target) -> bool:
    """AURA_STATE_HEALTHLESS_20_PERCENT, which Unit::Update (Unit.cpp:534) keeps while health < max x 0.20f."""
    if 'health_pct' in target:
        from service.progression import number
        return number(target['health_pct'], 'target.health_pct') < 20.0
    if 'health' in target and 'max_health' in target:
        from service.progression import number
        health, maximum = number(target['health'], 'target.health'), number(target['max_health'], 'target.max_health')
        return maximum > 0 and health < F.f32(maximum * F.f32(0.20))
    return False   # unknown health: the state is not shown, as the client then refuses the cast


def _has_equipped(cat, ch, s) -> bool:
    """Player::HasItemFitToSpellReqirements (Player.cpp:18643), armor case: a worn item of the spell's class with a
    subclass in its mask (Holy Shield: a shield, class 4 subclass mask 0x40)."""
    for entry in ch.items:
        it = cat.item(entry)
        if it and it['class'] == s['EquippedItemClass'] and (1 << it['subclass']) & s['EquippedItemSubClassMask']:
            return True
    return False


def validate(cat, ch, s, req) -> None:
    t = req['target']
    mechanic = s['Mechanic']
    if mechanic and spells.is_positive(s):
        # Spell::CheckCast (Spell.cpp:4925): a positive spell on a target immune to it fails TARGET_AURASTATE. Forbearance
        # (25771) is SPELL_AURA_MECHANIC_IMMUNITY to MECHANIC_INVULNERABILITY, so it refuses Divine Shield, Divine
        # Protection and Blessing of Protection (Unit::IsImmuneToSpell, Unit.cpp:7368) for its minute.
        for aura, misc in _aura_rows(cat, ch, t):
            if (aura == A_MECHANIC_IMMUNITY and misc == mechanic) or \
                    (aura == A_MECHANIC_IMMUNITY_MASK and misc & (1 << (mechanic - 1))):
                raise CastError('SPELL_FAILED_TARGET_AURASTATE')
    if s['AttributesEx'] & SPELL_ATTR_EX_EXCLUDE_CASTER and t['kind'] != 'unit':
        # Divine Intervention: never on herself (SPELL_ATTR_EX_EXCLUDE_CASTER); a follower in her party (below).
        raise CastError('SPELL_FAILED_BAD_TARGETS')
    if s['Id'] in HAMMER_OF_WRATH and t['kind'] != 'unit':
        # TARGET_UNIT_ENEMY (6): Spell::CheckCast wants a hostile unit; herself or a point is no target for it.
        raise CastError('SPELL_FAILED_BAD_TARGETS')
    if s['TargetAuraState'] == AURA_STATE_HEALTHLESS_20_PERCENT and t['kind'] == 'unit' and not _below_20(t):
        # Hammer of Wrath: Spell.dbc TargetAuraState 2. CMaNGOS's CheckCast leaves this column to the 1.12 client, which
        # refuses the cast with SPELL_FAILED_TARGET_AURASTATE; the service is that client here.
        raise CastError('SPELL_FAILED_TARGET_AURASTATE')
    if t['kind'] == 'unit' and any(s[f'EffectImplicitTargetA{e}'] == TARGET_UNIT_RAID for e in spells._effects(s)) \
            and not t.get('party', False):
        # Spell::CheckCast (Spell.cpp:5088): TARGET_UNIT_RAID (group filter) needs a unit in her party or raid; the
        # plugin marks her followers (target.party). Herself is a self cast.
        raise CastError('SPELL_FAILED_BAD_TARGETS')
    if s['EquippedItemClass'] == ITEM_CLASS_ARMOR and not _has_equipped(cat, ch, s):
        # Spell::CheckItems (Spell.cpp:6540).
        raise CastError('SPELL_FAILED_EQUIPPED_ITEM_CLASS')


# --- casts --------------------------------------------------------------------------------------------------------

def resolve(cat, ch, req) -> dict:
    s = cat.spell(req['spell'])
    sid, t = s['Id'], req['target']
    validate(cat, ch, s, req)
    area_self = sid in HOLY_WRATH and t['kind'] == 'self'
    if area_self:
        # Spell::CheckCast checks a target's creature type only for a non-caster target ("ignore self casts, including
        # area casts when caster selected as target", Spell.cpp:4911): Holy Wrath checks each victim instead (below).
        spells._validate(cat, {**s, 'TargetCreatureType': 0}, ch, req)
        # The resolver's frame (cost, cast, cooldown, interrupts): its one self-centred damage roll is discarded.
        req = {**req, 'target': {**t, 'level': ch.level}}
    out = spells.resolve(cat, ch, req, feral_dispatch=False, triggered=area_self)
    _category(cat, ch, s, out)
    if s['TargetCreatureType']:
        out['creature_types'] = creature_types(s['TargetCreatureType'])
    if sid in INVULNERABILITY:
        _invulnerability(cat, s, out)
    elif sid in HAMMER_OF_WRATH:
        _ranged_ability(cat, ch, s, req, out)
    elif sid in HOLY_WRATH:
        _holy_wrath(cat, ch, s, req, out)
    elif sid in TURN_UNDEAD:
        _turn_undead(cat, s, req, out)
    elif sid in CONSECRATION:
        _consecration(s, out)
    elif sid in RIGHTEOUS_FURY:
        _righteous_fury(s, out)
    elif sid in HOLY_SHIELD:
        _holy_shield(s, out)
    elif sid in DIVINE_INTERVENTION:
        _divine_intervention(cat, s, out)
    return out


def _divine_intervention(cat, s, out) -> None:
    """Divine Intervention (Paladin.cpp:141-153, its Symbol of Divinity a reagent): Spell::EffectSanctuary on the follower
    (SpellEffects.cpp:4193: spells cast on it interrupted, its combat stopped), the trigger 19753 on it (three minutes
    immune to every school, stunned and unattackable: auras 39/12/93), and EffectInstaKill on the caster."""
    aura = cat.spell(DIVINE_INTERVENTION_AURA)
    duration = cat.duration(aura, 0)
    auras = []
    for e in spells._effects(aura):
        kind = aura[f'EffectApplyAuraName{e}']
        row = {'spell': DIVINE_INTERVENTION_AURA, 'eff': e, 'aura': kind, 'duration_ms': duration, 'amplitude_ms': 0,
               'stack': 'replace_same_caster'}
        if kind == A_SCHOOL_IMMUNITY:
            misc = aura[f'EffectMiscValue{e}']
            row.update(type='school_immunity', school_mask=misc, schools=[n for i, n in enumerate(SCHOOLS) if misc & (1 << i)],
                       purges_existing=False, suppresses_threat=True)
        elif kind == A_MOD_STUN:
            row.update(type='stun', breaks_on_damage=False, single_target=False)
        elif kind == A_MOD_UNATTACKABLE:
            row.update(type='unattackable')   # Aura::HandleAuraModUnattackable: UNIT_FLAG_NON_ATTACKABLE
        auras.append(row)
    out['auras'] = auras
    out['events'] = [e for e in out.get('events', []) if e.get('type') not in ('sanctuary', 'instakill')] + [
        {'type': 'sanctuary', 'target': 'target'},
        {'type': 'instakill', 'target': 'caster'}]
    out['immunity'] = {'school_mask': next((a['school_mask'] for a in auras if a['type'] == 'school_immunity'), 0),
                       'duration_ms': duration}


def _category(cat, ch, s, out) -> None:
    """Spell.dbc Category: Player::AddCooldown (Player.cpp:20196) starts the category timer for every spell in it
    (Divine Shield and Divine Protection share category 37; Blessing of Protection has 20), its CategoryRecoveryTime
    through the cast's SPELLMOD_COOLDOWN as the spell's own (Player.cpp:20264-20269: Improved Hammer of Justice,
    Guardian's Favor), never below 0."""
    if s['Category'] and s['CategoryRecoveryTime']:
        from service.skyrim_balance import spell_classes
        classes = spell_classes()
        out['cooldown_category'] = s['Category']
        out['category_cooldown_ms'] = max(0, int(F.apply_spell_mod(s, F.SPELLMOD_COOLDOWN, s['CategoryRecoveryTime'],
                                                                   ch.mods)))
        out['category_spells'] = sorted(sid for sid in cat.class_spells if sid in cat.spells and
                                        classes.get(sid) == classes.get(s['Id']) and
                                        cat.spell(sid)['Category'] == s['Category'])


def _invulnerability(cat, s, out) -> None:
    """Divine Shield (all schools, attack time doubled), Divine Protection and Blessing of Protection (physical, pacify),
    and the Forbearance each applies to its target first (Spell::prepare AddPrecastSpell(25771), Spell.cpp:3218/3246)."""
    purge = bool(s['AttributesEx'] & SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT)
    mask = 0
    for a in out['auras']:
        misc = s[f"EffectMiscValue{a['eff']}"]
        if a['aura'] == A_SCHOOL_IMMUNITY:
            # Aura::HandleAuraModSchoolImmunity (SpellAuras.cpp:2997): immune to the schools in misc (1 physical, 126
            # every magic school); IMMUNITY_PURGES_EFFECT removes her negative auras of those schools; HostileRefManager::
            # HandleSuppressed drops her from attackers' threat while it lasts (they turn to others).
            a.update(type='school_immunity', school_mask=misc, schools=[n for i, n in enumerate(SCHOOLS) if misc & (1 << i)],
                     purges_existing=purge, suppresses_threat=True)
            a.pop('amount', None)
            mask |= misc
        elif a['aura'] == A_PACIFY:
            # Aura::HandleAuraModPacify (SpellAuras.cpp:4387): UNIT_FLAG_PACIFIED - no auto attack
            # (Unit::AttackerStateUpdate, Unit.cpp:2614) and no PreventionType-2 spells (Spell::CheckCasterAuras, 6082).
            a.update(type='pacify', blocks_auto_attack=True, prevention_type=2)
            a.pop('amount', None)
        elif a['aura'] == A_MELEE_HASTE:
            # Unit::ApplyAttackTimePercentMod (Unit.cpp:10474): a negative amount multiplies the attack time by
            # (100 - amount) / 100 - Divine Shield's -100% doubles it (half speed), it does not stop attacks.
            a['attack_time_multiplier'] = attack_time_multiplier(a['amount'])
    forbearance = cat.spell(FORBEARANCE) if FORBEARANCE in cat.spells else None
    if forbearance:
        out['auras'].append({
            'spell': FORBEARANCE, 'eff': 1, 'aura': A_MECHANIC_IMMUNITY, 'type': 'mechanic_immunity',
            'mechanic': forbearance['EffectMiscValue1'], 'name': 'invulnerability',
            'duration_ms': cat.duration(forbearance, 0), 'amplitude_ms': 0, 'stack': 'replace',
            # Unaffected by the immunity it accompanies (SPELL_ATTR_NO_IMMUNITIES) and not cancellable or dispellable.
            'precast': True, 'debuff': bool(forbearance['Attributes'] & 0x04000000),
            'blocks_spells': sorted(INVULNERABILITY)})
    out['immunity'] = {'school_mask': mask, 'duration_ms': next((a['duration_ms'] for a in out['auras']
                                                                 if a['type'] == 'school_immunity'), 0)}


def attack_time_multiplier(amount: float) -> float:
    """Unit::ApplyAttackTimePercentMod via ApplyPercentModFloatVar (Util.h:156): the attack time multiplier of one
    SPELL_AURA_MOD_MELEE_HASTE amount."""
    return (100.0 - amount) / 100.0 if amount < 0 else 100.0 / (100.0 + amount)


def ranged_total(cat, ch, aura) -> int:
    """Her SPELL_AURA_MOD_HIT_CHANCE / SPELL_AURA_MOD_CRIT_PERCENT for the ranged attack type, which a ranged-class
    spell's roll reads (Unit::GetHitChance / GetCritChance by GetWeaponAttackType: RANGED_ATTACK). An item-neutral aura
    (Spell.dbc EquippedItemClass -1) counts for every attack type (Player::UpdateWeaponHitChances, StatSystem.cpp:518;
    Aura::HandleAuraModCritPercent, SpellAuras.cpp:3843); a weapon-dependent one only while her ranged weapon fits it
    (Player::_ApplyWeaponDependentAuraCritMod, Player.cpp:7191). A Paladin has no ranged weapon in 1.12 (and
    paladin_talents.build keeps only the main hand's weapon auras), so Precision and Conviction - main-hand weapon
    auras - never reach Hammer of Wrath."""
    return sum(a['amount'] for a in ch.auras if a['aura'] == aura and (ch.custom_rules or 'custom_rule' not in a)
               and (a['spell'] not in cat.spells or cat.spell(a['spell'])['EquippedItemClass'] == -1))


def _ability_miss(ch, s, skill, defense, vs_player, hit=None) -> float:
    """Unit::CalculateEffectiveMissChance (Unit.cpp:3770) for an ability, the caster a player. hit: her weapon hit
    chance for the ability's attack type (the main hand's, ch.total(54), unless given)."""
    chance = 5.0                                     # GetMissChance(attack type)
    difference = defense - skill
    factor = 0.04
    if not vs_player and difference > 0:
        if difference > 10:
            chance += 10 * 0.1
            difference -= 10
            factor = 0.4
            chance += difference * 0.2
        else:
            factor = 0.1
    chance += difference * factor
    # GetHitChance(ability): the weapon hit of its attack type (SPELL_AURA_MOD_HIT_CHANCE) with RESIST_MISS spell mods.
    chance -= F.apply_spell_mod(s, F.SPELLMOD_RESIST_MISS_CHANCE, ch.total(54) if hit is None else hit, ch.mods)
    minimum = 1.0 if difference > 10 else 0.0
    return max(minimum, min(chance, 100.0))


def _ranged_ability(cat, ch, s, req, out) -> None:
    """Hammer of Wrath is DmgClass RANGED (Spell.dbc): Unit::SpellHitResult sends it to MeleeSpellHitResult (no magic
    resist side, no dodge or parry: Unit::CanDodgeAbility/CanParryAbility want melee, deflect is a ranged parry of 0);
    its crit is the ranged crit with the attack table's suppression (CalculateSpellCritChance -> CalculateEffectiveCrit
    Chance, Unit.cpp:3733), x2; hit and crit chance are the ranged attack type's (ranged_total: no main-hand weapon
    auras); MeleeDamageBonusDone (Unit.cpp:7460) gives a non-weapon Holy ability the same Holy damage x coefficient and
    percent mods as a spell, then the target's Judgement of the Crusader (MeleeDamageBonusTaken, paladin.taken_bonus);
    a creature may partially block it (CalculateAbsorbResistBlock -> RollAbilityPartialBlockOutcome) before the Holy
    partial resist. The magic roll spells.resolve made is replaced."""
    from service import melee, native_values
    t = req['target']
    rng = random.Random(req.get('seed', 0))
    level, vs_player = int(t['level']), bool(t.get('is_player', False))
    skill, defense = ch.level * 5, level * 5         # GetSkillMaxForLevel: IsSpellUseWeaponSkill is false (SpellMgr.h:1500)
    miss = _ability_miss(ch, s, skill, defense, vs_player, ranged_total(cat, ch, A_MOD_HIT_CHANCE))
    # Player::UpdateAllCritPercentages: RANGED_CRIT_PERCENTAGE starts from the same agility share as the main hand's.
    ranged_crit = F.melee_crit_from_agility(ch.cls, ch.level, ch.stats['agi']) + ranged_total(cat, ch, A_MOD_CRIT_PERCENT)
    crit_chance = F.apply_spell_mod(s, F.SPELLMOD_CRITICAL_CHANCE, ranged_crit, ch.mods)
    if crit_chance >= 0.005:
        crit_chance = max(0.0, min(crit_chance + (skill - defense) * (0.04 if vs_player else 0.2), 100.0))
    out.update(hit_chance=100.0 - miss, crit_chance=crit_chance, events=[])
    if melee._die(rng, [('miss', miss)]) == 'miss':
        out['events'].append({'type': 'miss'})
        return
    if native_values.absorbs(t, s['School'], rng):
        out['events'].append({'type': 'absorb', 'mana': out['cost']})
        return
    base, raw_base = spells._base_value(ch, s, 1, rng)
    crit = F.roll_chance(rng, crit_chance)
    dmg = paladin.taken_bonus(cat, ch, s, 1, t, spells._damage_done(ch, s, 1, base, dot=False, raw_base=raw_base))
    if crit:
        multiplier = F.crit_multiplier(s['DmgClass'])
        if ch.talent_mode:
            multiplier = 1 + F.apply_spell_mod(s, F.SPELLMOD_CRIT_DAMAGE_BONUS, multiplier - 1, ch.mods)
        dmg = max(dmg, int(dmg * multiplier))
    blocked = 0
    if not vs_player and not t.get('behind', False):
        # Unit::CalculateEffectiveBlockChance (Unit.cpp:3285): a creature's 5%, no bonus from a positive difference.
        difference = defense - skill
        block = max(0.0, min(5.0 + (difference * 0.04 if difference <= 0 else 0.0), 100.0))
        if F.roll_chance(rng, block):
            blocked = min(dmg, level // 2 + int(cat.creature_strength(level) / 20))   # Creature::GetShieldBlockValue
    dmg -= blocked
    resisted = int(dmg * F.partial_resist_multiplier(spells._resist_pct(ch, s, t, False), False, False, True, rng))
    out['events'].append({'type': 'damage', 'amount': dmg - resisted, 'resisted': resisted, 'blocked': blocked,
                          'crit': crit, 'school': s['School']})


def _holy_wrath(cat, ch, s, req, out) -> None:
    """Holy Wrath: an area at the caster (TARGET_LOCATION_CASTER_SRC / ENEMY_AOE_AT_SRC_LOC, radius 20 yd) whose victims
    each pass Spell::CheckTarget -> CheckTargetCreatureType (Spell.cpp:6941) on their own: undead and demons are struck,
    every other unit in the area is left alone (target.creature_type; an unknown type is not assumed to qualify).
    Like War Stomp, the plugin asks for the victims (req.targets) when the 2-second cast lands; each gets its own hit,
    roll, crit and partial resist (spells.resolve, triggered)."""
    radius = cat.radius_yd(s, 1)
    out['events'] = []
    out['area'] = {'radius_yd': radius, 'hostile': True, 'channel': False,
                   'creature_types': creature_types(s['TargetCreatureType']),
                   **({'max_targets': s['MaxAffectedTargets']} if s['MaxAffectedTargets'] else {})}
    if 'targets' not in req:
        return
    out['results'], out['excluded'] = [], []
    rng = random.Random(req.get('seed', 0))
    for victim in req['targets']:
        if victim.get('dead', not victim.get('alive', True)) or victim.get('friendly', victim.get('reaction') == 'friendly') \
                or not victim.get('attackable', True) or not victim.get('los', True):
            continue
        if float(victim.get('distance_yd', 0)) > radius:
            continue
        if not s['TargetCreatureType'] & spells.CREATURE_TYPES.get(victim.get('creature_type'), 0):
            out['excluded'].append(victim['id'])
            continue
        hit = spells.resolve(cat, ch, {**req, 'target': {**victim, 'kind': 'unit', 'distance_yd': 0},
                                       'seed': rng.getrandbits(32)}, feral_dispatch=False, triggered=True)
        for event in hit['events']:
            out['results'].append({'id': victim['id'], **event})


def _turn_undead(cat, s, req, out) -> None:
    """Turn Undead: SPELL_AURA_MOD_FEAR on an undead target (TargetCreatureType 0x20), MECHANIC_TURN - one turned unit
    at a time (SpellMgr::IsSingleTargetSpell, SpellMgr.h:2242) - with its +25% run speed (effect 2). Damage may end it
    (UnitAuraProcHandler.cpp:40 HandleRemoveByDamageChanceProc, as roots), and its heartbeat resist rolls
    (SPELL_ATTR_HEARTBEAT_RESIST)."""
    t = req['target']
    rng = random.Random(f"{req.get('seed', 0)}:turn_undead")
    for a in out['auras']:
        if a['aura'] == A_FEAR:
            a.update(type='fear', mechanic='turn', single_target=spells.single_target(cat, s),
                     damage_break={'victim_level': t.get('level', 0)})
            a.pop('amount', None)
            spells._heartbeat(a, s, False, 100.0 - out.get('hit_chance', 100.0), a['duration_ms'], rng)


def _consecration(s, out) -> None:
    """Consecration: SPELL_EFFECT_PERSISTENT_AREA_AURA (27) at TARGET_LOCATION_CASTER_DEST - a dynamic object left on
    the ground where she stood, ticking every EffectAmplitude for its duration whether she moves, casts or not (it is
    not a channel: IsChanneledSpell, SpellMgr.h:1371). tick_area resolves each tick's victims."""
    for a in out['auras']:
        if 'area' in a:
            a['area'].update(anchor='caster_position', hostile=True, tick_ms=a['amplitude_ms'],
                             ticks=a['duration_ms'] // a['amplitude_ms'] if a['amplitude_ms'] else 0)


def _righteous_fury(s, out) -> None:
    """Righteous Fury: SPELL_AURA_MOD_THREAT (Aura::HandleModThreat, SpellAuras.cpp:2811) on the schools in misc (2:
    Holy), +amount% threat (Improved Righteous Fury's ALL_EFFECTS spell mod included)."""
    for a in out['auras']:
        if a['aura'] == A_THREAT:
            misc = s[f"EffectMiscValue{a['eff']}"]
            a.update(type='threat', school_mask=misc, multiplier=1 + a['amount'] / 100.0)


def _holy_shield(s, out) -> None:
    """Holy Shield: +block chance (SPELL_AURA_MOD_BLOCK_PERCENT) and SPELL_AURA_PROC_TRIGGER_DAMAGE on a block
    (spell_proc_event 20925 procEx BLOCK), ProcCharges blocks; the proc's damage comes from incoming_hit."""
    for a in out['auras']:
        if a['aura'] == A_MOD_BLOCK_PERCENT:
            a['type'] = 'block_chance'
        elif a['aura'] == A_PROC_TRIGGER_DAMAGE:
            a.update(type='block_damage', school=s['School'], on='block')
        a['charges'] = s['ProcCharges']


# --- her defences against an incoming hit ------------------------------------------------------------------------

def _skill_bonus(ch, skill) -> int:
    return sum(a['amount'] for a in ch.auras if a['aura'] in (A_MOD_SKILL, A_MOD_SKILL_TALENT) and a['misc'] == skill)


def immune(ch, school_mask: int) -> bool:
    """Unit::IsImmuneToDamage (Unit.cpp:7326): a SPELL_AURA_SCHOOL_IMMUNITY covering the damage's school."""
    return any(a['aura'] == A_SCHOOL_IMMUNITY and a['misc'] & school_mask for a in ch.auras)


def defense_table(ch, attacker: dict, ranged: bool = False, ctx: dict | None = None) -> dict:
    """Unit::RollMeleeOutcomeAgainst (Unit.cpp:2709) with a creature attacking her: its skill is level x 5
    (GetUnitMeleeSkill), hers her Defense (level x 5 plus SPELL_AURA_MOD_SKILL/_TALENT 95). Against a player every
    skill difference counts 0.04 a point (CalculateEffectiveMiss/Dodge/Parry/BlockChance); a creature crits 5% plus
    0.04 a point and crushes from 15 points over her capped Defense (CalculateEffectiveCrushChance). She dodges, parries
    and blocks only what is in front of her and not while casting; parry needs a weapon, block a shield (CanBlockInCombat).
    A sitting player is always crit."""
    ctx = ctx or {}
    level = int(attacker['level'])
    skill = level * 5
    cap = ch.level * 5
    defense = cap + _skill_bonus(ch, SKILL_DEFENSE)
    diff = defense - skill
    sides = {}
    miss = 5.0 + diff * 0.04 - ch.total(A_ATTACKER_RANGED_HIT_CHANCE if ranged else A_ATTACKER_MELEE_HIT_CHANCE)
    sides['miss'] = max(1.0 if diff > 10 else 0.0, min(miss, 100.0))
    if ctx.get('sitting'):
        sides['crit'] = 100.0
        return sides
    react = not ctx.get('stunned') and not ctx.get('casting') and attacker.get('in_front', True)

    def side(own):
        return max(0.0, min(own + diff * 0.04, 100.0)) if react and own >= 0.005 else 0.0
    sides['dodge'] = side(ch.dodge())
    sides['parry'] = 0.0 if ranged or not ch.weapon_entry else side(ch.parry())
    sides['block'] = side(ch.block_chance()) if ch.shield_entry else 0.0
    crit = 5.0 + (skill - defense) * 0.04 + ch.total(A_ATTACKER_RANGED_CRIT_CHANCE if ranged else A_ATTACKER_MELEE_CRIT_CHANCE)
    sides['crit'] = max(0.0, min(crit, 100.0))
    deficit = skill - min(defense, cap)
    sides['crush'] = max(0.0, min(2.0 * deficit - 15.0, 100.0)) if deficit >= 15 else 0.0
    return sides


def incoming(cat, ch, req, out) -> dict:
    """incoming_hit with 'attack': the WoW outcome of a creature's hit on her, before Skyrim applies it.

    attack: {damage: the hit as she would take it (Skyrim health with caster.skyrim, else WoW), kind: 'melee' (default),
    'ranged' or 'spell', school: 0-6 (default 0), crit: a spell's own crit (kind 'spell' only)}; target: the attacker
    (level, in_front: the attacker in her frontal arc); ctx: casting/sitting/stunned.
    Physical school immunity first (CalculateMeleeDamage, Unit.cpp:1924), then the table, the block (her block value:
    Player::GetShieldBlockValue), and the procs a block or a crit raises (Holy Shield, Redoubt, Reckoning). Procs the
    earlier layers found (Nature's Grasp, gear) need a landed hit (procEx NONE wants a hit or crit, SpellMgr.cpp:913)."""
    attack = req.get('attack')
    if attack is None:
        return out
    from service import melee
    from service.progression import number
    if not isinstance(attack, dict):
        raise ValueError('attack must be an object')
    kind = attack.get('kind', 'melee')
    if kind not in ('melee', 'ranged', 'spell'):
        raise ValueError("attack.kind must be 'melee', 'ranged' or 'spell'")
    school = attack.get('school', 0)
    if isinstance(school, bool) or school not in range(7):
        raise ValueError('attack.school must be a school from 0 to 6')
    damage = number(attack.get('damage', 0), 'attack.damage')
    if damage < 0:
        raise ValueError('attack.damage must be nonnegative')
    native = 'skyrim' in req.get('caster', {})
    ctx, t = req.get('ctx', {}), req['target']
    rng = random.Random(f"{req.get('seed', 0)}:paladin_defense")
    result = {'kind': kind, 'school': school}
    out['defense'] = result
    if immune(ch, 1 << school):
        result.update(outcome='immune', damage=0, multiplier=0)
        _drop_procs(out)
        return out
    from service import paladin_auras
    flat = paladin_auras.taken_flat(ch, school, native)
    if flat:
        # SPELL_AURA_MOD_DAMAGE_TAKEN (Blessing of Sanctuary): Unit::CalculateMeleeDamage takes it off the hit before
        # the outcome's multiplier and the block (MeleeDamageBonusTaken, Unit.cpp:1935, 7635).
        reduced = min(damage, -flat) if flat < 0 else -flat
        damage -= reduced
        result['damage_reduced'] = round(reduced, 2) if native else int(reduced)
    if kind == 'spell':
        outcome = 'crit' if attack.get('crit', False) else 'hit'
    else:
        table = defense_table(ch, t, kind == 'ranged', ctx)
        if school:
            table['block'] = 0.0     # Pre-WotLK: no block against elemental/magic melee (CanBlockInCombat, Unit.cpp:3093)
        result['chances'] = table
        outcome = melee._die(rng, list(table.items()))
        if kind == 'ranged':     # CalculateMeleeDamage: no parry or dodge of a ranged attack
            outcome = {'parry': 'hit', 'dodge': 'miss'}.get(outcome, outcome)
    crit = SPELL_CRIT_MULTIPLIER if kind == 'spell' else CREATURE_CRIT_MULTIPLIER
    multiplier = {'miss': 0, 'dodge': 0, 'parry': 0, 'crit': crit, 'crush': 1.5}.get(outcome, 1)
    taken = damage * multiplier
    if not native:   # CalculateCritAmount's uint32(amount * multiplier); a crush adds half (CalculateMeleeDamage)
        taken = int(int(damage) * crit) if outcome == 'crit' else int(damage) + int(damage) // 2 if outcome == 'crush' else \
            int(taken)
    result.update(outcome=outcome, multiplier=multiplier)
    full_block = False
    if outcome == 'block':
        value = ch.block_value()
        result['block_value'] = value
        blocked = min(taken, value * block_health_factor() if native else value)
        full_block = blocked >= taken
        result.update(blocked=round(blocked, 2) if native else int(blocked), full_block=full_block)
        taken -= blocked
    result['damage'] = round(taken, 2) if native else int(taken)
    landed = outcome in ('hit', 'crit', 'crush') or (outcome == 'block' and not full_block)
    if not landed:
        _drop_procs(out)
    if ctx.get('sitting'):
        return out           # Unit::IsTriggeredAtSpellProcEvent: no proc while sitting
    flag = PROC_TAKE_HARMFUL_SPELL if kind == 'spell' else PROC_TAKE_RANGED if kind == 'ranged' else PROC_TAKE_MELEE
    if outcome == 'block':
        _block_procs(cat, ch, req, out, flag)
    if outcome == 'crit':
        _crit_procs(cat, ch, req, out, rng, flag)
    from service import paladin_talents
    paladin_talents.eye_for_an_eye(cat, ch, req, out, kind, outcome, result['damage'], native)
    return out


def block_health_factor() -> float:
    """WoW health -> Skyrim health of an incoming weapon hit: the Druid's white-hit conversion (skyrim_balance
    FERAL_MELEE: one Iron Sword hit, 7, per naked level-60 white hit, 96.5), so a block value takes the same share of a
    native hit that it would of a WoW one of the same scale."""
    from service.skyrim_balance import FERAL_MELEE
    return FERAL_MELEE.health_factor


def _drop_procs(out) -> None:
    for key in ('procs', 'consumed_auras', 'proc_chance'):
        out.pop(key, None)


def _proc_flags(cat, s) -> tuple[int, int]:
    row = next((r for r in cat.raw['spell_proc_event'] if r['entry'] in (s['Id'], cat.chain(s['Id']))), {})
    return row.get('procFlags') or s['ProcFlags'], row.get('procEx', 0)


def _block_procs(cat, ch, req, out, flag) -> None:
    """A block (PROC_EX_BLOCK): Holy Shield's SPELL_AURA_PROC_TRIGGER_DAMAGE hits the attacker
    (Unit::HandleProcTriggerDamageAuraProc, UnitAuraProcHandler.cpp:1585) and every block-proc holder with charges
    (Holy Shield, Redoubt's buff) drops one (UnitAuraProcHandler.cpp:505)."""
    from service import native_values
    charges = {}
    seen = set()
    rng = random.Random(f"{req.get('seed', 0)}:holy_shield")
    for a in ch.auras:
        sid = a['spell']
        if sid in seen or sid not in cat.spells:
            continue
        s = cat.spell(sid)
        flags, proc_ex = _proc_flags(cat, s)
        if not (flags & flag and proc_ex & PROC_EX_BLOCK):
            continue
        seen.add(sid)
        if s['ProcCharges']:
            charges[str(sid)] = 1
        for e in spells._effects(s):
            if s[f'EffectApplyAuraName{e}'] != A_PROC_TRIGGER_DAMAGE:
                continue
            # The proc is the aura's own spell: positive by target mode, so it cannot miss (Unit::SpellHitResult,
            # Unit.cpp:2916); binary (every effect an aura, SpellMgr.h:696), so no partial resist
            # (CalculateDamageAbsorbAndResist, Unit.cpp:2371); no crit (IsSpellAbleToCrit: no damage effect). Spell
            # power with its coefficient applies now (CalculateSpellDamage -> SpellDamageBonusDone), then the
            # attacker's Judgement of the Crusader through the same coefficient (SpellDamageBonusTaken; target.judgements).
            base, raw = spells._base_value(ch, s, e, rng)
            amount = paladin.taken_bonus(cat, ch, s, e, req['target'],
                                         spells._damage_done(ch, s, e, base, dot=False, raw_base=raw))
            event = ({'type': 'absorb', 'mana': 0} if native_values.absorbs(req['target'], s['School'], rng)
                     else {'type': 'damage', 'amount': amount, 'resisted': 0, 'crit': False, 'school': s['School']})
            from service import skyrim
            proc = {'source_spell': sid, 'spell': sid, 'target': 'target', 'cooldown_ms': 0, 'cost': 0,
                    'skill': skyrim.spell_skill(s, cat.balance), 'events': [event], 'auras': []}
            threat(ch, s, proc)
            out.setdefault('procs', []).append(proc)
    if charges:
        out['charges_used'] = {**out.get('charges_used', {}), **charges}


def _crit_procs(cat, ch, req, out, rng, flag) -> None:
    """A critical hit taken (PROC_EX_CRITICAL_HIT): Redoubt's passive casts its block buff on her (20128-20134: block
    chance for 10 s or ProcCharges blocks), Reckoning's its extra attack (20178, SPELL_EFFECT_ADD_EXTRA_ATTACKS: pending
    swings, at most 5, made at once on her next melee update - Unit::DoExtraAttacks, Unit.cpp:2680)."""
    seen = set()
    for a in ch.auras:
        sid = a['spell']
        if a['aura'] != A_PROC_TRIGGER_SPELL or sid in seen or sid not in cat.spells:
            continue
        s = cat.spell(sid)
        flags, proc_ex = _proc_flags(cat, s)
        trigger = s[f"EffectTriggerSpell{a['eff']}"]
        if not (flags & flag and proc_ex & PROC_EX_CRITICAL_HIT) or trigger not in cat.spells:
            continue
        seen.add(sid)
        chance = min(100.0, max(0.0, F.apply_spell_mod(s, 18, s['ProcChance'], ch.mods)))   # SPELLMOD_CHANCE_OF_SUCCESS
        if not F.roll_chance(rng, chance):
            continue
        ts = cat.spell(trigger)
        proc = {'source_spell': sid, 'spell': trigger, 'target': 'caster', 'cooldown_ms': 0, 'proc_chance': chance,
                'events': [], 'auras': []}
        for e in spells._effects(ts):
            value = int(F.effect_value(ts, e, ch.level, None))
            if ts[f'Effect{e}'] == 19:
                proc['events'].append({'type': 'extra_attacks', 'count': value, 'max_pending': MAX_EXTRA_ATTACKS})
            elif ts[f'EffectApplyAuraName{e}'] == A_MOD_BLOCK_PERCENT:
                proc['auras'].append({'spell': trigger, 'eff': e, 'aura': A_MOD_BLOCK_PERCENT, 'type': 'block_chance',
                                      'amount': value, 'duration_ms': cat.duration(ts, ch.level), 'amplitude_ms': 0,
                                      'charges': ts['ProcCharges'], 'stack': 'replace_same_caster'})
        out.setdefault('procs', []).append(proc)


# --- server hooks -------------------------------------------------------------------------------------------------

def check_swing(ch) -> None:
    """Unit::AttackerStateUpdate (Unit.cpp:2614): no swing while pacified (Divine Protection, Blessing of Protection)."""
    from service.melee import MeleeError
    if any(a['aura'] == A_PACIFY for a in ch.auras):
        raise MeleeError('ERR_ATTACK_PACIFIED')


def swing_timing(ch, out) -> dict:
    """Her attack time with SPELL_AURA_MOD_MELEE_HASTE (Unit::ApplyAttackTimePercentMod): Divine Shield doubles it.
    The Druid's swing timing stays as it always was (her melee ignores haste auras), so only other classes'."""
    if ch.cls == F.CLASS_DRUID or 'delay_ms' not in out:
        return out
    factor = 1.0
    for a in ch.auras:
        if a['aura'] == A_MELEE_HASTE:
            factor *= attack_time_multiplier(a['amount'])
    if factor != 1.0:
        out['delay_ms'] = int(out['delay_ms'] * factor)
    return out


def immune_damage(ch, school: int, defence: dict) -> dict:
    """damage_taken with a school: no damage of a school she is immune to (Unit::IsImmuneToDamage)."""
    if immune(ch, 1 << school):
        return {**defence, 'damage': 0, 'resisted': 0, 'immune': True}
    return defence


def threat(ch, s, out) -> dict:
    """Unit::ApplyTotalThreatModifier (Unit.cpp:8637): her threat in the spell's school times every
    SPELL_AURA_MOD_THREAT on it (Righteous Fury: Holy x1.6, x1.9 with Improved Righteous Fury), on top of the
    SPELLMOD_THREAT multiplier. Healing threat uses it too (threatAssist). A Paladin's only; the Druid's form threat
    stays feral's."""
    if ch.cls != F.CLASS_PALADIN:
        return out
    factor = 1.0
    for a in ch.auras:
        if a['aura'] == A_THREAT and a['misc'] & (1 << s['School']):
            factor *= (100.0 + max(a['amount'], -99.99)) / 100.0
    if factor != 1.0:
        out['threat_multiplier'] = out.get('threat_multiplier', 1.0) * factor
    return out
