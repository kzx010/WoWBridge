"""Live Skyrim defenses, excluding bridge-owned AV contributions (docs/native-defenses.md)."""
from service import formulas as F
from service.progression import number

VALUES = (27, 28, 29, 39, 40, 41, 42, 43, 44, 45, 83, 155, 156, 157)


def snapshot(data):
    if not isinstance(data, dict) or set(data) - {'live', 'owned', 'base'}:
        raise ValueError('actor_values must contain live, owned and base maps')
    out = {}
    for name in ('live', 'owned', 'base'):
        rows = data.get(name, {})
        if not isinstance(rows, dict) or len(rows) > len(VALUES):
            raise ValueError(f'actor_values.{name} must be an actor value map')
        out[name] = {}
        for av, value in rows.items():
            if str(av) not in {str(i) for i in VALUES}:
                raise ValueError('unsupported defense actor value')
            out[name][str(av)] = number(value, f'actor_values.{name}.{av}')
    return out


def external(data, av, default=0):
    key = str(av)
    return data['live'].get(key, default) - data['owned'].get(key, 0)


def resist_percent(data, school, dispel=0):
    """Native magic and element resistances multiply damage; disease uses its dispel category."""
    if school == 0:
        return 0.0
    # Nature is lightning (Shock). Poison resistance only meets spells marked Poison and disease resistance spells
    # marked Disease: draugr, skeletons, automatons and vampires are immune to poison, not to Wrath or Roots.
    elements = {2: (41,), 3: (42,), 4: (43,)}.get(school, ())
    if dispel == 3:
        elements += (45,)
    elif dispel == 4 and 40 not in elements:
        elements += (40,)
    multiplier = 1.0
    for av in (44, *elements):
        multiplier *= max(0, 1 - external(data, av) / 100)
    return (1 - multiplier) * 100


def resistance(data, school, attacker_level, victim_level, dispel=0):
    pct = resist_percent(data, school, dispel)
    # Invert Unit::CalculateEffectiveMagicResistancePercent. Keep fractions:
    # truncating low-level resistance would lose small native effects.
    skill = max(F.skill_max(attacker_level if pct >= 0 else victim_level), 100)
    return pct * skill / (75 if pct >= 0 else 100)


def regen_multipliers(data):
    out = {}
    for name, rate, mult, default in (('health', 27, 155, .7), ('magicka', 28, 156, 3), ('stamina', 29, 157, 5)):
        base = data['base'].get(str(rate), default)
        # A zero base (a fixture that switches native regeneration off) leaves the WoW rate alone.
        ratio = max(0, external(data, rate, base)) / base if base > 0 else 1
        out[name] = ratio * max(0, external(data, mult, 100)) / 100
    return out


def target_resistance(target, school, attacker_level, dispel=0):
    value = number(target.get('resist', {}).get(str(school), 0), 'target.resist')
    if 'actor_values' in target:
        data = snapshot(target['actor_values'])
        value += resistance(data, school, attacker_level, target['level'], dispel)
    return value


def absorb_chance(data):
    return min(100, max(0, external(data, 83)))


def absorbs(target, school, rng):
    if school == 0 or 'actor_values' not in target:
        return False
    chance = absorb_chance(snapshot(target['actor_values']))
    return chance > 0 and F.roll_chance(rng, chance)
