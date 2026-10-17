"""Live Skyrim instance modifiers, separate from intrinsic catalog item stats.

Native magic still executes in Skyrim. Mirror resource/resistance/weapon/cost
modifiers used by the WoW service without feeding its own equip abilities back.
ActorValue IDs come from CommonLib/Skyrim.esm, not magic effect editor names.
"""
import math


def number(value):
    value = float(value)
    if not math.isfinite(value):
        raise ValueError('non-finite equipment modifier')
    return value


def totals(modifiers):
    effects = modifiers.get('enchantments', [])
    if len(effects) > 256:
        raise ValueError('too many equipment enchantment effects')
    result = {}
    for effect in effects:
        av = int(effect['av'])
        result[av] = result.get(av, 0) + number(effect['magnitude'])
    # Fortified skills as Skyrim has them now (potions, blessings and enchantments alike) replace the worn sum.
    live = modifiers.get('live', {})
    if len(live) > 32:
        raise ValueError('too many live actor values')
    for av, value in live.items():
        result[int(av)] = number(value)
    return result


def apply(ch, modifiers):
    values = totals(modifiers)
    # Skyrim magicka costs use the established .2 resource conversion.
    ch.item_mana += values.get(25, 0) * 5
    ch.native_health_bonus = values.get(24, 0)
    if ch.actor_values is None:  # New snapshots already include worn native enchantments.
        ch.magicka_regen_percent = values.get(156, 0)
        for av, schools in ((41, (2,)), (43, (4,)), (40, (3,)), (44, (1, 2, 3, 4, 5, 6))):
            for school in schools:
                ch.item_resist[school] = ch.item_resist.get(school, 0) + values.get(av, 0) * 3
    # Match the service's main-hand selection and do not improve an unequipped
    # catalog item or feral claws. Ratios compare the same native weapon with
    # and without its ExtraHealth, so native skills/perks cancel here.
    entry = int(modifiers.get('weapon_entry', 0))
    ratio = max(0, number(modifiers.get('temper_multiplier', 1)))
    if entry and entry == ch.weapon_entry:
        ch.weapon['min'] *= ratio
        ch.weapon['max'] *= ratio
        ch.weapon_temper = ratio
        ch.weapon_damage = max(0, number(modifiers.get('weapon_damage', 0)))
    elif not entry and 'weapon_damage' in modifiers:
        # A vanilla weapon or her bare hands (the plugin's UnarmedDamage): its Skyrim damage governs the swing the
        # same way (melee.native_factor); WoW's roll lends spread, table and crits only. Tempering is Skyrim's own.
        ch.weapon_damage = max(0, number(modifiers.get('weapon_damage', 0)))
        ch.weapon_temper = 1.0
        if modifiers.get('weapon_unarmed'):
            ch.weapon_unarmed = True
    ch.weapon_skill = modifiers.get('weapon_skill', ch.weapon_skill)
    if ch.weapon_skill not in ('OneHanded', 'TwoHanded', 'Marksman'):
        raise ValueError('invalid native weapon skill')
    av = {'OneHanded': 96, 'TwoHanded': 97, 'Marksman': 98}[ch.weapon_skill]
    ch.weapon_multiplier = max(0, 1 + values.get(av, 0) / 100)
    ch.equipment_modifiers = modifiers


def cost_multiplier(caster, skill):
    av = {'Alteration': 108, 'Conjuration': 109, 'Destruction': 110,
          'Illusion': 111, 'Restoration': 112}.get(skill)
    return max(0, 1 - totals(caster.get('equipment_modifiers', {})).get(av, 0) / 100)
