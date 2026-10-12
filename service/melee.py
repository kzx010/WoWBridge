"""A player's melee swing (auto-attack) against a creature, as CMaNGOS rolls it: Unit::CalculateMeleeDamage (Unit.cpp:1897)
-> weapon damage with attack power (Player::CalculateMinMaxDamage, StatSystem.cpp) -> armor (CalcArmorReducedDamage)
-> the one-roll attack table (RollMeleeOutcomeAgainst, Unit.cpp:2709): miss, dodge, parry, block, glancing, crit, hit.

Skyrim targets are creatures with CMaNGOS's defaults: they can dodge, parry and block (no NO_PARRY/NO_BLOCK flags),
their defense is level x 5, their armor and strength are creature_template_classlevelstats' (class 1) for their level.
"""
import random

from service import formulas as F
from service.character import Character


class MeleeError(Exception):
    """A swing that cannot be made; the plugin shows the client's error (ERR_*)."""


def _die(rng: random.Random, sides: list[tuple[str, float]]) -> str:
    """Die<UnitCombatDieSide> (Unit.h): chances in hundredths stacked in order, one urand(1, 10000)."""
    roll, acc = rng.randint(1, 10000), 0
    for name, chance in sides:
        acc += int(max(0.0, min(chance, 100.0)) * 100)
        if roll <= acc:
            return name
    return 'hit'


def target_armor(cat, target):
    """Native target aura snapshot, e.g. Faerie Fire; flat reductions precede mitigation."""
    from service.progression import number
    reduction = number(target.get('armor_reduction', 0), 'target.armor_reduction')
    if reduction < 0:
        raise ValueError('target.armor_reduction must be nonnegative')
    return max(0, cat.creature_armor(int(target['level'])) - reduction)


def chances(ch: Character, target: dict) -> dict:
    """Each side of the attack table for this attacker and target (percent)."""
    level = int(target['level'])
    skill = ch.level * 5                     # weapon skill at its maximum for the level
    cap = ch.level * 5                       # GetSkillMaxForLevel
    defense = level * 5                      # a creature's GetDefenseSkillValue
    front = bool(target.get('in_front', True))   # the attacker in front of the target: it can parry and block
    out = {}
    # CalculateEffectiveMissChance: 5% for every unit; against an NPC with more defense than our skill, 0.1 a point
    # for the first 10, then 0.4 + 0.2 a point (pre-WotLK); else 0.04 a point.
    miss, diff = 5.0, defense - skill
    if diff > 10:
        miss += 10 * 0.1
        miss += (diff - 10) * 0.2
        miss += (diff - 10) * 0.4
    else:
        miss += diff * (0.1 if diff > 0 else 0.04)
    out['miss'] = miss - (ch.total(54) if ch.talent_mode else 0)
    # Dodge: 5% base for creatures, 0.1 a point of positive difference (0.04 when ours is higher).
    out['dodge'] = 5.0 + diff * (0.1 if diff > 0 else 0.04)
    # Parry (from in front): 5% base; 0.1 a point, 0.6 beyond 10 points.
    out['parry'] = (5.0 + diff * ((0.6 if diff > 10 else 0.1) if diff > 0 else 0.04)) if front else 0.0
    # Block (from in front): 5% base; NPCs gain nothing from a positive difference.
    out['block'] = (5.0 + diff * (0.0 if diff > 0 else 0.04)) if front else 0.0
    # Glancing (players against creatures above level 10): 10 + 2 a point of defense over our capped skill; druids are
    # not among the wand users with the caster penalty.
    out['glance'] = (10.0 + (defense - min(skill, cap)) * 2) if level > 10 and not target.get('is_player') else 0.0
    # Crit: agility and gear; against NPCs 0.2 a point of skill over their defense (crit suppression below).
    out['crit'] = ch.melee_crit() + (skill - defense) * 0.2
    return {k: max(0.0, min(v, 100.0)) for k, v in out.items()}


def swing(cat, ch: Character, req: dict) -> dict:
    t = req['target']
    if not t.get('alive', True):
        raise MeleeError('ERR_INVALID_ATTACK_TARGET')
    if not t.get('attackable', t.get('reaction') != 'friendly'):   # a townsperson she may strike says attackable
        raise MeleeError('ERR_INVALID_ATTACK_TARGET')
    if t.get('distance_yd', 0.0) >= F.melee_reach(t.get('combat_reach_yd', F.DEFAULT_COMBAT_REACH)):
        raise MeleeError('ERR_BADATTACKPOS')
    if not t.get('facing', True):
        raise MeleeError('ERR_BADATTACKFACING')
    from service import feral, spells
    caster = req.get('caster', {})
    feral.state(caster)
    next_swing = caster.get('next_swing')
    if next_swing is not None:
        if next_swing not in cat.spells or feral.ability(cat, next_swing) != 9881:
            raise ValueError('caster.next_swing must be a Maul rank')
        try:
            ability = feral.resolve(cat, ch, {**req, 'spell': next_swing,
                                              'target': {**t, 'kind': 'unit', 'distance_yd': t.get('distance_yd', 0)}},
                                    executing_swing=True)
        except spells.CastError as e:
            # Unit.cpp:AttackerStateUpdate falls back to an ordinary swing when
            # its queued spell fails CheckCast (e.g. rage spent since queuing).
            fallback = swing(cat, ch, {**req, 'caster': {k: v for k, v in caster.items() if k != 'next_swing'}})
            fallback.update(next_swing_consumed=True, next_swing_error=str(e))
            return fallback
        event = ability['events'][0]
        out = {'outcome': event.get('outcome', event['type']), 'delay_ms': ch.melee_weapon()['delay'],
               'damage': event.get('amount', 0), 'blocked': event.get('blocked', 0), 'spell': next_swing,
               'power': ability['power'], 'cost': ability['cost'], 'rage_gained': 0,
               'next_swing_consumed': True}
        if 'power_refund' in ability:
            out['power_refund'] = ability['power_refund']
        if ability.get('breaks_prowl'):
            out['breaks_prowl'] = True
        return out
    w = ch.melee_weapon()
    rng = random.Random(req.get('seed', 0))
    from service.perks import WHITE
    table = chances(ch, t)
    table['crit'] = F.apply_spell_mod(WHITE, F.SPELLMOD_CRITICAL_CHANCE, table['crit'], ch.mods)
    side = _die(rng, list(table.items()))
    out = {'outcome': side, 'delay_ms': w['delay'], 'damage': 0, 'blocked': 0}
    if caster.get('prowling', False):
        out['breaks_prowl'] = True
    if ch.form in ('bear', 'dire_bear'):
        out['rage_gained'] = 0
    if side in ('miss', 'dodge', 'parry'):
        return out
    # Player::CalculateMinMaxDamage: weapon damage plus attack power / 14 a second of the weapon's speed.
    bonus = ch.attack_power() / 14.0 * (w['delay'] / 1000.0)
    if ch.form != 'none':
        bonus += ch.total(13, 1)  # StatSystem.cpp:CalculateMinMaxDamage, Tiger's Fury.
    multiplier = ch.multiplier(79, 1) * (ch.weapon_multiplier if ch.form not in ('cat', 'bear', 'dire_bear') else 1)
    damage = rng.randint(int((w['min'] + bonus) * multiplier), int((w['max'] + bonus) * multiplier))      # Unit::CalculateDamage
    damage = int(F.apply_spell_mod(WHITE, F.SPELLMOD_DAMAGE, damage, ch.mods))
    armor = target_armor(cat, t)
    damage = F.armor_reduced(damage, armor, ch.level)                       # CalcArmorReducedDamage
    damage = int(damage)
    if side == 'crit':
        damage = int(damage * (1 + F.apply_spell_mod(WHITE, F.SPELLMOD_CRIT_DAMAGE_BONUS, 1, ch.mods)))
    elif side == 'glance':
        # CalculateGlanceAmount: a multiplier between low and high ends set by the defense difference.
        diff = int(t['level']) * 5 - ch.level * 5
        if diff >= 0:                       # our skill beating their defense: no reduction
            high = min(max(1.2 - 0.03 * diff, 0.2), 0.99)
            low = min(max(1.3 - 0.05 * diff, 0.01), min(0.91, high))
            damage = int(damage * rng.randint(int(low * 100), int(high * 100)) * 0.01)
    elif side == 'block':
        # Creature::GetShieldBlockValue: level / 2 + strength / 20.
        blocked = min(damage, int(t['level']) // 2 + int(cat.creature_strength(int(t['level'])) / 20))
        out['blocked'] = blocked
        damage -= blocked
    out['damage'] = max(damage, 0)
    if ch.form in ('bear', 'dire_bear'):
        out['rage_gained'] = F.reward_rage(out['damage'], ch.level, True)
    return out


def ability_outcome(ch, s, target, rng):
    """Unit.cpp:MeleeSpellHitResult: yellow attacks cannot glance or crit on
    the hit die. Crit and partial block roll separately for damaging spells.
    Pounce/Ravage have NO_ACTIVE_DEFENSE; Rake has COMPLETELY_BLOCKED.
    """
    table = chances(ch, target)
    table['miss'] = max(0, F.apply_spell_mod(s, F.SPELLMOD_RESIST_MISS_CHANCE,
                                           table['miss'] - ch.total(54), ch.mods))
    sides = [('miss', table['miss'])]
    if not s['Attributes'] & 0x200000:
        sides += [('dodge', 0 if target.get('is_player') and not target.get('in_front', True) else table['dodge']),
                  ('parry', table['parry'])]
        if s['AttributesEx3'] & 8:
            sides.append(('block', table['block']))
    return _die(rng, sides)


def ability_damage(cat, ch, s, target, amount, rng):
    """Unit.cpp:CalculateSpellDamage then CalculateAbsorbResistBlock.
    Physical direct ability damage uses armor, crit x2, then partial block.
    """
    table = chances(ch, target)
    crit = F.roll_chance(rng, F.apply_spell_mod(s, F.SPELLMOD_CRITICAL_CHANCE, table['crit'], ch.mods))
    if crit:
        multiplier = 1 + F.apply_spell_mod(s, F.SPELLMOD_CRIT_DAMAGE_BONUS, 1, ch.mods)
        amount = int(amount * multiplier)
    amount = int(F.armor_reduced(amount, target_armor(cat, target), ch.level)) if amount else 0
    blocked = 0
    if not s['Attributes'] & 0x200000 and not s['AttributesEx3'] & 8 and F.roll_chance(rng, table['block']):
        blocked = min(amount, int(target['level']) // 2 + int(cat.creature_strength(int(target['level'])) / 20))
    return {'type': 'damage', 'amount': amount - blocked, 'school': 0, 'resisted': 0,
            'crit': crit, 'blocked': blocked, 'outcome': 'crit' if crit else ('block' if blocked else 'hit')}
