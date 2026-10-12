"""Spell resolution: one cast request -> a deterministic (seeded) outcome the Skyrim plugin applies exactly once.

Pipeline order follows CMaNGOS: validation -> cost/cast time/GCD (spell mods) -> MagicSpellHitResult for harmful
spells -> per-effect values (CalculateSpellEffectValue) -> caster bonuses (SpellDamageBonusDone /
SpellHealingBonusDone) -> crit (Unit::CalculateSpellDamage) -> partial resist (CalculateDamageAbsorbAndResist).
Periodic amounts are snapshotted at application (SpellAuras.cpp HandlePeriodicDamage/Heal) and every tick
re-rolls partial resist (Aura::PeriodicTick). Skyrim targets carry no WoW auras, so "taken" modifiers are identity.
"""
import random

from service import formulas as F
from service.character import Character, A_PERIODIC_DAMAGE, A_PERIODIC_HEAL, A_MOD_ROOT, APPLY_AURA

E_SCHOOL_DAMAGE, E_HEAL, E_PERSISTENT_AREA_AURA = 2, 10, 27
E_AREA_AURA_PARTY, E_DISPEL, E_RESURRECT_NEW = 35, 38, 113
DISPELS = {1: 'Magic', 2: 'Curse', 3: 'Disease', 4: 'Poison', 5: 'Stealth', 6: 'Invisibility'}
# SharedDefines.h: CreatureType (1.12 client names), mask bit = type - 1.
CREATURE_TYPES = {name: 1 << i for i, name in enumerate(
    ('Beast', 'Dragonkin', 'Demon', 'Elemental', 'Giant', 'Undead', 'Humanoid', 'Critter', 'Mechanical'))}
ATTR_OUTDOORS_ONLY = 0x8000
ATTR_HEARTBEAT_RESIST = 0x40000000
CUSTOM_IDOL_OF_REJUVENATION = 4415   # OVERRIDE_CLASS_SCRIPTS id; unhandled in CMaNGOS -> WoWBridge custom rule


class CastError(Exception):
    """SpellCastResult-style failure; the plugin maps the code to WoW's own error string."""


def _effects(s: dict) -> list[int]:
    return [e for e in (1, 2, 3) if s[f'Effect{e}']]


def is_positive(s: dict) -> bool:
    for e in _effects(s):
        if s[f'Effect{e}'] == E_HEAL or s[f'EffectApplyAuraName{e}'] == A_PERIODIC_HEAL:
            return True
    # SpellMgr.h:IsPositiveEffect/IsPositiveEffectTargetMode: these book spells use
    # caster (1), friendly unit (21), or target party (37) for every effect.
    return all(s[f'EffectImplicitTargetA{e}'] in (1, 21, 37) or s[f'Effect{e}'] == E_RESURRECT_NEW
               for e in _effects(s))


def _school_mask(s: dict) -> int:
    return 1 << s['School']


def _healing_benefit(ch: Character, cat, s: dict) -> int:
    benefit = ch.healing_done(_school_mask(s))
    # WoWBridge custom rule (labelled): Idol of Rejuvenation's class script 4415 is not implemented by CMaNGOS.
    # 1.12 tooltip: "Increases healing done by Rejuvenation by up to 50" -> treated as +50 bonus healing for
    # spells in its affect mask (spell_affect 27853: family mask 0x10 = Rejuvenation), scaled by the coefficient.
    for a in ch.class_scripts(CUSTOM_IDOL_OF_REJUVENATION) if ch.custom_rules else ():
        if int(s['SpellFamilyFlags']) & cat.affect_mask(a['spell'], a['eff'] - 1):
            benefit += a['amount']
    return benefit


def _damage_done(ch, s, eff, base, dot: bool) -> int:
    """Unit::SpellDamageBonusDone (Unit.cpp:7031) for a player with no damage-percent auras."""
    total = F.bonus_with_coeffs(s, eff, 0, ch.spell_damage(_school_mask(s)), ch.mods)
    dmg = (base + total) * ch.multiplier(79, _school_mask(s))
    dmg = F.apply_spell_mod(s, F.SPELLMOD_DOT if dot else F.SPELLMOD_DAMAGE, dmg, ch.mods)
    return max(0, int(dmg))


def _healing_done(ch, cat, s, eff, base, dot: bool) -> int:
    """Unit::SpellHealingBonusDone (Unit.cpp:7193)."""
    total = F.bonus_with_coeffs(s, eff, 0, _healing_benefit(ch, cat, s), ch.mods)
    heal = F.apply_spell_mod(s, F.SPELLMOD_DOT if dot else F.SPELLMOD_DAMAGE,
                            (base + total) * ch.multiplier(136), ch.mods)
    return max(0, int(heal))


def _resist_pct(ch, s, target, binary) -> float:
    res = int(target.get('resist', {}).get(str(s['School']), 0))
    return F.effective_resist_percent(ch.level, target['level'], res, binary, not target.get('is_player', False))


def _validate(cat, s, ch, req) -> None:
    t, ctx = req['target'], req.get('ctx', {})
    if s['Attributes'] & ATTR_OUTDOORS_ONLY and ctx.get('indoors'):
        raise CastError('SPELL_FAILED_ONLY_OUTDOORS')
    dead = t.get('dead', not t.get('alive', True))
    resurrect = any(s[f'Effect{e}'] == E_RESURRECT_NEW for e in _effects(s))
    # Spell::CheckCast (Spell.cpp:4824), Spell::CheckTarget: resurrection needs a
    # dead target, while the remaining book requires a living unit.
    if resurrect and not dead:
        raise CastError('SPELL_FAILED_TARGET_NOT_DEAD')
    if not resurrect and dead:
        raise CastError('SPELL_FAILED_TARGETS_DEAD')
    if s['TargetCreatureType'] and not s['TargetCreatureType'] & CREATURE_TYPES.get(t.get('creature_type'), 0):
        # Spell::CheckTargetCreatureType (Spell.cpp:6908).
        raise CastError('SPELL_FAILED_BAD_TARGETS')
    if resurrect and t['kind'] != 'unit':
        raise CastError('SPELL_FAILED_BAD_TARGETS')
    if t['kind'] == 'self':
        _validate_reagents(s, req)
        return
    if t['kind'] in ('unit', 'ground'):
        lo, hi = cat.range_yd(s)
        hi = F.apply_spell_mod(s, 5, hi, ch.mods) if ch.talent_mode else hi
        d = float(t['distance_yd'])
        if d > hi or d < lo:
            raise CastError('SPELL_FAILED_OUT_OF_RANGE')
        # Spell::CheckRange (Spell.cpp:6246): players face what spell_facing lists (HasInArc: the front half).
        if t['kind'] == 'unit' and cat.needs_facing(s['Id']) and not t.get('in_front', True):
            raise CastError('SPELL_FAILED_UNIT_NOT_INFRONT')
        if not t.get('los', True):
            raise CastError('SPELL_FAILED_LINE_OF_SIGHT')
    if t['kind'] == 'unit':
        # Spell::CheckTarget: a helpful spell wants a friend, a harmful one a unit she can attack. In WoW those are the
        # same split; Skyrim lets her strike a friendly townsperson too, so the plugin says so (attackable).
        friendly = t.get('friendly', t.get('reaction') == 'friendly')
        attackable = t.get('attackable', not friendly)
        healable = t.get('healable', friendly)   # the plugin: anything not hostile (the user: a chicken takes a heal)
        # User policy: Rebirth may revive hostile corpses. Keep normal friendly
        # healing rules and all resurrection life-state/range/LOS/reagent checks.
        if (is_positive(s) and not resurrect and not healable) or (not is_positive(s) and not attackable):
            raise CastError('SPELL_FAILED_BAD_TARGETS')
    _validate_reagents(s, req)


def _reagents(s):
    return [{'item': s[f'Reagent{e}'], 'count': s[f'ReagentCount{e}']}
            for e in range(1, 9) if s.get(f'Reagent{e}', 0) > 0 and s.get(f'ReagentCount{e}', 0) > 0]


def _validate_reagents(s, req):
    # Spell::CheckItems (Spell.cpp:6558): the plugin owns inventory and consumes
    # the listed quantities once a cast succeeds. JSON object keys are strings.
    inventory = req.get('caster', {}).get('reagents', {})
    for reagent in _reagents(s):
        if inventory.get(str(reagent['item']), inventory.get(reagent['item'], 0)) < reagent['count']:
            raise CastError('SPELL_FAILED_REAGENTS')


def _cost(s, ch):
    # Spell::CalculatePowerCost (Spell.cpp:6331-6364): percentage is of CREATE
    # mana, not gear/intellect max mana, and integer division precedes spell mods.
    base = s['ManaCost'] + s.get('ManaCostPerlevel', 0) * (F.spell_rank_level(s, ch.level) - s['BaseLevel'])
    base += s['ManaCostPercentage'] * ch.base_mana // 100
    return max(0, int(F.apply_spell_mod(s, F.SPELLMOD_COST, base, ch.mods)))


def _cooldown(s: dict, ch: Character) -> int:
    """Player::AddCooldown (Player.cpp:20196): the spell's own recovery and its category's, each with the cooldown
    spell mods; the spell is ready again when both have passed."""
    rec = int(F.apply_spell_mod(s, F.SPELLMOD_COOLDOWN, s['RecoveryTime'], ch.mods)) if s['RecoveryTime'] else 0
    cat = int(F.apply_spell_mod(s, F.SPELLMOD_COOLDOWN, s['CategoryRecoveryTime'], ch.mods)) \
        if s['Category'] and s['CategoryRecoveryTime'] else 0
    return max(rec, cat, 0)


def resolve(cat, ch: Character, req: dict, feral_dispatch=True, triggered=False) -> dict:
    s = cat.spell(req['spell'])
    if 'talents' in req.get('caster', {}):
        if s['Attributes'] & 0x40:
            raise CastError('SPELL_FAILED_SPELL_UNAVAILABLE')
        from service.talents import book
        _, definition = book().class_definition(ch.cls)
        for talent in book().rows(definition).values():
            root = s['Id'] if s['Id'] in talent['rank_spells'] else cat.chain(s['Id'])
            if root in talent['rank_spells']:
                if ch.talents.get(talent['id'], 0) < talent['rank_spells'].index(root) + 1:
                    raise CastError('SPELL_FAILED_NOT_KNOWN')
    from service import feral
    from service import racials
    # Spell::CheckCast deliberately omits stance restrictions for triggered
    # spells: Grasp cast before shifting must still root the attacker in Cat.
    if not triggered:
        feral.validate_form(s, req.get('caster', {}))
    racials.validate(s, ch, req)
    kind = feral.ability(cat, s['Id'])
    if feral_dispatch and ((kind is not None and (kind not in feral.TALENT_ABILITIES or ch.talent_mode)) or
                           s['Id'] in feral.FORM_SPELLS or (ch.talent_mode and s['Id'] in feral.TALENT_FORM_SPELLS)):
        return feral.resolve(cat, ch, req)
    rng = random.Random(req.get('seed', 0))
    if not triggered:
        _validate(cat, s, ch, req)
    # Proc targets have already passed the parent's targeting checks. In
    # particular Starfire Stun has a zero DBC range, not a new manual cast gate.
    t = req['target']
    cast_ms = int(F.apply_spell_mod(s, F.SPELLMOD_CASTING_TIME, cat.cast_time(s), ch.mods))
    out = {
        'spell': s['Id'], 'name': s['SpellName'], 'rank': s['Rank1'], 'school': s['School'], 'icon': cat.icon(s),
        'cost': _cost(s, ch),
        'cast_ms': max(0, cast_ms) if ch.talent_mode else cast_ms,
        'gcd_ms': s['StartRecoveryTime'], 'cooldown_ms': _cooldown(s, ch),
        # What interrupts it (the plugin owns the cast): InterruptFlags 0x1 movement, 0x2 pushback, 0x10 damage
        # cancels; a channel's ChannelInterruptFlags 0x8 movement, 0x2 damage ends it, 0x4000 damage shortens it.
        'interrupt_flags': s['InterruptFlags'], 'channel_interrupt_flags': s['ChannelInterruptFlags'],
        'events': [], 'auras': [],
    }
    if ch.talent_mode:
        from service.talent_effects import mechanics
        out.update(mechanics(ch, s))
    if _reagents(s):
        out['reagents'] = _reagents(s)
    positive = is_positive(s)
    heartbeat_pct = 0.0
    # SpellMgr.h:IsBinarySpell: pure non-damage auras are binary. Keep the
    # original six spells' hit/seed behavior, extending this to new debuffs.
    binary = any(s[f'EffectApplyAuraName{e}'] in (A_MOD_ROOT, 12, 22, 91) for e in _effects(s))
    if not positive and t['kind'] == 'unit':
        # Unit::MagicSpellHitResult (Unit.cpp:2872): miss and full resist share one die side pre-WotLK.
        miss = F.spell_miss_chance(ch.level, t['level'], t.get('is_player', False),
                                   F.apply_spell_mod(s, F.SPELLMOD_RESIST_MISS_CHANCE, ch.spell_hit(), ch.mods))
        # Unit::CalculateSpellResistChance (Unit.cpp:4010): on the same side of the die, a binary spell is resisted by
        # its resistance percent, a player's other spell fully by the distribution's 100% column (Unit.cpp:4018-4022).
        pct = _resist_pct(ch, s, t, binary)
        full = pct if binary else (F.partial_resist_distribution()[int(pct * 100)][4] * 0.01 if pct > 0 else 0.0)
        resist_chance = miss + full
        out['hit_chance'] = 100.0 - resist_chance
        if F.roll_chance(rng, resist_chance):
            out['events'].append({'type': 'resist' if binary or rng.random() * resist_chance >= miss else 'miss'})
            return out
        heartbeat_pct = resist_chance
    duration = int(F.apply_spell_mod(s, F.SPELLMOD_DURATION, cat.duration(s, ch.level), ch.mods))
    crit_chance = F.apply_spell_mod(s, F.SPELLMOD_CRITICAL_CHANCE, ch.spell_crit(s['School']), ch.mods)
    crit = F.roll_chance(rng, crit_chance)   # rolled once per cast (CalculateSpellDamage / Spell::DoAllEffectOnTarget)
    out['crit_chance'] = crit_chance
    attacker_is_player = True
    for e in _effects(s):
        effect, aura = s[f'Effect{e}'], s[f'EffectApplyAuraName{e}']
        base = int(F.effect_value(s, e, ch.level, rng, mods=ch.mods))
        if effect == E_SCHOOL_DAMAGE:
            dmg = _damage_done(ch, s, e, base, dot=False)
            if crit:
                multiplier = F.crit_multiplier(s['DmgClass'])
                if ch.talent_mode:
                    multiplier = 1 + F.apply_spell_mod(s, F.SPELLMOD_CRIT_DAMAGE_BONUS, multiplier - 1, ch.mods)
                dmg = max(dmg, int(dmg * multiplier))
            frac = F.partial_resist_multiplier(_resist_pct(ch, s, t, binary), False, binary, attacker_is_player, rng)
            resisted = int(dmg * frac)
            out['events'].append({'type': 'damage', 'amount': dmg - resisted, 'resisted': resisted, 'crit': crit, 'school': s['School']})
        elif effect == E_HEAL:
            if s['Id'] == 18562 and ch.talent_mode:
                hots = t.get('hots', [])
                if not isinstance(hots, list):
                    raise ValueError('target.hots must be an array')
                candidates = []
                from service.progression import number
                for hot in hots:
                    if not isinstance(hot, dict) or hot.get('spell') not in cat.spells:
                        raise ValueError('target.hots requires a catalog spell')
                    hs = cat.spell(hot['spell'])
                    if hs['SpellFamilyName'] == 7 and int(hs['SpellFamilyFlags']) & 0x50:
                        he = next((i for i in (1, 2, 3) if hs[f'EffectApplyAuraName{i}'] == 8), None)
                        if he:
                            remaining = number(hot.get('remaining_ms'), 'target.hots remaining_ms')
                            amount = number(hot.get('per_tick'), 'target.hots per_tick')
                            if remaining > 0 and amount >= 0:
                                candidates.append((remaining, hot['spell'], he, amount))
                if not candidates:
                    raise CastError('SPELL_FAILED_TARGET_AURASTATE')
                _, hot_id, he, amount = min(candidates)
                hs = cat.spell(hot_id)
                # The 1.12 client promises 12 seconds of Rejuvenation or 18
                # of Regrowth. CMaNGOS uses the whole original aura duration,
                # incorrectly granting all 21 seconds of Regrowth here.
                window_ms = 12000 if int(hs['SpellFamilyFlags']) & 0x10 else 18000
                base += int(amount * window_ms / hs[f'EffectAmplitude{he}'])
                out['consumed_target_hot'] = hot_id
            heal = _healing_done(ch, cat, s, e, base, dot=False)
            if crit:
                multiplier = F.crit_multiplier(s['DmgClass'])
                if ch.talent_mode:
                    multiplier = 1 + F.apply_spell_mod(s, F.SPELLMOD_CRIT_DAMAGE_BONUS, multiplier - 1, ch.mods)
                heal = int(heal * multiplier)
            out['events'].append({'type': 'heal', 'amount': heal, 'crit': crit, 'school': s['School']})
        elif effect == E_DISPEL:
            # SpellEffects.cpp:Spell::EffectDispel/ProcessDispelList: base is
            # the number of removal attempts; the plugin selects matching effects.
            out['events'].append({'type': 'dispel', 'dispel': DISPELS[s[f'EffectMiscValue{e}']], 'count': base})
        elif effect == 68 and ch.talent_mode:
            out['events'].append({'type': 'interrupt', 'school_lockout_ms': cat.duration(s, ch.level)})
        elif effect == E_RESURRECT_NEW:
            # SpellEffects.cpp:Spell::EffectResurrect (4820): fixed amounts,
            # without healing coefficients or crit (unlike percent resurrection).
            out['events'].append({'type': 'resurrect', 'health': base, 'mana': s[f'EffectMiscValue{e}']})
        elif effect in (APPLY_AURA, E_PERSISTENT_AREA_AURA, E_AREA_AURA_PARTY):
            amp = s[f'EffectAmplitude{e}']
            a = {'eff': e, 'aura': aura, 'duration_ms': duration, 'amplitude_ms': amp, 'stack': 'replace_same_caster'}
            if aura == A_PERIODIC_DAMAGE:
                a['per_tick'] = _damage_done(ch, s, e, base, dot=True)
                a['type'] = 'damage'
            elif aura == A_PERIODIC_HEAL:
                a['per_tick'] = _healing_done(ch, cat, s, e, base, dot=True)
                a['type'] = 'heal'
            elif aura == A_MOD_ROOT:
                a['type'] = 'root'
                _heartbeat(a, s, positive, heartbeat_pct, duration, rng)
                a['damage_break'] = {'victim_level': t.get('level', ch.level)}
            elif aura == 12:
                # SpellAuras.cpp:Aura::HandleAuraModStun; Unit::DealDamage
                # removes AURA_INTERRUPT_FLAG_DAMAGE (0x2) even for periodic damage.
                a.update(type='stun', breaks_on_damage=bool(s['AuraInterruptFlags'] & 2),
                         single_target=bool(s['Mechanic'] == 10 and s['SpellFamilyFlags']))
                _heartbeat(a, s, positive, heartbeat_pct, duration, rng)
            elif aura == 15:
                # Unit.cpp:Unit::DealMeleeDamage (2280-2283): raw aura amount
                # and DAMAGE spell mods, no spell-power coefficient or crit.
                a.update(type='damage_shield', amount=max(0, int(F.apply_spell_mod(s, F.SPELLMOD_DAMAGE, base, ch.mods))),
                         school=s['School'])
            elif aura == 23:
                # SpellAuras.cpp:Aura::HandlePeriodicTriggerSpell/PeriodicTick:
                # Abolish Poison triggers 3137, whose effect is a one-poison dispel.
                trigger = cat.spell(s[f'EffectTriggerSpell{e}'])
                te = next(i for i in _effects(trigger) if trigger[f'Effect{i}'] == E_DISPEL)
                a.update(type='periodic_dispel', spell=trigger['Id'],
                         dispel=DISPELS[trigger[f'EffectMiscValue{te}']],
                         count=int(F.effect_value(trigger, te, ch.level, rng, mods=ch.mods)))
            elif aura == 22:
                a.update(type='armor' if s[f'EffectMiscValue{e}'] == 1 else 'resistance',
                         amount=base, school_mask=s[f'EffectMiscValue{e}'])
            elif aura == 16 and s['Id'] == racials.SHADOWMELD:
                a.update(type='stealth', amount=base, shadowmeld=True,
                         interrupt_flags=s['AuraInterruptFlags'])
            elif aura in (44, 45, 151):
                # SpellAuras.cpp:HandleAuraTrackCreatures/Resources/Stealthed.
                misc = s[f'EffectMiscValue{e}']
                a.update(type={44: 'track_creatures', 45: 'track_resources', 151: 'track_stealthed'}[aura],
                         misc=misc, mask=1 << (misc - 1) if aura != 151 and 1 <= misc <= 32 else 0,
                         stack='replace_tracking')
                # IsRemovedOnShapeshiftLost: self-cast stance-bound positive auras.
                a['ends_on_form_loss'] = bool(s['Stances'] and not s['Attributes'] & 0x10000
                                             and not s['AttributesEx2'] & 0x80000)
            elif aura == 29:
                a.update(type='stat', amount=base, stat=s[f'EffectMiscValue{e}'])
            elif aura == 143:
                a.update(type='resistance', amount=base, school_mask=s[f'EffectMiscValue{e}'], exclusive=True)
            elif aura == 41:
                # SpellAuras.cpp:Aura::HandleAuraModDispelImmunity: Faerie Fire
                # prevents application of Stealth and Invisibility auras.
                a.update(type='dispel_immunity', dispel=DISPELS[s[f'EffectMiscValue{e}']])
            elif aura == 87:
                a.update(type='damage_taken_percent', amount=base, school_mask=s[f'EffectMiscValue{e}'])
            elif aura == 91:
                # Unit.cpp:Unit::GetAttackDistance: yards added to detection range.
                a.update(type='detect_range', amount=base)
            elif aura in (107, 108):
                a.update(type='spell_mod', amount=base, op=s[f'EffectMiscValue{e}'],
                         modifier='flat' if aura == 107 else 'percent', family=s['SpellFamilyName'],
                         mask=cat.affect_mask(s['Id'], e - 1))
            elif aura == 110:
                a.update(type='mana_regen_percent', amount=base, power=s[f'EffectMiscValue{e}'])
            elif aura == 134:
                a.update(type='mana_regen_interrupt', amount=base)
            elif aura == 149:
                # Spell.cpp:Spell::Delayed/DelayedChannel: chance to avoid damage
                # pushback, 100% for Barkskin. HandleSpellSpecificBoosts (SpellAuras.cpp:
                # 5617-5618) returns for DRUID: it does NOT cast tooltip spell 22839.
                a.update(type='pushback_immunity', amount=base, school_mask=s[f'EffectMiscValue{e}'])
            elif aura == 42 and ch.talent_mode:
                a.update(type='proc', spell=s['Id'], trigger_spell=s[f'EffectTriggerSpell{e}'])
            elif aura == 54 and ch.talent_mode:
                a.update(type='hit_chance', amount=base)
            else:
                a['type'] = f'aura_{aura}'
                a['amount'] = base
            if effect == E_PERSISTENT_AREA_AURA:
                a['area'] = {'radius_yd': cat.radius_yd(s, e), 'channel': True}
            elif effect == E_AREA_AURA_PARTY or s[f'EffectImplicitTargetA{e}'] == 37:
                # SpellEffects.cpp:Spell::EffectApplyAreaAura; target 37 applies
                # Gift to the selected party, effect 35 maintains Tranquility's party aura.
                a['area'] = {'radius_yd': cat.radius_yd(s, e), 'party': True,
                             'channel': bool(s['AttributesEx'] & 0x40)}
            if a.get('per_tick') is not None and amp and effect == APPLY_AURA and a['type'] == 'damage':
                a['ticks'] = []
                for k in range(1, duration // amp + 1):         # Aura::PeriodicTick: partial resist each tick
                    frac = F.partial_resist_multiplier(_resist_pct(ch, s, t, binary), True, binary, attacker_is_player, rng)
                    r = int(a['per_tick'] * frac)
                    a['ticks'].append({'t_ms': k * amp, 'amount': a['per_tick'] - r, 'resisted': r})
            elif a.get('per_tick') is not None and amp and effect == APPLY_AURA:
                a['ticks'] = [{'t_ms': k * amp, 'amount': a['per_tick']} for k in range(1, duration // amp + 1)]
            out['auras'].append(a)
    return out


def _heartbeat(a, s, positive, heartbeat_pct, duration, rng):
    if s['Attributes'] & ATTR_HEARTBEAT_RESIST and not positive:
        chance, interval = F.heartbeat(heartbeat_pct, duration)
        a['heartbeat'] = {'chance': chance, 'interval_ms': interval,
                          'breaks_at_ms': next((k * interval for k in range(1, duration // interval + 1)
                                                if F.roll_chance(rng, chance)), None)}


def tick_area(cat, ch: Character, req: dict) -> dict:
    """One tick of Hurricane damage or Tranquility healing against units the plugin found in the area.
    Hurricane re-rolls hit per target; a miss is silent, and a hit gets a partial
    resist roll (SpellAuras.cpp:Aura::PeriodicTick). Tranquility applies healing
    bonuses without hit, resistance or critical rolls."""
    s = cat.spell(req['spell'])
    rng = random.Random(req.get('seed', 0))
    eff = next(e for e in _effects(s) if s[f'Effect{e}'] in (E_PERSISTENT_AREA_AURA, E_AREA_AURA_PARTY)
               and s[f'EffectApplyAuraName{e}'] in (A_PERIODIC_DAMAGE, A_PERIODIC_HEAL))
    healing = s[f'EffectApplyAuraName{eff}'] == A_PERIODIC_HEAL
    base = int(F.effect_value(s, eff, ch.level, rng, mods=ch.mods))
    per_tick = _healing_done(ch, cat, s, eff, base, dot=True) if healing else _damage_done(ch, s, eff, base, dot=True)
    results = []
    for t in req['targets']:
        if healing:
            # SpellAuras.cpp:Aura::PeriodicTick (PERIODIC_HEAL): living party
            # members receive the heal without a hit, resistance or critical roll.
            if t.get('dead', not t.get('alive', True)) or not t.get('friendly', t.get('reaction', 'friendly') == 'friendly'):
                continue
            results.append({'id': t['id'], 'type': 'heal', 'amount': per_tick, 'school': s['School']})
            continue
        miss = F.spell_miss_chance(ch.level, t['level'], t.get('is_player', False),
                                   F.apply_spell_mod(s, F.SPELLMOD_RESIST_MISS_CHANCE, ch.spell_hit(), ch.mods))
        if F.roll_chance(rng, miss):
            results.append({'id': t['id'], 'type': 'miss', 'silent': True})
            continue
        frac = F.partial_resist_multiplier(_resist_pct(ch, s, t, False), True, False, True, rng)
        r = int(per_tick * frac)
        results.append({'id': t['id'], 'type': 'damage', 'amount': per_tick - r, 'resisted': r, 'school': s['School']})
    out = {'spell': s['Id'], 'tick': req.get('tick', 0), 'results': results}
    if ch.talent_mode:
        from service.talent_effects import mechanics
        out.update(mechanics(ch, s))
    return out


def damage_taken(req: dict) -> dict:
    """Break-on-damage roll for a damage-breakable aura (Unit::HandleRemoveByDamageChanceProc)."""
    rng = random.Random(req.get('seed', 0))
    chance = F.damage_break_chance(int(req['damage']), int(req['victim_level']))
    return {'break': F.roll_chance(rng, chance), 'chance': chance}
