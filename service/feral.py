"""Stateless druid forms and abilities, following CMaNGOS Classic.

The forms' passive spells the core links in code are in the catalog
(data/build_catalog.py LINKED_SPELLS). Display IDs come from SpellAuras.cpp:HandleAuraModShapeshift;
form IDs/flags from 1.12 SpellShapeshiftForm.dbc (Cat 112, other forms 80).
"""
import math
import random

from service import formulas as F, melee, spells

FORMS = {'none': 0, 'cat': 1, 'travel': 3, 'aquatic': 4, 'bear': 5, 'dire_bear': 8}
FORM_SPELLS = {5487: 'bear', 9634: 'dire_bear', 768: 'cat', 783: 'travel', 1066: 'aquatic'}
TALENT_FORM_SPELLS = {24858: 'moonkin'}
TALENT_ABILITIES = {16979}
DISPLAY = {'cat': 892, 'travel': 632, 'aquatic': 2428, 'bear': 2281, 'dire_bear': 2281}
DISPLAY['moonkin'] = 15374
LINKED = {'none': (), 'cat': (3025,), 'bear': (1178, 21178), 'dire_bear': (9635, 21178),
          'travel': (5419,), 'aquatic': (5421,)}
LINKED['moonkin'] = (24905,)
ABILITIES = {9881, 9908, 6795, 9898, 8983, 5229, 22896, 9850, 9830, 9904, 9896,
             31018, 9913, 9827, 9867, 9846, 9821, 9892, 17392}


def ability(cat, sid):
    """Identify mechanics by progression chain; read values from the cast rank."""
    chain = cat.chain(sid)
    return next((anchor for anchor in ABILITIES | TALENT_ABILITIES | {5209} if cat.chain(anchor) == chain), None)


# Player.cpp:RegenerateAll/Regenerate, default rates of 1; client rage units.
RESOURCES = {'energy_per_tick': 20, 'energy_tick_ms': 2000, 'energy_max': 100,
             'rage_decay_per_tick': 2.5, 'rage_decay_tick_ms': 2000, 'rage_max': 100,
             'combo_points_max': 5}


def form_name(caster):
    form = caster.get('form', 'none')
    if form not in FORMS and not (form == 'moonkin' and 'talents' in caster):
        raise ValueError('caster.form must be none, bear, dire_bear, cat, travel, aquatic or moonkin')
    return form


def form_mask(form):
    value = 31 if form == 'moonkin' else FORMS[form]
    return 1 << (value - 1) if value else 0


def state(caster):
    form_name(caster)
    power = caster.get('power', {})
    if not isinstance(power, dict):
        raise ValueError('caster.power must be an object')
    for kind in ('rage', 'energy'):
        value = power.get(kind, 0)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not 0 <= value <= 100:
            raise ValueError(f'caster.power.{kind} must be a number from 0 to 100')
        if kind == 'energy' and int(value) != value:
            raise ValueError('caster.power.energy must be an integer')
    cp = caster.get('combo_points', 0)
    if isinstance(cp, bool) or not isinstance(cp, int) or not 0 <= cp <= 5:
        raise ValueError('caster.combo_points must be an integer from 0 to 5')
    if not isinstance(caster.get('prowling', False), bool):
        raise ValueError('caster.prowling must be a boolean')
    if not isinstance(caster.get('in_combat', False), bool):
        raise ValueError('caster.in_combat must be a boolean')
    return power, cp


def validate_form(s, caster):
    """SpellMgr.cpp:GetErrorAtShapeshiftedCast, called by Spell.cpp:CheckCast."""
    # The client first cancels its current form for a form-button cast. The
    # protocol represents that operation in one request, including toggling off.
    if s['Id'] in FORM_SPELLS or ('talents' in caster and s['Id'] in TALENT_FORM_SPELLS):
        # The client's check (the core has none): Aquatic Form is taken only while swimming; leaving it is not.
        if FORM_SPELLS.get(s['Id']) == 'aquatic' and form_name(caster) != 'aquatic' and caster.get('swimming') is False:
            raise spells.CastError('SPELL_FAILED_ONLY_UNDERWATER')
        return
    form = form_name(caster) != 'none'
    mask = form_mask(form_name(caster))
    if mask & s['StancesNot']:
        raise spells.CastError('SPELL_FAILED_NOT_SHAPESHIFT')
    if mask & s['Stances']:
        return
    if form and s['Attributes'] & 0x10000:
        raise spells.CastError('SPELL_FAILED_NOT_SHAPESHIFT')
    if s['Stances'] and (form or not s['AttributesEx2'] & 0x80000):
        raise spells.CastError('SPELL_FAILED_ONLY_SHAPESHIFT')


def linked_spell(cat, sid):
    return cat.spells[sid]


def boosts(cat, ch, form):
    for sid in LINKED[form]:
        s = linked_spell(cat, sid)
        for e in spells._effects(s):
            value = int(F.effect_value(s, e, ch.level, None, maximum=True, mods=ch.mods))
            if ch.talent_mode and s[f'EffectApplyAuraName{e}'] == 99:
                predatory = next((a['amount'] for a in ch.auras if a['aura'] == 4 and
                                  cat.spell(a['spell'])['SpellIconID'] == 1563), 0)
                value += ch.level * predatory // 100
            yield s, e, value


def profile(cat, ch):
    """HandleShapeshiftBoosts, UpdateAttackPowerAndDamage, CalculateMinMaxDamage.

    Armor percent applies to equipment/base armor before agility and flat buffs;
    bear health is a fixed bonus preserving health percentage, not stamina.
    """
    effects = list(boosts(cat, ch, ch.form))
    def total(aura):
        return sum(v for s, e, v in effects if s[f'EffectApplyAuraName{e}'] == aura)
    w = ch.melee_weapon()
    low, high = ch.weapon_damage_range()
    return {'form': ch.form, 'power': 'energy' if ch.form == 'cat' else
            ('rage' if ch.form in ('bear', 'dire_bear') else 'mana'),
            'display_id': DISPLAY.get(ch.form), 'attack_speed_ms': w['delay'],
            'weapon_damage': {'min': w['min'], 'max': w['max']},
            'damage': {'min': low, 'max': high}, 'attack_power': ch.attack_power(),
            'attack_power_bonus': total(99) + (ch.stats['agi'] if ch.form == 'cat' else 0),
            'armor_percent': round((ch.multiplier(142, 1) - 1) * 100, 8) if ch.talents else total(142),
            'health_bonus': total(34),
            'stats': {str(a['misc']): a['amount'] for a in ch.auras if a['spell'] in (24900, 24899)},
            'movement_speed_percent': (max((a['amount'] for a in ch.auras if a['aura'] == 31), default=0)
                                       if ch.talents else total(31)), 'swim_speed_percent': total(58),
            'water_breathing': any(s[f'EffectApplyAuraName{e}'] == 82 for s, e, _ in effects),
            'threat_percent': total(10),
            'feral_threat_bonus_percent': max(0, total(10) - 30) if ch.form in ('bear', 'dire_bear') else 0,
            'stealth_level_bonus': ch.total(154),
            'dodge_bonus_percent': sum(a['amount'] for a in ch.auras if a['aura'] == 49 and a['spell'] in (24867, 24864)),
            'leader_of_the_pack': ({'crit_percent': sum(a['amount'] for a in ch.auras if a['aura'] == 52 and a['spell'] == 24932),
                                    'radius_yd': cat.radius_yd(cat.spell(24932), 1)}
                                   if 17007 in ch.talent_spells and ch.form in ('cat', 'bear', 'dire_bear') else None),
            'linked_spells': list(LINKED[ch.form])}


def _aura(cat, ch, s, e, **fields):
    out = {'eff': e, 'aura': s[f'EffectApplyAuraName{e}'],
            'duration_ms': int(F.apply_spell_mod(s, F.SPELLMOD_DURATION, cat.duration(s, ch.level), ch.mods)),
            'amplitude_ms': s.get(f'EffectAmplitude{e}', 0), 'stack': 'replace_same_caster', **fields}
    if ability(cat, s['Id']) in (5229, 22896, 9913, 9846, 9821) or s['Id'] == 25503:
        out['ends_on_form_loss'] = True  # SpellAuras.cpp:IsRemovedOnShapeshiftLost.
    return out


def _value(s, ch, e, rng, cp=0):
    # Object.cpp:CalculateSpellEffectValue: combo contribution precedes ALL_EFFECTS mods.
    value = F.effect_value(s, e, ch.level, rng) + int(s.get(f'EffectPointsPerComboPoint{e}', 0) * cp)
    return int(F.apply_spell_mod(s, F.SPELLMOD_ALL_EFFECTS, value, ch.mods, eff=e))


def _bonus(ch, s, e, base, dot=False):
    # Unit.cpp:MeleeDamageBonusDone/SpellBonusWithCoeffs; feral uses physical
    # flat damage, not caster spell power. AP coefficient is independent of it.
    value = F.bonus_with_coeffs(s, e, base, ch.total(13, 1), ch.mods)
    value += int(ch.attack_power() * s.get(f'EffectBonusCoefficientFromAP{e}', 0))
    value *= ch.multiplier(79, 1)
    return max(0, int(F.apply_spell_mod(s, F.SPELLMOD_DOT if dot else F.SPELLMOD_DAMAGE, value, ch.mods)))


def _bleed(cat, ch, s, e, rng, cp):
    base = _value(s, ch, e, rng, cp)
    if ability(cat, s['Id']) == 9896:  # SpellAuras.cpp:HandlePeriodicDamage: Rip AP capped at four CP.
        base += int(ch.attack_power() * min(cp, 4) / 100)
    amount = _bonus(ch, s, e, base, dot=True)
    a = _aura(cat, ch, s, e, type='damage', per_tick=amount, school=0, bleed=True)
    a['ticks'] = [{'t_ms': t, 'amount': amount, 'resisted': 0}
                  for t in range(a['amplitude_ms'], a['duration_ms'] + 1, a['amplitude_ms'])]
    return a


def _metadata(cat, ch, s):
    power = {0: 'mana', 1: 'rage', 3: 'energy'}[s['PowerType']]
    cost = spells._cost(s, ch)
    if power == 'rage':
        cost /= 10
    return {'spell': s['Id'], 'name': s['SpellName'], 'rank': s['Rank1'], 'school': s['School'],
            'icon': cat.icon(s), 'power': power, 'cost': cost,
            'cast_ms': int(F.apply_spell_mod(s, F.SPELLMOD_CASTING_TIME, cat.cast_time(s), ch.mods)),
            'gcd_ms': s['StartRecoveryTime'], 'cooldown_ms': spells._cooldown(s, ch),
            'interrupt_flags': s['InterruptFlags'], 'channel_interrupt_flags': s['ChannelInterruptFlags'],
            'events': [], 'auras': []}


def resolve(cat, ch, req, executing_swing=False):
    s, caster = cat.spell(req['spell']), req.get('caster', {})
    power, cp = state(caster)
    validate_form(s, caster)
    kind = ability(cat, s['Id'])
    self_spell = s['Id'] in FORM_SPELLS or s['Id'] in TALENT_FORM_SPELLS or kind in (5229, 22896, 9913, 9846, 9821, 9898, 5209)
    if req['target'].get('kind') != ('self' if self_spell else 'unit'):
        raise spells.CastError('SPELL_FAILED_BAD_TARGETS')
    spells._validate(cat, s, ch, req)
    out = _metadata(cat, ch, s)
    sid, rng = s['Id'], random.Random(req.get('seed', 0))
    if sid in FORM_SPELLS or sid in TALENT_FORM_SPELLS:
        form = FORM_SPELLS.get(sid, TALENT_FORM_SPELLS.get(sid))
        leaving = ch.form == form
        from service import character
        changed = character.build(cat, ch.items, ch.level, auras=caster.get('auras', []),
                                  form='none' if leaving else form, race=ch.race, cls=ch.cls,
                                  talents=ch.talents if ch.talent_mode else None,
                                  indoors=ch.indoors or req.get('ctx', {}).get('indoors', False))
        aura = _aura(cat, ch, s, 1, type='shapeshift', **profile(cat, changed))
        aura.update(action='leave' if leaving else 'enter', previous_form=ch.form,
                    removes=['root', 'removable_snare', 'water_walk'], clears_prowl=True,
                    interrupt_flags=s['AuraInterruptFlags'])
        if leaving:
            out.update(cost=0, gcd_ms=0)
        elif form in ('bear', 'dire_bear', 'cat'):
            aura['power_set'] = {aura['power']: 0}
            furor = next((a['amount'] for a in ch.auras if a['aura'] == 4 and
                          cat.spell(a['spell'])['SpellIconID'] == 238), 0) if ch.talent_mode else 0
            if furor and F.roll_chance(rng, furor):
                aura['power_set'][aura['power']] = 40 if form == 'cat' else 10
        out['auras'].append(aura)
        if form == 'cat' and not leaving:
            # HandleAuraModShapeshift triggers this without another cost/GCD.
            # The host checks learned knowledge and Find Herbs/Find Minerals.
            out['form_tracking'] = spells.resolve(cat, changed,
                {**req, 'spell': 5225, 'caster': {**caster, 'form': 'cat'}})
        return out
    # Finishers consume only the points the host attached to this request's target.
    if kind in (9896, 31018) and not cp:
        raise spells.CastError('SPELL_FAILED_NO_COMBO_POINTS')
    if power.get(out['power'], 0) < out['cost']:
        raise spells.CastError('SPELL_FAILED_NO_POWER: ERR_OUT_OF_' + out['power'].upper())
    if kind == 16979:
        out['events'].append({'type': 'charge', 'target': req['target'].get('id')})
        trigger = cat.spell(s['EffectTriggerSpell2'])
        result = spells.resolve(cat, ch, {**req, 'spell': trigger['Id']}, feral_dispatch=False)
        out['events'].extend(result['events'])
        out['auras'].extend(result['auras'])
        return out
    if s['Attributes'] & 0x20000 and not caster.get('prowling', False):
        raise spells.CastError('SPELL_FAILED_ONLY_STEALTHED')
    if kind in (9830, 9867) and not req['target'].get('behind', False):
        raise spells.CastError('SPELL_FAILED_NOT_BEHIND')
    if kind == 9913 and caster.get('in_combat', False):
        raise spells.CastError('SPELL_FAILED_AFFECTING_COMBAT')
    if kind == 17392:
        # The original spell path owns Nature binary resistance and typed FF auras.
        result = spells.resolve(cat, ch, req, feral_dispatch=False)
        result.update(power='energy', cost=out['cost'])
        if caster.get('prowling', False):
            result['breaks_prowl'] = True
        return result
    if kind == 9881 and not executing_swing:
        out.update(cost_deferred=True)
        out['events'].append({'type': 'next_swing', 'spell': sid, 'bonus_damage': _value(s, ch, 1, rng)})
        return out
    if kind == 5229:
        out['auras'].append(_aura(cat, ch, s, 1, type='periodic_power', power='rage',
                                  per_tick=_value(s, ch, 1, rng) / 10))
        trigger = linked_spell(cat, 25503)
        # EffectDummy passes custom points for effect 1, but the current core's
        # original_data row has its armor aura in effect 2. Preserve that row.
        e = next(e for e in spells._effects(trigger) if trigger[f'EffectApplyAuraName{e}'] == 101)
        amount = (-27 if ch.form == 'bear' else -16) if e == 1 else _value(trigger, ch, e, rng)
        out['auras'].append(_aura(cat, ch, trigger, e, type='armor_percent', amount=amount,
                                  school_mask=1, spell=25503))
        out['auras'].append(_aura(cat, ch, s, 3, type='interrupt_regen', power='rage', dummy_combat=True))
        return out
    if kind == 22896:
        # Each periodic trigger casts an ordinary self heal, including its own
        # crit roll (Spell.cpp:DoAllEffectOnTarget; SpellEffects.cpp:EffectHeal).
        a = _aura(cat, ch, s, 1, type='rage_heal', power='rage', max_rage_per_tick=10,
                  health_per_rage=F.apply_spell_mod(cat.spell(22845), F.SPELLMOD_DAMAGE, _value(s, ch, 1, rng), ch.mods), spell=22845,
                  crit_chance=ch.spell_crit(0), crit_multiplier=1.5)
        a['ticks'] = [{'t_ms': t, 'crit': F.roll_chance(rng, a['crit_chance'])}
                      for t in range(a['amplitude_ms'], a['duration_ms'] + 1, a['amplitude_ms'])]
        out['auras'].append(a)
        return out
    if kind in (9913, 9846, 9821):
        for e in spells._effects(s):
            aura_kind = {16: 'stealth', 33: 'movement_speed', 13: 'damage_done', 31: 'movement_speed'}[s[f'EffectApplyAuraName{e}']]
            aura = _aura(cat, ch, s, e, type=aura_kind, amount=_value(s, ch, e, rng))
            if aura_kind == 'damage_done':
                aura['school_mask'] = s[f'EffectMiscValue{e}']
            aura['ends_on_form_loss'] = True
            if aura_kind == 'stealth':
                aura.update(prowling=True, interrupt_flags=s['AuraInterruptFlags'])
            out['auras'].append(aura)
        return out
    if kind == 9898:
        a = _aura(cat, ch, s, 1, type='attack_power', amount=_value(s, ch, 1, rng),
                  area={'radius_yd': cat.radius_yd(s, 1), 'hostile': True, 'channel': False})
        out['auras'].append(a)
        return out
    if kind == 5209:
        # Aura 11 forces nearby enemies to attack; unlike Growl's Effect 114
        # this aura does not match top threat (HandleAuraModTaunt/ApplyTaunt).
        out['auras'].append(_aura(cat, ch, s, 1, type='taunt', force_attack=True,
                                  area={'radius_yd': cat.radius_yd(s, 1), 'hostile': True, 'channel': False}))
        return out
    t = {**req['target']}
    if t['kind'] != 'unit':
        raise spells.CastError('SPELL_FAILED_BAD_TARGETS')
    if kind == 6795:
        miss = F.spell_miss_chance(ch.level, t['level'], t.get('is_player', False), ch.spell_hit())
        if F.roll_chance(rng, miss):
            out['events'].append({'type': 'miss'})
        else:
            out['events'].append({'type': 'taunt', 'match_top_threat': True})
            out['auras'].append(_aura(cat, ch, s, 2, type='taunt', force_attack=True))
        return out
    area_swipe = kind == 9908 and 'targets' in req
    targets = req.get('targets', [t]) if kind == 9908 else [t]
    if len(targets) > 64:
        raise ValueError('too many targets')
    for target in targets[:s['EffectChainTarget1'] or 1]:
        target = {**target}
        if kind == 9908:
            spells._validate(cat, s, ch, {**req, 'target': target})
        if target.get('behind', False):
            target['in_front'] = False
        side = melee.ability_outcome(ch, s, target, rng)
        dest = [] if area_swipe else out['events']
        if side != 'hit':
            dest.append({'type': side})
            if kind != 9908 and s['AttributesEx'] & 0x8000000 and side in ('miss', 'dodge', 'parry'):
                out['power_refund'] = int(spells._cost(s, ch) * .8) / (10 if out['power'] == 'rage' else 1)
        else:
            if kind == 9892:
                dest.append({'type': 'threat', 'amount': _value(s, ch, 1, rng)})
            elif kind == 8983:
                out['auras'].append(_aura(cat, ch, s, 1, type='stun', breaks_on_damage=False, single_target=False))
            elif kind == 9827:
                out['auras'].append(_aura(cat, ch, s, 1, type='stun', breaks_on_damage=False, single_target=False))
                out['auras'].append(_bleed(cat, ch, cat.spell(s['EffectTriggerSpell2']), 1, rng, cp))
            elif kind == 9896:
                out['auras'].append(_bleed(cat, ch, s, 1, rng, cp))
            else:
                if kind in (9881, 9850, 9830, 9867):
                    low, high = ch.weapon_damage_range()
                    percent = ch.multiplier(79, 1)
                    low, high = low / percent, high / percent
                    base, multiplier = 0, 1
                    for e in spells._effects(s):
                        if s[f'Effect{e}'] == 58:
                            base += _value(s, ch, e, rng)
                        elif s[f'Effect{e}'] == 31:
                            multiplier *= _value(s, ch, e, rng) / 100
                            base = int(base * multiplier)
                    damage = (base + int(rng.randint(int(low), int(high)) * multiplier)) * percent
                    damage = max(0, int(F.apply_spell_mod(s, F.SPELLMOD_DAMAGE, damage, ch.mods)))
                else:
                    base = _value(s, ch, 1, rng, cp)
                    if kind == 31018:
                        extra = power.get('energy', 0) - out['cost']
                        base += int(extra * s['DmgMultiplier1']) + int(ch.attack_power() * .03 * cp)
                        out.update(extra_power_cost=extra)
                    damage = _bonus(ch, s, 1, base)
                dest.append(melee.ability_damage(cat, ch, s, target, damage, rng))
                if kind == 9904:
                    out['auras'].append(_bleed(cat, ch, s, 2, rng, cp))
            gained = sum(_value(s, ch, e, rng) for e in spells._effects(s) if s[f'Effect{e}'] == 80)
            if gained:
                out['combo_points'] = min(gained, 5 - cp)
            if kind in (9896, 31018):
                out['combo_points_spent'] = cp
        if area_swipe:
            out.setdefault('results', []).append({'id': target['id'], **dest[0]})
    if caster.get('prowling', False):
        out['breaks_prowl'] = True
    return out
