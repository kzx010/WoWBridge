"""Convert completed WoW outcomes to Skyrim units without changing combat rolls."""
from service.character import A_MOD_ROOT, A_PERIODIC_DAMAGE, A_PERIODIC_HEAL
from service.skyrim_effects import aura_effects
from service.crafting import cost_multiplier
from service.skyrim_balance import (COUNTERPARTS, SPELLS, FERAL_MELEE,
                                    SKILL_COST_BASE, SKILL_COST_SCALE, SKILL_COST_MULT)


SKILLS = {'Destruction': 'destruction', 'Restoration': 'restoration', 'Alteration': 'alteration',
          'TwoHanded': 'two_handed', 'OneHanded': 'one_handed', 'Marksman': 'archery',
          'Block': 'block', 'Sneak': 'sneak', 'Alchemy': 'alchemy', 'Lockpicking': 'lockpicking',
          'Conjuration': 'conjuration', 'Illusion': 'illusion'}


def spell_skill(spell: dict, rules=None) -> str:
    rules = SPELLS if rules is None else rules
    if spell['Id'] in rules:
        return rules[spell['Id']].skill
    from service.feral import ABILITIES
    if spell['Id'] in ABILITIES:
        return 'OneHanded'
    effects = [e for e in (1, 2, 3) if spell[f'Effect{e}']]
    # Roots train Alteration, including Entangling Roots' accompanying damage over time.
    if any(spell[f'EffectApplyAuraName{e}'] == A_MOD_ROOT for e in effects):
        return 'Alteration'
    if any(spell[f'Effect{e}'] == 10 or spell[f'EffectApplyAuraName{e}'] == A_PERIODIC_HEAL for e in effects):
        return 'Restoration'
    if any(spell[f'Effect{e}'] in (38, 113) for e in effects):
        return 'Restoration'
    if any(spell[f'Effect{e}'] == 2 or spell[f'EffectApplyAuraName{e}'] == A_PERIODIC_DAMAGE for e in effects):
        return 'Destruction'
    return 'Alteration'


def melee_skill(cat, ch) -> str:
    return ch.weapon_skill


def cost_factor(level: float) -> float:
    """Vanilla PC skill discount; perks/enchantments are not implied by skill."""
    return SKILL_COST_MULT * (1 - (min(100, max(0, level)) * SKILL_COST_BASE) ** SKILL_COST_SCALE)


def convert(out: dict, caster: dict, skill: str, rules=None) -> dict:
    """Convert resources and attach native buff effects; preserve WoW buff values and timing."""
    if 'skyrim' not in caster:
        return out
    levels = caster['skyrim']
    rules = SPELLS if rules is None else rules
    rule = rules.get(out.get('spell'))
    if rule is None and 'damage' in out and caster.get('form') in ('cat', 'bear', 'dire_bear'):
        rule = FERAL_MELEE
    # The slice uses fixed native magnitudes. Unmapped catalog extensions and
    # humanoid weapon swings retain their prior unit conversion.
    factor = rule.health_factor if rule else 0.25 * (0.5 + levels.get(SKILLS[skill], 50) / 100)
    # Swiftmend's DBC heal is a one-point placeholder, not a health budget.
    # Its resolved amount comes from a snapshotted WoW HoT: use that HoT's
    # conversion, while keeping Swiftmend's own cost/skill rule.
    if out.get('spell') == 18562 and out.get('consumed_target_hot') in rules:
        factor = rules[out['consumed_target_hot']].health_factor

    def health_amounts(part: dict, keys=('amount', 'resisted', 'absorbed')):
        for key in keys:
            if key in part:
                value = part[key]
                part[key] = round(value * factor)
                if rule and rule.minimum_health and factor > 0 and value > 0 and key in ('amount', 'per_tick', 'damage', 'bonus_damage', 'health'):
                    part[key] = max(1, part[key])

    out['skill'] = skill
    if 'cost' in out and out.get('power', 'mana') == 'mana':
        cost = out['cost']
        enchant_cost = cost_multiplier(caster, skill)
        if rule and rule.cost:
            # Retain CMaNGOS's rank/level/cost-mod ratio, normalized to level 60.
            ratio = cost / rule.wow_cost if rule.wow_cost else 1
            native_cost = rule.base_cost * cost_factor(levels.get(SKILLS[skill], 50)) * ratio * enchant_cost
            leaving = any(a['type'] == 'shapeshift' and a['action'] == 'leave' for a in out.get('auras', []))
            out['cost'] = 0 if leaving or native_cost <= 0 else max(1, round(native_cost))
        else:
            out['cost'] = max(1, round(cost * 0.2 * enchant_cost)) if cost > 0 and enchant_cost > 0 else 0
    health_amounts(out, ('damage', 'blocked', 'resisted', 'absorbed'))
    for event in out.get('events', []) + out.get('results', []):
        if event['type'] in ('damage', 'heal'):
            health_amounts(event, ('amount', 'resisted', 'absorbed', 'blocked'))
        elif event['type'] == 'next_swing':
            health_amounts(event, ('bonus_damage',))
        elif event['type'] == 'resurrect':
            health_amounts(event, ('health',))
            mana_factor = (sum(COUNTERPARTS[name]['magnitude'] * units / reference
                               for name, units, reference in rule.mana) if rule and rule.mana else .2)
            event['mana'] = round(event['mana'] * mana_factor)
    for aura in out.get('auras', []):
        if aura['type'] == 'rage_heal':
            # A rate must retain its fraction; round only the completed tick.
            aura['health_per_rage'] *= factor
        elif aura['type'] == 'shapeshift':
            # Profile/AP/base weapon values remain WoW diagnostics. Use these
            # explicit equivalents for the fixed bonus health in Skyrim.
            aura['skyrim_health_bonus'] = round(aura['health_bonus'] * factor)
        if aura['type'] in ('damage', 'heal', 'damage_shield'):
            health_amounts(aura, ('amount', 'per_tick', 'resisted', 'absorbed'))
            for tick in aura.get('ticks', []):
                health_amounts(tick)
        elif aura['type'] == 'damage_done' and rule:
            # Tiger's Fury is a flat health-damage bonus, not a percentage.
            health_amounts(aura, ('amount',))
        effects = aura_effects(aura)
        native = rule.native if rule else {}
        if rule and rule.native_from is not None:
            native = rules[rule.native_from].native
        if aura['type'] in native:
            effects = [{'mgef': mgef, 'magnitude': abs(aura['amount']) * target / reference}
                       for mgef, target, reference in native[aura['type']]]
            effects.sort(key=lambda e: e['mgef'])
        if effects:
            aura['skyrim'] = effects
    for proc in out.get('procs', []):
        convert(proc, caster, proc.get('skill', skill), rules)
    return out
