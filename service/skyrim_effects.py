"""Shared WoW -> vanilla Skyrim magnitudes for gear and spell auras."""

STAT = {0: [(0x49504, .1)], 1: [(0x493AA, .15)], 3: [(0x49507, 1.5)],
        4: [(0x7A0F4, 1), (0x7A0FF, .5), (0x7A106, .5)],
        5: [(0x49504, 1.5)], 6: [(0x7A0FD, 1)], 7: [(0x493AA, 1.5)],
        38: [(0x7A0FF, .1), (0x7A106, .1)], 43: [(0x7A0FD, 2)]}
RESISTS = {'fire_res': 0x48C8B, 'frost_res': 0x48F45, 'nature_res': 0x49295,
           'arcane_res': 0xB7A35, 'shadow_res': 0xB7A35}
RESIST_DIVISOR = 3
ARMOR_DIVISORS = {1: 15, 2: 8, 3: 12, 4: 30}
# Skyrim.esm DATA: archetype 0 (Value Modifier), primary AV 39 (DamageResist).
# 0xE5F55 doomLordDamageEffect: beneficial; 0x10319D VoiceMarkedForDeathArmorEffect01:
# detrimental (flag 0x4). Its positive magnitude subtracts armor; do not negate it again.
ARMOR_BUFF, ARMOR_DEBUFF = 0xE5F55, 0x10319D
MAGICKA_REGEN = STAT[6][0][0]  # 0x7A0FD EnchFortifyMagickaRateConstantSelf


def aura_effects(aura: dict) -> list[dict]:
    """Keep WoW amounts intact and describe only their Skyrim equivalents."""
    native = {}

    def add(effect, value):
        native[effect] = native.get(effect, 0) + value

    amount = aura.get('amount', 0)
    if aura['type'] == 'armor':
        add(ARMOR_BUFF if amount >= 0 else ARMOR_DEBUFF, abs(amount) / ARMOR_DIVISORS[2])
    elif aura['type'] == 'stat':
        for which, stat in enumerate((4, 3, 7, 5, 6)):
            if aura['stat'] in (-1, which):
                for effect, scale in STAT[stat]:
                    add(effect, amount * scale)
    elif aura['type'] == 'resistance':
        # Holy has no gear equivalent. As in build_esp.effects_for, Arcane and
        # Shadow both contribute to the shared Resist Magic effect.
        for school, field in ((2, 'fire_res'), (3, 'nature_res'), (4, 'frost_res'),
                              (5, 'shadow_res'), (6, 'arcane_res')):
            if aura['school_mask'] & (1 << school):
                add(RESISTS[field], amount / RESIST_DIVISOR)
    elif aura['type'] == 'mana_regen_percent':
        add(MAGICKA_REGEN, amount)
    return [{'mgef': effect, 'magnitude': value} for effect, value in sorted(native.items()) if value]
