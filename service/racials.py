"""Night Elf General-tab racials; Classic rules and explicit Skyrim equivalents."""
import random

from service import formulas as F
from service.skyrim_effects import ARMOR_BUFF, aura_effects

SHADOWMELD = 20580
PASSIVES = (20582, 20583, 20585)


def validate(s, ch, req):
    from service.spells import CastError
    if s['Id'] not in (SHADOWMELD, *PASSIVES):
        return
    if ch.race != 4:
        raise CastError('SPELL_FAILED_NOT_KNOWN')
    if s['Id'] in PASSIVES:
        raise CastError('SPELL_FAILED_NOT_KNOWN')   # automatic passives, never cast from bars
    if req['target'].get('kind') != 'self':
        raise CastError('SPELL_FAILED_BAD_TARGETS')
    caster = req.get('caster', {})
    # Playtest rule: stationary Shadowmeld may drop combat even in combat.
    if not isinstance(caster.get('moving', False), bool):
        raise ValueError('caster.moving must be a boolean')
    if caster.get('moving', False):
        raise CastError('SPELL_FAILED_MOVING')


def passives(cat, ch, skyrim=False):
    if ch.race != 4:
        return []
    out = []
    for sid in PASSIVES:
        if sid not in cat.spells:   # older catalogs remain readable; regeneration supplies racials
            continue
        s = cat.spell(sid)
        p = {'spell': sid, 'name': s['SpellName'], 'passive': True, 'duration_ms': -1}
        if sid == 20582:
            p.update(type='dodge', amount=F.effect_value(s, 1, ch.level, None, maximum=True))
            # Skyrim has no dodge table: armor gives .12% physical mitigation per point, capped at 80%.
            if skyrim:
                p['skyrim'] = [{'mgef': ARMOR_BUFF, 'magnitude': p['amount'] / .12}]
        elif sid == 20583:
            p.update(type='resistance', amount=F.effect_value(s, 1, ch.level, None, maximum=True), school_mask=8)
            if skyrim:
                p['skyrim'] = aura_effects(p)   # existing Nature -> poison resistance, /3
        else:
            # Player::BuildPlayerRepop casts 20584 for Night Elves, not from this dummy passive.
            p.update(type='ghost', ghost_spell=20584, informational=True)
        out.append(p)
    return out


def resist_damage(ch, req):
    """Optional incoming spell resistance roll; the legacy Roots-break request stays unchanged."""
    school = req['school']
    if isinstance(school, bool) or not isinstance(school, int) or school not in range(1, 7):
        raise ValueError('school must be a magical school from 1 to 6')
    damage, level = int(req['damage']), int(req.get('attacker_level', req['victim_level']))
    if damage < 0 or level <= 0:
        raise ValueError('damage must be nonnegative and attacker_level positive')
    binary = req.get('binary', False)
    if not isinstance(binary, bool):
        raise ValueError('binary must be a boolean')
    pct = F.effective_resist_percent(level, ch.level, ch.resistance(school), binary,
                                   target_is_creature=False)
    rng = random.Random(req.get('seed', 0))
    fraction = (1.0 if F.roll_chance(rng, pct) else 0.0) if binary else F.partial_resist_multiplier(
        pct, req.get('periodic', False), False, req.get('attacker_is_player', False), rng)
    resisted = int(damage * fraction)
    return {'damage': damage - resisted, 'resisted': resisted, 'resistance': ch.resistance(school)}
