"""Attacker talent procs and charged buffs; the plugin owns cooldowns/auras.

spell_proc_event overrides inherit by rank as in SpellMgr. The plugin supplies
incoming weapon hits for Nature's Grasp and owns aura/charge lifetime.
"""
import random

from service import formulas as F
from service.progression import number


class _LowRoll:
    def randint(self, lo, hi):
        return lo


def mechanics(ch, spell):
    # Spell::Delayed initializes percent modifiers at 100, then subtracts 100.
    return {'pushback_resist_pct': min(100, max(0, F.apply_spell_mod(spell, 9, 100, ch.mods) - 100)),
            'threat_multiplier': max(0, F.apply_spell_mod(spell, 2, 100, ch.mods)) / 100}


def incoming(cat, ch, req):
    """Nature's Grasp: a landed incoming melee/ranged weapon hit, not our cast."""
    from service import spells
    out = {'events': [], 'auras': []}
    if not req.get('weapon_hit', False):
        return out
    rng = random.Random(req.get('seed', 0))
    for a in ch.auras:
        if a['aura'] != 42 or cat.chain(a['spell']) != 16689:
            continue
        s = cat.spell(a['spell'])
        chance = min(100, max(0, F.apply_spell_mod(s, 18, s['ProcChance'], ch.mods)))
        out['proc_chance'] = chance
        if not F.roll_chance(rng, chance):
            continue
        sid = s[f'EffectTriggerSpell{a["eff"]}']
        triggered = spells.resolve(cat, ch, {**req, 'spell': sid}, feral_dispatch=False, triggered=True)
        triggered['tooltip'] = tooltip(cat, ch, cat.spell(sid))
        out['consumed_auras'] = [a['spell']]
        out['procs'] = [{'source_spell': a['spell'], 'spell': sid, 'target': 'target',
                         'cooldown_ms': 0, **triggered}]
        break
    return out


def tooltip(cat, ch, spell):
    """Unconverted, noncritical caster values, using the same formulas as resolve.

    The client converts health tokens to Skyrim units. Buffs retain this snapshot
    at application rather than borrowing a later talent allocation. Native caster
    ranks normalize bonus additions before their final health-factor conversion.
    """
    from service import feral, paladin, spells, skyrim
    effects = []
    for eff in (1, 2, 3):
        bases = [spells._effect_base(ch, spell, eff, F.effect_value(spell, eff, ch.level, _LowRoll())),
                 spells._effect_base(ch, spell, eff, F.effect_value(spell, eff, ch.level, None, maximum=True))]
        values = [v for v, _ in bases]
        effect, aura = spell[f'Effect{eff}'], spell[f'EffectApplyAuraName{eff}']
        if feral.ability(cat, spell['Id']) == 9881 and effect == 58:
            # The queued bonus is _value, before the subsequent melee swing's
            # damage modifiers. Match next_swing.bonus_damage exactly.
            values = [feral.floor_base(ch, spell, eff, raw, value) for value, raw in bases]
        elif feral.ability(cat, spell['Id']) in (9908, 9850, 9830, 9904, 9896, 31018, 9867) and (effect in (2, 58) or aura == 3):
            # Feral attacks use melee/AP bonuses, never caster spell power.
            # Weapon abilities display their additional damage, not the whole
            # weapon roll; percent weapon multipliers/combo tokens stay intact.
            values = [feral._bonus(ch, spell, eff, v, dot=aura == 3, raw_base=raw) for v, raw in bases]
        elif effect == 2 or aura == 3:
            values = [spells._damage_done(ch, spell, eff, v, dot=aura == 3, raw_base=raw) for v, raw in bases]
        elif effect == 10 or aura == 8:
            values = [spells._healing_done(ch, cat, spell, eff, v, dot=aura == 8, raw_base=raw) for v, raw in bases]
        elif aura == 15:
            values = [max(0, int(F.apply_spell_mod(spell, F.SPELLMOD_DAMAGE, v, ch.mods))) for v in values]
        elif effect == 77 and (linked := paladin.script_heal(spell['Id'])) in cat.spells:
            # Holy Light / Flash of Light heal through 19968 / 19993 (paladin.linked_heal), noncritical.
            values = [paladin.linked_heal(cat, ch, spell, linked, v, raw, None)[0] for v, raw in bases]
        elif effect == 67 and spell['SpellFamilyName'] == paladin.SPELLFAMILY_PALADIN:
            values = [ch.max_health] * 2   # Lay on Hands: the caster's maximum health (Spell::EffectHealMaxHealth)
        if ch.native_mode and spell['Id'] == 29166 and aura == 110:
            native = cat.balance[29166].native['mana_regen_percent'][0]
            values = [v * native[1] / native[2] for v in values]
        if ch.native_mode and aura == 142 and spell['Id'] in feral.LINKED[ch.form]:
            values = [feral.native_armor_percent(cat, ch)] * 2
        effects.append(sorted(abs(v) for v in values))
    profile = {'effects': effects,
            'cast_ms': max(0, int(F.apply_spell_mod(spell, F.SPELLMOD_CASTING_TIME, cat.cast_time(spell), ch.mods))),
            'duration_ms': int(F.apply_spell_mod(spell, F.SPELLMOD_DURATION, cat.duration(spell, ch.level), ch.mods)),
            'range_max': F.apply_spell_mod(spell, 5, cat.range_yd(spell)[1], ch.mods),
            'proc_chance': min(100, max(0, F.apply_spell_mod(spell, 18, spell['ProcChance'], ch.mods))),
            'cooldown_ms': spells._cooldown(spell, ch)}

    rule = cat.balance.get(spell['Id'])
    if rule is not None:
        caster = {'skyrim': ch.skyrim_skills, 'equipment_modifiers': ch.equipment_modifiers} if ch.native_mode else {}
        profile.update(health_factor=skyrim.health_factor(rule, caster), minimum_health=rule.minimum_health)
    if ch.native_mode:
        form = feral.FORM_SPELLS.get(spell['Id'], feral.TALENT_FORM_SPELLS.get(spell['Id']))
        if form:
            profile['linked_spells'] = {str(sid): tooltip(cat, ch, cat.spell(sid)) for sid in feral.LINKED[form]}
            # Preview the destination form, including unlearned spellbook rows,
            # without treating the player's current form bonus as a talent.
            armor = feral.native_armor_percent(cat, ch, form)
            for sid in feral.LINKED[form]:
                linked = cat.spell(sid)
                for eff in (1, 2, 3):
                    if linked[f'EffectApplyAuraName{eff}'] == 142:
                        profile['linked_spells'][str(sid)]['effects'][eff-1] = [armor, armor]
    if paladin.is_seal(spell['Id']):
        profile['linked_spells'] = paladin.seal_tooltips(cat, ch, spell)
    from service import paladin_talents
    if (holy_shock := paladin_talents.linked_tooltips(cat, ch, spell)) is not None:
        profile['linked_spells'] = holy_shock   # $25912s1 / $25914s1: the rank's damage and heal
    if feral.ability(cat, spell['Id']) == 22896:
        # Health per rage is a fractional rate; round only the completed tick.
        profile['health_rates'] = [True, False, False]
    if feral.ability(cat, spell['Id']) == 9827:
        # Pounce's description references its triggered bleed. Snapshot that
        # bleed's AP bonus and timing along with the parent cast's conversion.
        linked = cat.spell(spell['EffectTriggerSpell2'])
        bleed = tooltip(cat, ch, linked)
        for eff in (1, 2, 3):
            if linked[f'EffectApplyAuraName{eff}'] == 3:
                bleed['effects'][eff - 1] = [feral._bonus(ch, linked, eff,
                    int(F.effect_value(linked, eff, ch.level, roll, maximum=maximum, mods=ch.mods)), dot=True,
                    raw_base=int(F.effect_value(linked, eff, ch.level, roll, maximum=maximum)))
                    for roll, maximum in ((_LowRoll(), False), (None, True))]
        profile['linked_spells'] = {str(linked['Id']): bleed}
    return profile


def finish(cat, ch, req, out):
    if 'talents' not in req.get('caster', {}) or out.get('cost_deferred'):
        return out
    caster = req.get('caster', {})
    from service import feral
    out['feral_threat_bonus_percent'] = feral.profile(cat, ch)['feral_threat_bonus_percent']
    cooldowns = caster.get('proc_cooldowns', {})
    if not isinstance(cooldowns, dict):
        raise ValueError('caster.proc_cooldowns must be an object')
    # Spell::cast / TakePower: on-next-melee modifiers are consumed by
    # execution, never by queueing (which returned above as cost_deferred).
    sid = req.get('spell') if req.get('op') == 'resolve' else out.get('spell')
    s = cat.spell(sid) if sid else None
    consumed = set()
    if s:
        for a in ch.auras:
            source = cat.spell(a['spell'])
            if source['ProcCharges'] <= 0:
                continue
            mods = [m for m in ch.mods if m['spell'] == a['spell'] and F.affects(m, s)]
            if any(m['op'] == F.SPELLMOD_CASTING_TIME and cat.cast_time(s) > 0 or
                   m['op'] == F.SPELLMOD_COST and (s['ManaCost'] or s['ManaCostPercentage']) for m in mods):
                consumed.add(a['spell'])
    if consumed:   # beside what the cast consumed itself (Judgement's seal)
        out['consumed_auras'] = sorted(set(out.get('consumed_auras', [])) | consumed)
    events = out.get('events', []) + out.get('results', [])
    aura_success = bool(out.get('auras')) and not any(e.get('type') in ('miss', 'resist', 'dodge', 'parry') for e in events)
    if req.get('op') == 'melee':
        successful = out['outcome'] in ('hit', 'crit', 'block', 'glance') and out['damage'] > 0
        flags = 0x10 if out.get('spell') else 0x4
        crit = out['outcome'] == 'crit'
        event_spell = cat.spell(out['spell']) if out.get('spell') else None
    else:
        successful = any(e.get('type') in ('damage', 'heal') for e in events)
        crit = any(e.get('crit') for e in events)
        event_spell = s
        flags = 0x10 if s and s['DmgClass'] == 2 else (0x4000 if s and any(e.get('type') == 'heal' for e in events) else 0x10000)
    school = 1 << event_spell['School'] if event_spell else 1
    rng = random.Random(f'{req.get("seed", 0)}:talent_procs')
    overrides = {r['entry']: r for r in cat.raw['spell_proc_event']}
    procs = []
    from service.paladin import is_seal
    from service.paladin_talents import overridden
    for a in ch.auras:
        # A seal's proc is paladin.melee_procs'; one CMaNGOS overrides in code (Illumination) is paladin_talents'.
        if a['aura'] != 42 or is_seal(a['spell']) or overridden(a['spell']):
            continue
        source = cat.spell(a['spell'])
        row = overrides.get(source['Id'], overrides.get(cat.chain(source['Id']), {}))
        mask = int(row.get(f'SpellFamilyMask{a["eff"] - 1}', 0))
        if not successful and not (s and aura_success and mask):
            continue
        if not (row.get('procFlags') or source['ProcFlags']) & flags:
            continue
        if row.get('SchoolMask') and not row['SchoolMask'] & school:
            continue
        if row.get('procEx', 0) & 2 and not crit:
            continue
        if row.get('SpellFamilyName') and (not event_spell or row['SpellFamilyName'] != event_spell['SpellFamilyName']):
            continue
        if mask and (not event_spell or not mask & int(event_spell['SpellFamilyFlags'])):
            continue
        if number(cooldowns.get(str(source['Id']), 0), 'caster.proc_cooldowns value') > 0:
            continue
        chance = row.get('CustomChance') or source['ProcChance']
        if row.get('ppmRate'):
            chance = row['ppmRate'] * ch.melee_weapon()['delay'] / 600
        chance = F.apply_spell_mod(source, 18, chance, ch.mods)
        if not F.roll_chance(rng, chance):
            continue
        trigger_id = source[f'EffectTriggerSpell{a["eff"]}']
        if not trigger_id or trigger_id not in cat.spells:   # a catalog older than the class manifest lacks it
            continue
        trigger = cat.spell(trigger_id)
        proc = {'source_spell': source['Id'], 'spell': trigger_id,
                'cooldown_ms': int(row.get('Cooldown', 0) * 1000), 'events': [], 'auras': []}
        resolved = any(trigger[f'Effect{e}'] in (2, 6, 10, 35) for e in (1, 2, 3))
        for e in (1, 2, 3):
            effect = trigger[f'Effect{e}']
            amount = int(F.effect_value(trigger, e, ch.level, rng, mods=ch.mods))
            if effect == 80:
                gain = min(amount, 5 - caster.get('combo_points', 0) - out.get('combo_points', 0))
                if gain > 0:
                    out['combo_points'] = out.get('combo_points', 0) + gain
                    proc['events'].append({'type': 'combo_points', 'amount': gain})
            elif effect == 30 and not resolved:   # spells.resolve energizes a resolved trigger (Engulfing Shadows)
                power = {0: 'mana', 1: 'rage', 3: 'energy'}.get(trigger[f'EffectMiscValue{e}'])
                proc['events'].append({'type': 'power', 'power': power, 'amount': amount / 10 if power == 'rage' else amount})
        from service import spells
        if resolved:
            friendly = spells.is_positive(trigger)
            # A melee op's target is a unit without a kind (Vindication's debuff on the swing's victim).
            target = {'kind': 'self'} if friendly else {'kind': 'unit', **req['target']}
            result = spells.resolve(cat, ch, {**req, 'spell': trigger_id, 'target': target}, feral_dispatch=False, triggered=True)
            proc.update(auras=result['auras'], events=proc['events'] + result['events'], target='caster' if friendly else 'target',
                        school=trigger['School'], tooltip=tooltip(cat, ch, trigger))
        procs.append(proc)
    if procs:
        out['procs'] = out.get('procs', []) + procs
    return out
