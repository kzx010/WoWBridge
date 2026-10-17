"""The Paladin's code-driven spells as CMaNGOS runs them; data/paladin_manifest.json holds every link with its evidence.

Holy Light and Flash of Light heal through code-linked spells (19968, 19993); Lay on Hands heals for the caster's
maximum health at the price of all her mana; a seal (one at a time) procs on her white swings; Judgement consumes the
seal and casts its rank's judgement on the target. Skyrim targets carry no WoW auras: the plugin keeps the active seal
in caster.auras and this caster's judgements on a unit in target.judgements, which bring back the effects CMaNGOS
reads from the units' auras (Judgement of the Crusader's holy damage taken, Light and Wisdom's procs).
"""
import json
import math
import random
from functools import lru_cache
from pathlib import Path

from service import formulas as F, native_values
from service.progression import number

MANIFEST = Path(__file__).resolve().parent.parent / 'data/paladin_manifest.json'
JUDGEMENT = 20271
SPELLFAMILY_PALADIN = 10
HOLY_MASK = 1 << 1                                     # SPELL_SCHOOL_MASK_HOLY
# SpellAuraDefines.h / SharedDefines.h
A_DUMMY, A_MOD_ATTACKSPEED, A_MOD_DAMAGE_TAKEN, A_PROC_TRIGGER_SPELL, A_PREVENTS_FLEEING = 4, 9, 14, 42, 92
E_SCHOOL_DAMAGE, E_DUMMY, E_HEAL, E_ENERGIZE, E_WEAPON_PERCENT_DAMAGE, E_HEAL_MAX_HEALTH, E_SCRIPT_EFFECT = \
    2, 3, 10, 30, 31, 67, 77
DMG_NONE, DMG_MAGIC, DMG_MELEE, DMG_RANGED = 0, 1, 2, 3
ATTR_NO_ACTIVE_DEFENSE, ATTR_EX_USE_ALL_MANA, ATTR_EX2_CANT_CRIT, ATTR_EX3_COMPLETELY_BLOCKED = 0x200000, 0x2, 0x20000000, 0x8
ATTR_IS_ABILITY, ATTR_EX2_AUTO_REPEAT, ATTR_EX3_NOT_A_PROC = 0x10, 0x20, 0x200
ATTR_EX3_SUPPRESS_TARGET_PROCS, ATTR_EX3_CAN_PROC_FROM_PROCS = 0x20000, 0x4000000
# SpellMgr.h ProcFlags, the victim's side
PROC_TAKE_MELEE_SWING, PROC_TAKE_MELEE_ABILITY, PROC_TAKE_RANGED_ATTACK, PROC_TAKE_RANGED_ABILITY = 0x8, 0x20, 0x80, 0x200
PROC_TAKE_HARMFUL_ABILITY, PROC_TAKE_HARMFUL_SPELL = 0x2000, 0x20000
INVTYPE_2HWEAPON, BASE_ATTACK_TIME = 17, 2000
SEAL_OF_RIGHTEOUSNESS_FLAGS = 0x8000000               # UnitAuraProcHandler.cpp:1094 (the dummy proc's family mask)
# SpellAuras.cpp:1561-1570 (HandleAuraDummy): every Seal of the Crusader rank scales the main hand's base damage
# (m_modAttackBaseDPSPct) by 100 / (100 + 40).
CRUSADER_BASE_DPS = 40
LANDED = ('hit', 'crit', 'glance', 'block')            # a white swing that dealt damage: PROC_EX_NORMAL/CRITICAL_HIT


@lru_cache(maxsize=1)
def manifest():
    return json.loads(MANIFEST.read_text())


@lru_cache(maxsize=1)
def links():
    """kind -> from -> [to] of the manifest's links (a Holy Shock rank has two 'dummy' targets)."""
    out = {}
    for row in manifest()['links']:
        out.setdefault(row['kind'], {}).setdefault(row['from'], []).append(row['to'])
    return out


def _link(kind, sid):
    return links().get(kind, {}).get(sid, [None])[0]


@lru_cache(maxsize=1)
def groups():
    """SpellStacking group name -> (rule, spells): spell_group 5 Seal (UNIQUE), 6 Judgement (UNIQUE_PER_CASTER)."""
    return {g['name']: (g['rule'], frozenset(g['spells'])) for g in manifest()['stacking_groups'].values()}


def seals():
    return groups()['Seal'][1]


def judgements():
    return groups()['Judgement'][1]


def is_seal(sid):
    return sid in seals()


def script_heal(sid):
    """SpellEffects.cpp:Spell::EffectScriptEffect (4152-4170): the Holy Light (icon 70) and Flash of Light (icon 242)
    ranks' script effect casts 19968 or 19993 (manifest 'script_effect' links)."""
    return _link('script_effect', sid)


def handles(s):
    return s['SpellFamilyName'] == SPELLFAMILY_PALADIN and (
        s['Id'] == JUDGEMENT or is_seal(s['Id']) or script_heal(s['Id']) is not None or
        any(s[f'Effect{e}'] == E_HEAL_MAX_HEALTH for e in (1, 2, 3)))


def _require(cat, *sids):
    """A catalog built before the Paladin manifest (build/ may be stale) lacks the code-linked spells."""
    from service.spells import CastError
    if any(sid not in cat.spells for sid in sids):
        raise CastError('SPELL_FAILED_SPELL_UNAVAILABLE')


# --- shared CMaNGOS rules ---------------------------------------------------------------------------------------

def able_to_crit(s):
    """SpellMgr.h:IsSpellAbleToCrit / IsSpellEffectAbleToCrit: damage, heal and weapon effects, and a class spell's mana
    energize (Lay on Hands ranks 2-3); never with SPELL_ATTR_EX2_CANT_CRIT."""
    if s['AttributesEx2'] & ATTR_EX2_CANT_CRIT:
        return False
    for e in (1, 2, 3):
        effect = s[f'Effect{e}']
        if effect in (E_SCHOOL_DAMAGE, E_HEAL, E_WEAPON_PERCENT_DAMAGE, 17, 58, 121):
            return True
        if effect == E_ENERGIZE and s[f'EffectMiscValue{e}'] == 0 and s['SpellFamilyName'] and s['DmgClass']:
            return True
    return False


def crit_chance(ch, s, target=None):
    """Unit::CalculateSpellCritChance (Unit.cpp:3828) for a player: magic spells the school's spell crit, melee and
    ranged ones the attack table's (CalculateEffectiveCritChance), none others; SPELLMOD_CRITICAL_CHANCE; 0 under
    0.005 and for a spell that cannot crit (Unit::CanCrit)."""
    if not able_to_crit(s):
        return 0.0
    if s['DmgClass'] == DMG_MELEE:
        from service import melee
        return max(0.0, min(F.apply_spell_mod(s, F.SPELLMOD_CRITICAL_CHANCE, melee.chances(ch, target)['crit'],
                                              ch.mods), 100.0))
    chance = F.apply_spell_mod(s, F.SPELLMOD_CRITICAL_CHANCE,
                               ch.spell_crit(s['School']) if s['DmgClass'] == DMG_MAGIC else 0.0, ch.mods)
    return 0.0 if chance < 0.005 else max(0.0, min(chance, 100.0))


def crit_multiplier(ch, s):
    """Unit::GetCritMultiplier (Unit.cpp:3560): x1.5 for magic and none, x2 otherwise, the bonus part through
    SPELLMOD_CRIT_DAMAGE_BONUS."""
    base = 1.5 if s['DmgClass'] in (DMG_NONE, DMG_MAGIC) else 2.0
    return max(0.0, 1 + F.apply_spell_mod(s, F.SPELLMOD_CRIT_DAMAGE_BONUS, base - 1, ch.mods))


def crit_amount(ch, s, amount):
    """Unit::CalculateCritAmount (Unit.cpp:3600): uint32(amount x multiplier), never less than the plain amount."""
    return max(amount, int(amount * crit_multiplier(ch, s)))


def _value(ch, s, eff, rng, base_points=None):
    """CalculateSpellEffectValue with the spell's SPELLMOD_ALL_EFFECTS: (modified, rolled)."""
    rolled = F.effect_value(s, eff, ch.level, rng, base_points=base_points)
    return int(F.apply_spell_mod(s, F.SPELLMOD_ALL_EFFECTS, rolled, ch.mods, eff=eff)), int(rolled)


def target_judgements(cat, target):
    """This caster's judgements on the unit (the plugin's record of the auras Judgement put there)."""
    ids = target.get('judgements', [])
    if not isinstance(ids, list) or any(isinstance(i, bool) or not isinstance(i, int) or i not in judgements()
                                        or i not in cat.spells for i in ids):
        raise ValueError('target.judgements must be an array of judgement spell IDs')
    return ids


def damage_taken(cat, ch, target, school_mask):
    """Unit::SpellBaseDamageBonusTaken (Unit.cpp:7174) / GetTotalAuraModifierByMiscMask: SPELL_AURA_MOD_DAMAGE_TAKEN of
    these schools on the target - her judgements there (Judgement of the Crusader: holy)."""
    total = 0
    for sid in target_judgements(cat, target):
        j = cat.spell(sid)
        for e in (1, 2, 3):
            if j[f'EffectApplyAuraName{e}'] == A_MOD_DAMAGE_TAKEN and j[f'EffectMiscValue{e}'] & school_mask:
                total += int(F.effect_value(j, e, ch.level, None, mods=ch.mods))
    return total


def holy_taken(cat, ch, target):
    """The holy school's SPELL_AURA_MOD_DAMAGE_TAKEN on the target, i.e. Judgement of the Crusader."""
    return damage_taken(cat, ch, target, HOLY_MASK)


def taken_bonus(cat, ch, s, eff, target, dmg):
    """Her damage on a unit carrying her judgements (target.judgements): Unit::SpellDamageBonusTaken (Unit.cpp:7112;
    damage class magic or none, direct or periodic) and Unit::MeleeDamageBonusTaken (Unit.cpp:7611; melee and ranged,
    nothing on 0 damage) add the target's SPELL_AURA_MOD_DAMAGE_TAKEN of the spell's school - Judgement of the
    Crusader's holy damage taken - through the spell's coefficient and level penalty (SpellBonusWithCoeffs, donePart
    false; a non-physical spell is always scaled), after her done bonuses and before the crit. With caster.skyrim a
    lower rank's addition converts at the top rank's rate, as her gear and talent additions do
    (spells._rank_health_bonus). Another class's cast, or a target without judgements (every Druid request), is
    returned unchanged."""
    if ch.cls != F.CLASS_PALADIN or 'judgements' not in target or target.get('kind', 'unit') != 'unit':
        return dmg
    flat = damage_taken(cat, ch, target, 1 << s['School'])
    if not flat or (s['DmgClass'] in (DMG_MELEE, DMG_RANGED) and dmg <= 0):
        return dmg
    bonus = F.bonus_with_coeffs(s, eff, 0, flat, ch.mods)
    return max(0, dmg + bonus * ch.health_bonus_scales.get(s['Id'], 1))


def _mitigated(ch, s, target, amount, crit, rng):
    """Spell::DoAllEffectOnTarget (Spell.cpp:1258) after the crit: holy damage skips armor; CalculateAbsorbResistBlock's
    partial resist (Unit.cpp:2604), with the bridge's native absorption first as for every harmful spell."""
    from service import spells
    if native_values.absorbs(target, s['School'], rng):
        return {'type': 'absorb', 'mana': 0, 'school': s['School']}
    resisted = int(amount * F.partial_resist_multiplier(spells._resist_pct(ch, s, target, False), False, False, True, rng))
    return {'type': 'damage', 'amount': amount - resisted, 'resisted': resisted, 'crit': crit, 'school': s['School'],
            'spell': s['Id']}


def _holy_damage(cat, ch, s, target, value, raw, rng, block=False):
    """A holy damage spell of hers on a unit: Spell::CalculateSpellEffectDamage (Spell.cpp:6280) - SpellDamageBonusDone
    for magic, MeleeDamageBonusDone for melee (the same coefficient, level penalty, damage-done percent and
    SPELLMOD_DAMAGE for a non-weapon spell, spells._damage_done) - then the target's Judgement of the Crusader through
    the spell's coefficient (SpellDamageBonusTaken / MeleeDamageBonusTaken), the crit, a melee spell's partial block
    (Unit::RollAbilityPartialBlockOutcome), resist."""
    from service import spells, melee
    eff = next(e for e in (1, 2, 3) if s[f'Effect{e}'] in (E_SCHOOL_DAMAGE, E_WEAPON_PERCENT_DAMAGE))
    if s[f'Effect{eff}'] == E_SCHOOL_DAMAGE:
        dmg = spells._damage_done(ch, s, eff, value, dot=False, raw_base=raw)
    else:
        # Unit::MeleeDamageBonusDone (Unit.cpp:7439) for a weapon-based holy spell: the holy spell damage through the
        # coefficient (SpellBonusWithCoeffs); its weapon damage already carries the percent auras.
        bonus = F.bonus_with_coeffs(s, eff, 0, ch.spell_damage(HOLY_MASK), ch.mods)
        dmg = max(0, int(F.apply_spell_mod(s, F.SPELLMOD_DAMAGE, value + bonus, ch.mods)))
    dmg = taken_bonus(cat, ch, s, eff, target, dmg)
    chance = crit_chance(ch, s, target)
    crit = F.roll_chance(rng, chance)
    if crit:
        dmg = crit_amount(ch, s, dmg)
    blocked = 0
    if block and s['DmgClass'] == DMG_MELEE and not s['Attributes'] & ATTR_NO_ACTIVE_DEFENSE \
            and not s['AttributesEx3'] & ATTR_EX3_COMPLETELY_BLOCKED and F.roll_chance(rng, melee.chances(ch, target)['block']):
        blocked = min(dmg, int(target['level']) // 2 + int(cat.creature_strength(int(target['level'])) / 20))
    event = _mitigated(ch, s, target, dmg - blocked, crit, rng)
    if blocked:
        event['blocked'] = blocked
    return event, chance


# --- heals ---------------------------------------------------------------------------------------------------

def linked_heal(cat, ch, rank, linked, value, raw, rng, taken=0):
    """Spell::EffectScriptEffect (SpellEffects.cpp:4152-4170): the rank's rolled value (its SPELLMOD_ALL_EFFECTS
    included) is the custom basepoint of the code-linked heal, cast instantly (SpellMgr.cpp:168 drops 19968's own cast
    time). That spell heals (Spell::EffectHeal) with its own coefficient, level penalty (19968 is level 1: x0.2875 for
    every Holy Light rank, as CMaNGOS computes it) and SPELLMOD_DAMAGE (Unit::SpellHealingBonusDone), and rolls its own
    crit (Spell::HandleImmediateEffectExecution -> Unit::RollSpellCritOutcome). Native lower ranks scale the bonus at
    the rank's top-rank conversion (spells._rank_health_bonus by the rank). taken: the healed unit's flat bonus
    (Unit::SpellHealingBonusTaken after the done part, before the crit: Blessing of Light, paladin_auras.light_bonus),
    an addition like her gear's, so a native lower rank converts it at the top rank's rate too (no downrank gain)."""
    from service import spells
    h = cat.spell(linked)
    base, _ = _value(ch, h, 1, rng, base_points=value)
    heal = spells._healing_done(ch, cat, h, 1, base, dot=False, raw_base=raw, rank=rank)
    if taken:
        heal += taken * ch.health_bonus_scales.get(rank['Id'], 1)
    if rng is None:
        return heal, 0.0, False
    chance = crit_chance(ch, h)
    crit = F.roll_chance(rng, chance)
    return (crit_amount(ch, h, int(heal)) if crit else heal), chance, crit


def _script_heal(cat, ch, s, out, rng, req):
    from service import paladin_auras
    linked = script_heal(s['Id'])
    _require(cat, linked)
    value, raw = _value(ch, s, 1, rng)
    taken, blessing = paladin_auras.light_bonus(cat, ch, req, linked)
    heal, chance, crit = linked_heal(cat, ch, s, linked, value, raw, rng, taken)
    out['events'].append({'type': 'heal', 'amount': heal, 'crit': crit, 'school': s['School'], 'spell': linked,
                          **({'taken_bonus_wow': taken, 'taken_spell': blessing} if blessing else {})})
    out['crit_chance'] = chance
    out['linked_spell'] = linked


def _lay_on_hands(cat, ch, s, out, rng):
    """Spell::EffectHealMaxHealth (SpellEffects.cpp:3576): the target heals for the caster's maximum health, straight
    into m_healing - no healing bonus. Spell::CalculatePowerCost (Spell.cpp:6318): SPELL_ATTR_EX_USE_ALL_MANA costs all
    her remaining mana (use_all_mana; cost 0 here). Ranks 2-3's mana energize to the target is spells.resolve's power
    event, and makes the spell able to crit (IsSpellEffectAbleToCrit): Spell::DoAllEffectOnTarget multiplies the heal
    by the heal crit multiplier (Unit::CalculateCritAmount). No Forbearance: Spell::prepare (Spell.cpp:3215-3245) adds
    25771 only for MECHANIC_INVULNERABILITY spells (Divine Shield, Divine Protection, Blessing of Protection)."""
    chance = crit_chance(ch, s)
    crit = F.roll_chance(rng, chance)
    multiplier = crit_multiplier(ch, s) if crit else 1.0
    heal = crit_amount(ch, s, ch.max_health) if crit else ch.max_health
    # caster_max_health: the WoW rule (her maximum health times this) for a plugin that applies her native maximum.
    out['events'].insert(0, {'type': 'heal', 'amount': heal, 'crit': crit, 'school': s['School'],
                             'caster_max_health': multiplier})
    out['crit_chance'] = chance
    if s['AttributesEx'] & ATTR_EX_USE_ALL_MANA:
        out['use_all_mana'] = True


# --- seals ---------------------------------------------------------------------------------------------------

def proc_event(cat, sid):
    """spell_proc_event, the first rank's row standing for the chain (SpellMgr::LoadSpellProcEvents); Cooldown is in
    seconds in this dump (sql/updates/mangos/z2829_01_mangos_proc_cooldown.sql makes it the code's milliseconds)."""
    rows = {r['entry']: r for r in cat.raw['spell_proc_event']}
    return rows.get(sid, rows.get(cat.chain(sid), {}))


def seal_kind(cat, s):
    if s['SpellFamilyFlags'] & SEAL_OF_RIGHTEOUSNESS_FLAGS and s['EffectApplyAuraName1'] == A_DUMMY:
        return 'righteousness'
    if any(s[f'EffectApplyAuraName{e}'] == A_PROC_TRIGGER_SPELL for e in (1, 2, 3)):
        return 'proc'
    return 'passive'


def seal_payloads(s):
    """What a seal's procs cast: Righteousness's dummy-proc payload, the others' PROC_TRIGGER_SPELL."""
    return [x for x in [_link('dummy_proc', s['Id'])] if x] + \
        [s[f'EffectTriggerSpell{e}'] for e in (1, 2, 3) if s[f'EffectApplyAuraName{e}'] == A_PROC_TRIGGER_SPELL]


def seal_judgement(s):
    """spell_judgement (Paladin.cpp:77-116): a seal's effect 2 dummy aura carries its judgement in its base points
    (SpellEntry::CalculateSimpleValue, manifest 'judgement' links); the starting Seal of Righteousness has none."""
    return _link('judgement', s['Id'])


def _seal(cat, ch, s, out):
    """A seal is a 30-second self aura in SpellStacking group Seal (spell_group 5, UNIQUE): a new seal replaces the one
    she has (Unit::AddSpellAuraHolder removes the existing holder). The plugin keeps its id in caster.auras while it
    lasts; melee and Judgement read it there. Each aura names its role; the attack power (99) and haste (9) auras
    are Seal of the Crusader's, whose main-hand damage reductions the WoW swing applies (melee.roll_range)."""
    rule, _ = groups()['Seal']
    out.update(stack='Seal', stack_rule=rule)
    out.pop('stack_amounts', None)
    kind = seal_kind(cat, s)
    out['seal'] = {'chain': cat.chain(s['Id']), 'kind': kind, 'judgement': seal_judgement(s)}
    for a in out['auras']:
        if a['aura'] == A_DUMMY and a['eff'] == 3:
            a.pop('amount', None)
            a.update(type='seal_judgement', judgement=seal_judgement(s))
        elif a['aura'] == A_DUMMY and kind == 'righteousness':
            a.update(type='seal_proc', proc='righteousness', payload=_link('dummy_proc', s['Id']))
        elif a['aura'] == A_PROC_TRIGGER_SPELL:
            row = proc_event(cat, s['Id'])
            a.pop('trigger_spell', None)
            a.update(type='seal_proc', proc='trigger', payload=s[f"EffectTriggerSpell{a['eff']}"],
                     proc_flags=row.get('procFlags') or s['ProcFlags'], ppm=row.get('ppmRate', 0),
                     proc_chance=s['ProcChance'], proc_cooldown_ms=int(row.get('Cooldown', 0) * 1000))
        elif a['aura'] == A_MOD_ATTACKSPEED:
            # Aura::HandleModAttackSpeed: the main hand's swing time only (the plugin's melee_haste convention).
            a.update(type='melee_haste', main_hand_only=True)


def active_seal(cat, caster):
    """Her seal: the seal id in caster.auras (character.build refuses two; the plugin replaces it on each new seal)."""
    return next((sid for sid in caster.get('auras', []) if not isinstance(sid, bool) and isinstance(sid, int)
                 and is_seal(sid) and sid in cat.spells), None)


def crusader_pcts(ch):
    """Seal of the Crusader on her main hand: (base weapon damage pct, total damage pct). HandleAuraDummy
    (SpellAuras.cpp:1561-1570) divides m_modAttackBaseDPSPct by 1.4; SealOfTheCrusader::OnApply (Paladin.cpp:25-34)
    lowers UNIT_MOD_DAMAGE_MAINHAND TOTAL_PCT by 100 x haste / (haste + 100) percent, CMaNGOS's float arithmetic."""
    crusader = [a for a in ch.auras if a['aura'] == A_MOD_ATTACKSPEED and _is_crusader(a['spell'])]
    if not crusader:
        return 1.0, 1.0
    base = F.f32(100.0 / F.f32(100.0 + CRUSADER_BASE_DPS))
    total = 1.0
    for a in crusader:
        amount = float(a['amount'])
        reduction = F.f32(F.f32(-100.0 * amount) / F.f32(amount + 100.0))
        total = F.f32(total * F.f32(F.f32(100.0 + reduction) / 100.0))
    return base, total


@lru_cache(maxsize=1)
def _crusader():
    """Seal of the Crusader's ranks and Judgements (the manifest handler citing SpellAuras.cpp:1571)."""
    return frozenset(sid for h in manifest()['handlers'] if h['behaviour'].startswith('Seal of the Crusader')
                     for sid in h['spells'])


def _is_crusader(sid):
    return sid in _crusader()


def righteousness_damage(cat, ch, seal, target):
    """Unit::HandleDummyAuraProc (UnitAuraProcHandler.cpp:1094-1137), Seal of Righteousness on a melee hit: from the
    seal's aura amount (effect 1) and the main hand's speed (2 s bare-handed), a two-hander deals 1.2 x amount x 1.2 x
    1.03 x speed / 100 + 1 (coefficient .108 x speed), anything else 0.85 x ceil(amount x 1.2 x 1.03 x speed / 100) - 1
    (.092 x speed); plus 3% of the weapon's base damage, plus her holy spell damage and the target's holy damage taken
    through that coefficient and the seal's level penalty. CMaNGOS's float arithmetic; int32 truncations."""
    s = cat.spell(seal)
    from service.paladin_talents import righteousness_amount   # a labelled custom rule for Improved Seal of Righteousness
    amount = righteousness_amount(ch, s, int(F.effect_value(s, 1, ch.level, None, mods=ch.mods)))
    item = cat.item(ch.weapon_entry) if ch.weapon_entry else None
    speed = F.f32((item['delay'] if item else BASE_ATTACK_TIME) / 1000.0)
    if item and item['InventoryType'] == INVTYPE_2HWEAPON:
        base = F.f32(F.f32(F.f32(F.f32(F.f32(F.f32(F.f32(1.2) * amount) * F.f32(1.2)) * F.f32(1.03)) * speed) / 100.0) + 1)
        coeff = F.f32(F.f32(.108) * speed)
    else:
        scaled = F.f32(F.f32(F.f32(F.f32(F.f32(amount * F.f32(1.2)) * F.f32(1.03)) * speed) / 100.0))
        base = F.f32(F.f32(F.f32(0.85) * math.ceil(scaled)) - 1)
        coeff = F.f32(F.f32(.092) * speed)
    w = ch.weapon
    weapon = F.f32(F.f32(F.f32(0.03) * F.f32(F.f32(w['min']) + F.f32(w['max']))) / 2.0)
    point = int(F.f32(base + weapon)) + 1
    if point >= 0:
        bonus = ch.spell_damage(HOLY_MASK) + holy_taken(cat, ch, target)
        # Unit::CalculateLevelPenalty in single precision: 1 - (20 - level) x 0.0375 below level 20.
        level = s['SpellLevel']
        penalty = F.f32(1.0 - F.f32(F.f32(20.0 - level) * F.f32(0.0375))) if 0 < level < 20 else 1.0
        point = int(F.f32(point + F.f32(F.f32(bonus * coeff) * penalty)))
    return point


def _seal_procs(cat, ch, req, out, seal, rng):
    from service import melee, spells
    caster, t = req.get('caster', {}), {**req['target'], 'kind': 'unit'}
    s = cat.spell(seal)
    kind = seal_kind(cat, s)
    if kind == 'righteousness':
        payload = cat.spell(_link('dummy_proc', seal))
        value, raw = _value(ch, payload, 1, rng, base_points=righteousness_damage(cat, ch, seal, t))
        # The payload (CastCustomSpell): SPELL_ATTR_EX3_ALWAYS_HIT, SPELL_ATTR_EX2_CANT_CRIT, no active defense.
        event, _ = _holy_damage(cat, ch, payload, t, max(0, value), raw, rng)
        return [{'source_spell': seal, 'spell': payload['Id'], 'target': 'target', 'cooldown_ms': 0, 'school': 1,
                 'events': [event], 'auras': []}]
    if kind != 'proc':
        return []
    e = next(e for e in (1, 2, 3) if s[f'EffectApplyAuraName{e}'] == A_PROC_TRIGGER_SPELL)
    row = proc_event(cat, seal)
    cooldowns = caster.get('proc_cooldowns', {})
    if not isinstance(cooldowns, dict):
        raise ValueError('caster.proc_cooldowns must be an object')
    if not (row.get('procFlags') or s['ProcFlags']) & 0x4:
        return []
    if number(cooldowns.get(str(seal), 0), 'caster.proc_cooldowns value') > 0:
        return []
    # Unit::IsTriggeredAtSpellProcEvent (UnitAuraProcHandler.cpp:592-604): ppmRate x the weapon's own attack time
    # (GetAttackTime, unhasted) / 600, else the spell's ProcChance; SPELLMOD_CHANCE_OF_SUCCESS.
    chance = row.get('CustomChance') or s['ProcChance']
    if row.get('ppmRate'):
        chance = F.f32(F.f32(ch.melee_weapon()['delay'] * F.f32(row['ppmRate'])) / 600.0)
    chance = F.apply_spell_mod(s, 18, chance, ch.mods)
    out['seal_proc_chance'] = chance
    if not F.roll_chance(rng, chance):
        return []
    p = cat.spell(s[f'EffectTriggerSpell{e}'])
    proc = {'source_spell': seal, 'spell': p['Id'], 'cooldown_ms': int(row.get('Cooldown', 0) * 1000),
            'school': p['School'], 'events': [], 'auras': []}
    effect = p['Effect1']
    if effect == E_WEAPON_PERCENT_DAMAGE:
        # Seal of Command's 20424: a melee strike (Unit::MeleeSpellHitResult; melee.ability_outcome) of 70% of her
        # weapon roll (Spell::EffectWeaponDmg: int32(CalculateDamage x pct / 100)).
        proc['target'] = 'target'
        side = melee.ability_outcome(ch, p, t, rng)
        if side != 'hit':
            proc['events'].append({'type': side, 'spell': p['Id']})
        else:
            pct, _ = _value(ch, p, 1, rng)
            low, high, _ = melee.roll_range(ch, ch.melee_weapon())
            weapon = int(F.f32(rng.randint(int(low), int(high)) * F.f32(pct / 100.0)))
            event, _ = _holy_damage(cat, ch, p, t, weapon, weapon, rng, block=True)
            proc['events'].append(event)
    elif effect == E_HEAL:
        # Seal of Light: a self heal with DmgClass none - no healing bonus (SpellHealingBonusDone returns it), and
        # SPELL_ATTR_EX2_CANT_CRIT.
        proc['target'] = 'caster'
        proc['events'].append(_flat_heal(ch, p, rng))
    elif effect == E_ENERGIZE:
        proc['target'] = 'caster'
        proc['events'].append({'type': 'power', 'power': 'mana', 'amount': _value(ch, p, 1, rng)[0]})
    else:
        # Seal of Justice's stun: its own magic hit roll and aura (spells.resolve, triggered).
        result = spells.resolve(cat, ch, {**req, 'spell': p['Id'], 'target': t, 'seed': f"{req.get('seed', 0)}:seal"},
                                feral_dispatch=False, triggered=True)
        proc.update(target='target', events=result['events'], auras=result['auras'])
    return [proc]


def _flat_heal(ch, s, rng):
    amount = _value(ch, s, 1, rng)[0]
    crit = s['DmgClass'] != DMG_NONE and F.roll_chance(rng, crit_chance(ch, s))
    return {'type': 'heal', 'amount': crit_amount(ch, s, amount) if crit else amount, 'crit': crit,
            'school': s['School'], 'spell': s['Id']}


def victim_proc_flag(s):
    """Spell::PrepareMasksForProcSystem (Spell.cpp:808-884): the victim's proc flag of a harmful spell's hit - by
    DmgClass for melee (TAKE_MELEE_ABILITY) and ranged (TAKE_RANGED_ABILITY; an auto-repeat shot TAKE_RANGED_ATTACK),
    else TAKE_HARMFUL_ABILITY with SPELL_ATTR_IS_ABILITY or TAKE_HARMFUL_SPELL; none with
    SPELL_ATTR_EX3_SUPPRESS_TARGET_PROCS."""
    if s['AttributesEx3'] & ATTR_EX3_SUPPRESS_TARGET_PROCS:
        return 0
    if s['AttributesEx2'] & ATTR_EX2_AUTO_REPEAT:
        return PROC_TAKE_RANGED_ATTACK
    if s['DmgClass'] == DMG_MELEE:
        return PROC_TAKE_MELEE_ABILITY
    if s['DmgClass'] == DMG_RANGED:
        return PROC_TAKE_RANGED_ABILITY
    return PROC_TAKE_HARMFUL_ABILITY if s['Attributes'] & ATTR_IS_ABILITY else PROC_TAKE_HARMFUL_SPELL


def _judgement_procs(cat, ch, judged, flag, rng, hit=None, triggered=False):
    """The judged unit's Judgement of Light / Wisdom for her, on one landed damaging hit she made (Unit::
    ProcDamageAndSpell on the victim; Unit::IsTriggeredAtSpellProcEvent, UnitAuraProcHandler.cpp:514-684): the hit's
    victim flag in the aura's ProcFlags (Light 0x28: melee swings and melee abilities; Wisdom 0x222a8: also ranged
    attacks and abilities and harmful spells and abilities), no spell_proc_event row (procEx none: a damaging hit or
    crit, Aura::CanProcFrom), a triggered spell only with SPELL_ATTR_EX3_NOT_A_PROC (or CAN_PROC_FROM_PROCS on the
    aura), then the aura's ProcChance (the victim's: no PPM, no spell mods of hers). Paladin.cpp:36-75: the judged
    unit's 5373 / 1826 makes the attacker cast the rank's payload on herself - a heal or a mana energize, DmgClass
    none and SPELL_ATTR_EX2_CANT_CRIT. hit: the spell that struck (None for a white swing)."""
    procs = []
    for sid in judged:
        j = cat.spell(sid)
        payload = _link('script', sid)
        if payload is None or not j['ProcFlags'] & flag:
            continue
        if triggered and not (hit['AttributesEx3'] & ATTR_EX3_NOT_A_PROC or
                              j['AttributesEx3'] & ATTR_EX3_CAN_PROC_FROM_PROCS):
            continue
        if not F.roll_chance(rng, j['ProcChance']):
            continue
        if payload not in cat.spells:
            continue
        p = cat.spell(payload)
        event = _flat_heal(ch, p, rng) if p['Effect1'] == E_HEAL else \
            {'type': 'power', 'power': 'mana', 'amount': _value(ch, p, 1, rng)[0]}
        procs.append({'source_spell': sid, 'spell': p['Id'], 'target': 'caster', 'cooldown_ms': 0,
                      'school': p['School'], 'events': [event], 'auras': [],
                      **({'from_spell': hit['Id']} if hit is not None else {})})
    return procs


def _damaging(events):
    """A landed hit that dealt damage (Aura::CanProcFrom's 'damaging': data.damage != 0)."""
    return any(e.get('type') == 'damage' and e.get('amount', 0) > 0 for e in events)


def judged_hit(cat, attacker, req):
    """A follower's landed damaging hit on a unit carrying her judgements (op judged_hit): Paladin.cpp:36-75 - the judged
    unit's Judgement of Light / Wisdom makes whoever strikes it cast the rank's payload on itself (a heal or mana), by
    the hit's victim flag and the aura's ProcChance (_judgement_procs). attacker: the follower's level (no spell mods
    of hers apply to its payload). attack.kind: melee (a swing), ranged, or spell."""
    attack = req.get('attack', {})
    flags = {'melee': PROC_TAKE_MELEE_SWING, 'ranged': PROC_TAKE_RANGED_ATTACK, 'spell': PROC_TAKE_HARMFUL_SPELL}
    if attack.get('kind', 'melee') not in flags:
        raise ValueError("attack.kind must be 'melee', 'ranged' or 'spell'")
    judged = target_judgements(cat, req.get('target', {}))
    if attack.get('damage', 0) <= 0:
        return {'procs': []}
    rng = random.Random(f"{req.get('seed', 0)}:judged_hit")
    procs = _judgement_procs(cat, attacker, judged, flags[attack.get('kind', 'melee')], rng)
    for proc in procs:
        proc['target'] = 'attacker'   # the follower that struck, not her
    return {'procs': procs}


def spell_procs(cat, ch, req, out):
    """server resolve, last: the judged units' Light / Wisdom procs for her harmful spells and abilities that dealt
    damage (Spell::DoAllEffectOnTarget -> Unit::ProcDamageAndSpell with the hit's victim flag, Spell.cpp:1318): the
    cast's own hit on its unit target (Exorcism, Hammer of Wrath), each Holy Wrath victim (req.targets, by id), and
    what her casts cast on the target - a judgement's strike (Righteousness: not triggered, Paladin.cpp:109; Command's
    damage rank: triggered by its dummy, SPELL_ATTR_EX3_NOT_A_PROC) and Holy Shock's strike (triggered, NOT_A_PROC).
    Periodic damage (Consecration: TAKE_HARMFUL_PERIODIC) is outside both judgements' flags; Holy Shield's block damage
    raises no victim procs (HandleProcTriggerDamageAuraProc deals it directly). A Paladin's only."""
    if ch.cls != F.CLASS_PALADIN:
        return out
    from service import spells
    from service.paladin_talents import _hits
    s, t = cat.spell(req['spell']), req['target']
    hits = []
    if t.get('kind') == 'unit' and not spells.is_positive(s):
        hits.append((s, out.get('events', []), False, t, None))
    if out.get('results'):
        victims = {v['id']: v for v in req.get('targets', []) if isinstance(v, dict) and 'id' in v}
        for r in out['results']:
            if r.get('id') in victims:
                hits.append((s, [r], False, {**victims[r['id']], 'kind': 'unit'}, r['id']))
    hits += [(hit, events, triggered, {**t, 'kind': 'unit'}, None)
             for hit, events, triggered in _hits(cat, out.get('procs', []))]
    rng = random.Random(f"{req.get('seed', 0)}:judgement_procs")
    new = []
    for hit, events, triggered, target, victim in hits:
        if 'judgements' not in target or not _damaging(events):
            continue
        for proc in _judgement_procs(cat, ch, target_judgements(cat, target), victim_proc_flag(hit), rng, hit,
                                     triggered):
            new.append({**proc, 'victim': victim} if victim is not None else proc)
    if new:
        out['procs'] = out.get('procs', []) + new
    return out


def melee_procs(cat, ch, req, out):
    """Her white swing (melee op), after it lands: her seal's proc (Seal of Righteousness each hit; Command, Light,
    Wisdom and Justice by their proc event), the judged target's Light/Wisdom procs for her (the victim's
    PROC_FLAG_TAKE_MELEE_SWING at the aura's ProcChance; and for Seal of Command's strike, a triggered melee ability
    with SPELL_ATTR_EX3_NOT_A_PROC, its own roll; Seal of Righteousness's triggered payload lacks it), and
    Unit::DealMeleeDamage (Unit.cpp:2244-2253): a player's damaging hit refreshes her judgements
    (SPELL_ATTR_EX3_ALWAYS_HIT paladin auras of hers) on the victim."""
    caster = req.get('caster', {})
    if ch.cls != F.CLASS_PALADIN:
        return out
    t = req['target']
    judged = target_judgements(cat, t)
    if out.get('outcome') not in LANDED or out.get('damage', 0) <= 0 or out.get('spell'):
        return out
    rng = random.Random(f"{req.get('seed', 0)}:paladin_melee")
    procs = []
    seal = active_seal(cat, caster)
    if seal is not None and all(sid in cat.spells for sid in seal_payloads(cat.spell(seal))):
        procs += _seal_procs(cat, ch, req, out, seal, rng)
    procs += _judgement_procs(cat, ch, judged, PROC_TAKE_MELEE_SWING, rng)
    if judged:
        # The seal's strike is its own hit on the judged unit (TriggerProccedSpell: a triggered spell), rolled apart.
        strikes = random.Random(f"{req.get('seed', 0)}:judgement_procs")
        for proc in [p for p in procs if p.get('source_spell') == seal and p.get('target') == 'target']:
            if proc['spell'] in cat.spells and _damaging(proc['events']):
                hit = cat.spell(proc['spell'])
                procs += _judgement_procs(cat, ch, judged, victim_proc_flag(hit), strikes, hit, triggered=True)
    if procs:
        # A seal's damage rides on her swing: convert it with the swing's own native weapon factor when it has one.
        for proc in procs:
            if 'weapon_factor' in out and any(e['type'] == 'damage' for e in proc['events']):
                proc['weapon_factor'] = out['weapon_factor']
        out['procs'] = out.get('procs', []) + procs
    out['refresh_judgements'] = True
    return out


# --- Judgement -----------------------------------------------------------------------------------------------

def judgement(cat, ch, req):
    """Judgement (20271; spell_judgement, Paladin.cpp:77-116). CasterAuraState 5 (AURA_STATE_JUDGEMENT, which an
    active seal sets: SpellAuras.cpp:5303) else SPELL_FAILED_CASTER_AURASTATE; DmgClass none, so it never misses
    (Unit::SpellHitResult). It finds her seal's effect 2 judgement, removes the seal (consumed_auras) and casts that
    judgement on the target with TRIGGERED_IGNORE_HIT_CALCULATION (the payload in procs, 'judgement': true). The
    starting Seal of Righteousness (20154) has no effect 2: nothing is cast and it stays (the cost and cooldown
    still apply)."""
    from service import feral, spells
    s = cat.spell(JUDGEMENT)
    caster = req.get('caster', {})
    feral.validate_form(s, caster)
    seal = active_seal(cat, caster)
    if seal is None:
        raise spells.CastError('SPELL_FAILED_CASTER_AURASTATE')
    spells._validate(cat, s, ch, req)
    out = spells.outcome(cat, ch, s)
    judged = seal_judgement(cat.spell(seal))
    if judged is None:
        return out
    _require(cat, judged, *filter(None, [_link('dummy', judged)]))
    rng = random.Random(f"{req.get('seed', 0)}:judgement")
    out['consumed_auras'] = [seal]
    payload = judge(cat, ch, cat.spell(judged), {**req['target'], 'kind': 'unit'}, rng)
    out['procs'] = [{'source_spell': seal, 'judgement': True, 'target': 'target', 'cooldown_ms': 0, **payload}]
    return out


def judge(cat, ch, j, target, rng):
    """One judgement on the target: Righteousness's holy damage (Spell::EffectSchoolDMG, magic: a spell crit, partial
    resist), Command's dummy (SpellEffects.cpp:1454-1462) casting its damage rank - halved unless the target is stunned
    (JudgementOfCommand, Paladin.cpp:157-164; target.stunned), a melee-class holy strike (melee crit x2) - and the
    debuffs: the Crusader's holy damage taken (14), Justice's no-fleeing (92), Light and Wisdom's procs (42) for those
    who hit the target. Its auras stack in group Judgement (spell_group 6, UNIQUE_PER_CASTER): one per caster per
    target, a new one replacing hers; a stronger similar one already there bounces it (IsSimilarExistingAuraStronger,
    stack_amounts)."""
    from service import spells
    out = {'spell': j['Id'], 'name': j['SpellName'], 'rank': j['Rank1'], 'school': j['School'], 'events': [],
           'auras': []}
    duration = int(F.apply_spell_mod(j, F.SPELLMOD_DURATION, cat.duration(j, ch.level), ch.mods))
    for e in (1, 2, 3):
        effect, aura = j[f'Effect{e}'], j[f'EffectApplyAuraName{e}']
        if effect == E_SCHOOL_DAMAGE:
            value, raw = _value(ch, j, e, rng)
            event, out['crit_chance'] = _holy_damage(cat, ch, j, target, value, raw, rng)
            out['events'].append(event)
        elif effect == E_DUMMY and (damage := _link('dummy', j['Id'])) is not None:
            d = cat.spell(damage)
            value, raw = _value(ch, d, 1, rng)
            if not target.get('stunned', False):
                value = value // 2
            event, out['crit_chance'] = _holy_damage(cat, ch, d, target, value, raw, rng)
            event['halved'] = not target.get('stunned', False)
            out['events'].append(event)
        elif effect == 6 and aura:
            a = {'eff': e, 'aura': aura, 'duration_ms': duration, 'amplitude_ms': 0, 'stack': 'replace_same_caster',
                 'amount': int(F.effect_value(j, e, ch.level, None, mods=ch.mods))}
            if aura == A_MOD_DAMAGE_TAKEN:
                a.update(type='damage_taken', school_mask=j[f'EffectMiscValue{e}'])
            elif aura == A_PREVENTS_FLEEING:
                a.update(type='prevent_fleeing')
            elif aura == A_PROC_TRIGGER_SPELL and _link('script', j['Id']) in cat.spells:
                p = cat.spell(_link('script', j['Id']))
                a.update(type='judgement_proc', trigger_spell=j[f'EffectTriggerSpell{e}'], payload=p['Id'],
                         payload_type='heal' if p['Effect1'] == E_HEAL else 'mana',
                         payload_amount=int(F.effect_value(p, 1, ch.level, None, mods=ch.mods)),
                         proc_chance=j['ProcChance'], proc_flags=j['ProcFlags'])
            else:
                a['type'] = f'aura_{aura}'
            out['auras'].append(a)
    rule, _ = groups()['Judgement']
    out.update(stack='Judgement', stack_rule=rule, stack_amounts=spells.stack_amounts(j, out['auras']))
    return out


def seal_tooltips(cat, ch, s):
    """The spells a seal's description quotes ($20286s1, $20424s1, $20966s1, $20340s1...): its judgement (and the
    damage rank Command's judgement casts), its proc payload; talent_effects.tooltip profiles of each."""
    from service.talent_effects import tooltip
    quoted = [seal_judgement(s), _link('dummy', seal_judgement(s)), _link('script', seal_judgement(s))]
    quoted += seal_payloads(s)
    return {str(sid): tooltip(cat, ch, cat.spell(sid)) for sid in quoted if sid in cat.spells}


# --- dispatch ------------------------------------------------------------------------------------------------

def resolve(cat, ch, req):
    from service import spells
    s = cat.spell(req['spell'])
    if s['Id'] == JUDGEMENT:
        return judgement(cat, ch, req)
    out = spells.resolve(cat, ch, req, feral_dispatch=False)
    rng = random.Random(f"{req.get('seed', 0)}:paladin")
    if script_heal(s['Id']) is not None:
        _script_heal(cat, ch, s, out, rng, req)
    elif any(s[f'Effect{e}'] == E_HEAL_MAX_HEALTH for e in (1, 2, 3)):
        _lay_on_hands(cat, ch, s, out, rng)
    elif is_seal(s['Id']):
        _seal(cat, ch, s, out)
    return out
