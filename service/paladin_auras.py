"""Paladin auras and blessings, as CMaNGOS Classic @8ec338a applies the 1.12.1 Spell.dbc rows.

Auras (spell_group 8 'Aura', UNIQUE_PER_CASTER: one of hers at a time) - Devotion, Retribution, Concentration, Fire,
Frost and Shadow Resistance, Sanctity - are SPELL_EFFECT_APPLY_AREA_AURA_PARTY (35) on herself. AreaAura::Update
(SpellAuras.cpp:410-580) keeps giving every living party member within the effect radius her aura, at the rank its
level allows (SpellMgr::SelectAuraRankForLevel), and takes it off a member who leaves the radius or when she no longer
has it. The plugin asks op party_aura which of her followers are in it.

Blessings (spell_group 7 'Blessing', UNIQUE_PER_CASTER) - Might, Wisdom, Kings, Salvation, Light, Sanctuary, Sacrifice,
Protection and the Greater Blessings - are 5-minute (Greater: 15-minute) single-target auras. A Paladin keeps ONE
blessing on a unit: Unit::RemoveNoStackAurasDueToAuraHolder (Unit.cpp:4920-5045) removes her other blessing there
(any type), and a stronger similar one of hers (or a higher rank) bounces the new one (Spell::CheckCast,
Spell.cpp:4955-4995: SPELL_FAILED_AURA_BOUNCED). The 1.12.1 tooltips say the same: "Players may only have one Blessing
on them per Paladin at any one time." Blessing of Freedom is outside spell_group 7 (sql/updates/mangos/
z2828_01_mangos_spell_groups.sql), so it stays beside her other blessing.

Effects the Skyrim side needs the service for: Blessing of Light's bonus to her Holy Light / Flash of Light on the
blessed unit (light_bonus), Sanctuary's flat damage reduction and block damage and Sacrifice's damage transfer
(incoming_hit for her, op party_hit for a follower), Salvation's threat (paladin_defense.threat, also on her swings).
The Druid never reaches any of it: every path keys on these spell IDs or on a Paladin caster.
"""
import random

from service import formulas as F, paladin, paladin_defense, spells
from service.progression import number
from service.spells import CastError

# SpellAuraDefines.h
A_DUMMY, A_MOD_THREAT, A_MOD_DAMAGE_TAKEN, A_DAMAGE_SHIELD, A_PERIODIC_ENERGIZE = 4, 10, 14, 15, 24
A_PROC_TRIGGER_DAMAGE, A_MECHANIC_IMMUNITY, A_MOD_DAMAGE_PERCENT_DONE = 43, 77, 79
A_MOD_MECHANIC_RESISTANCE, A_MOD_TOTAL_STAT_PERCENTAGE, A_RESIST_PUSHBACK, A_SPLIT_DAMAGE_FLAT = 117, 137, 149, 153
E_APPLY_AURA, E_AREA_AURA_PARTY = 6, 35
# SharedDefines.h Mechanics (the ones these spells name)
MECHANICS = {7: 'root', 9: 'silence', 11: 'snare', 26: 'interrupt'}
# SpellTargetDefines.h / SpellTargets.cpp: explicit positive targets (SpellMgr.cpp:358 IsExplicitPositiveTarget)
TARGET_UNIT_FRIEND, TARGET_UNIT_RAID, TARGET_UNIT_RAID_AND_CLASS = 21, 57, 61
SPELL_ATTR_PASSIVE = 0x40
SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT, SPELL_ATTR_EX_EXCLUDE_CASTER = 0x8000, 0x80000
SPELL_ATTR_EX2_ALLOW_LOW_LEVEL_BUFF, SPELL_ATTR_EX2_USE_SHAPESHIFT_BAR = 0x8, 0x10
SPELLMOD_RADIUS = 6
SCHOOL_MASK_ALL = 0x7F
BLESSING_OF_FREEDOM = (1044,)            # its only rank (1.12.1 Spell.dbc; outside spell_group 7)
HOLY_LIGHT_HEAL, FLASH_OF_LIGHT_HEAL = 19968, 19993   # what every Holy Light / Flash of Light rank heals through
SYMBOL_OF_KINGS = 21177                   # the Greater Blessings' Spell.dbc Reagent1
GREATER_PARTY = 'party_as_one_class'   # decision D3: her followers are one class
MAX_MEMBERS = 64                          # server.MAX_TARGETS


def auras() -> frozenset:
    return paladin.groups()['Aura'][1]


def blessings() -> frozenset:
    return paladin.groups()['Blessing'][1]


def light_spells() -> frozenset:
    """Blessing of Light and Greater Blessing of Light (the manifest handler citing Paladin.cpp:124)."""
    return frozenset(sid for h in paladin.manifest()['handlers'] if h['behaviour'].startswith('Blessing of Light')
                     for sid in h['spells'])


def handles(sid: int) -> bool:
    return sid in auras() or sid in blessings() or sid in BLESSING_OF_FREEDOM


def is_greater(s) -> bool:
    return any(s[f'EffectImplicitTargetA{e}'] == TARGET_UNIT_RAID_AND_CLASS for e in spells._effects(s))


# --- validation ---------------------------------------------------------------------------------------------------

def _ranked(cat, sid) -> bool:
    """SpellMgr::GetSpellRank != 0: CMaNGOS's spell chains (SpellMgr::LoadSpellChains) hold only spells with ranks."""
    chain = cat.chain(sid)
    return sum(1 for row in cat.ranks.values() if row['chain'] == chain) > 1


def too_low(cat, s, level) -> bool:
    """SpellMgr::SelectAuraRankForLevel (SpellMgr.cpp:1167) for a unit `level` levels: a positive ranked buff more than
    ten levels above it finds no rank - CMaNGOS's default AutoDownrank = 0 (World.cpp:738, mangosd.conf) never picks a
    lower one."""
    if level + 10 >= s['SpellLevel'] or s['Attributes'] & SPELL_ATTR_PASSIVE:
        return False
    need = any((s[f'Effect{e}'] == E_APPLY_AURA and s[f'EffectImplicitTargetA{e}'] in
                (TARGET_UNIT_FRIEND, TARGET_UNIT_RAID, TARGET_UNIT_RAID_AND_CLASS)) or s[f'Effect{e}'] == E_AREA_AURA_PARTY
               for e in spells._effects(s))
    return need and _ranked(cat, s['Id'])


def validate(cat, ch, s, req) -> None:
    t = req['target']
    paladin_defense.validate(cat, ch, s, req)   # TARGET_UNIT_RAID needs her party (Salvation, Sacrifice)
    if t['kind'] == 'self' and s['AttributesEx'] & SPELL_ATTR_EX_EXCLUDE_CASTER:
        # Spell::CheckCast (Spell.cpp:4902): Blessing of Sacrifice cannot be cast on herself.
        raise CastError('SPELL_FAILED_BAD_TARGETS')
    if t['kind'] == 'unit' and is_greater(s) and not t.get('party', False):
        # TARGET_UNIT_RAID_AND_CLASS is a TARGET_GROUP filter (SpellTargets.cpp:94): Spell::CheckCast (Spell.cpp:5087)
        # wants a member of her group - a follower (target.party).
        raise CastError('SPELL_FAILED_BAD_TARGETS')
    if t['kind'] == 'unit' and 'level' in t and not s['AttributesEx2'] & SPELL_ATTR_EX2_ALLOW_LOW_LEVEL_BUFF \
            and too_low(cat, s, int(number(t['level'], 'target.level'))):
        # Spell::CheckCast (Spell.cpp:4855-4860): no rank for a unit that low.
        raise CastError('SPELL_FAILED_LOWLEVEL')


# --- casts --------------------------------------------------------------------------------------------------------

def resolve(cat, ch, req) -> dict:
    s = cat.spell(req['spell'])
    if s['Id'] in paladin_defense.DISPATCHED:
        # Blessing of Protection: paladin_defense's immunity and Forbearance, then the Blessing group below.
        out = paladin_defense.resolve(cat, ch, req)
    else:
        validate(cat, ch, s, req)
        out = spells.resolve(cat, ch, req, feral_dispatch=False)
        describe(cat, ch, s, out, req.get('caster', {}))
    stacking(cat, ch, s, req, out)
    return out


def describe(cat, ch, s, out, caster) -> dict:
    """Name each aura effect the generic resolver leaves as aura_N (their CMaNGOS handlers cited), and add what the
    plugin needs to apply it: the party aura's membership, the Greater Blessing's pending class targets, Freedom's
    immunities, the flat amounts a follower's hit takes in Skyrim units."""
    native = 'skyrim' in caster
    rule = cat.balance.get(s['Id'])
    for a in out['auras']:
        misc, aura = s[f"EffectMiscValue{a['eff']}"], a['aura']
        if aura == A_DUMMY and s['Id'] in light_spells():
            # Paladin.cpp:119-133 BlessingOfLight: effect 0's amount joins Holy Light's healing taken, effect 1's Flash
            # of Light's (light_bonus).
            heal = HOLY_LIGHT_HEAL if a['eff'] == 1 else FLASH_OF_LIGHT_HEAL
            a.update(type='healing_taken', linked_spell=heal, heals=cat.spell(heal)['SpellName'] if heal in cat.spells
                     else ('Holy Light' if heal == HOLY_LIGHT_HEAL else 'Flash of Light'))
        elif aura == A_MOD_THREAT:
            # Aura::HandleModThreat (SpellAuras.cpp:2811): x(100 + amount)/100 on the threat of every school in misc.
            a.update(type='threat', school_mask=misc & SCHOOL_MASK_ALL, multiplier=1 + a['amount'] / 100.0)
        elif aura == A_MOD_DAMAGE_TAKEN:
            # Unit::MeleeDamageBonusTaken (Unit.cpp:7635) / SpellBaseDamageBonusTaken (Unit.cpp:7174): a flat amount on
            # every hit and tick of these schools, before armor and the crit (Unit::CalculateMeleeDamage, Unit.cpp:1935).
            a.update(type='damage_taken', school_mask=misc & SCHOOL_MASK_ALL, per_hit=True)
            if native:
                a['native_per_hit'] = round(a['amount'] * paladin_defense.block_health_factor(), 2)
        elif aura == A_PROC_TRIGGER_DAMAGE:
            # Unit::HandleProcTriggerDamageAuraProc (UnitAuraProcHandler.cpp:1565-1591): on a blocked melee attack
            # (spell_proc_event procEx BLOCK) the blessed unit deals the amount to the attacker.
            a.update(type='block_damage', school=s['School'], on='block', proc_flags=s['ProcFlags'])
            if native and rule is not None:
                from service import skyrim
                a['native_amount'] = max(1, round(a['amount'] * skyrim.health_factor(rule, caster)))
        elif aura == A_MECHANIC_IMMUNITY:
            # Aura::HandleModMechanicImmunity (SpellAuras.cpp:2941): immune to the mechanic, and
            # SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT removes what she already has of it (RemoveAurasAtMechanicImmunity).
            a.pop('amount', None)
            a.update(type='mechanic_immunity', mechanic=misc, name=MECHANICS.get(misc, str(misc)),
                     purges_existing=bool(s['AttributesEx'] & SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT))
        elif aura == A_MOD_DAMAGE_PERCENT_DONE:
            # Unit::SpellDamageBonusDone / MeleeDamageBonusDone: x(100 + amount)/100 to damage of these schools.
            a.update(type='damage_done_percent', school_mask=misc & SCHOOL_MASK_ALL)
        elif aura == A_MOD_MECHANIC_RESISTANCE:
            # Unit::CalculateSpellResistChance (Unit.cpp:4030): +amount% to resist a spell of this mechanic outright.
            a.update(type='mechanic_resistance', mechanic=misc, name=MECHANICS.get(misc, str(misc)))
        elif aura == A_MOD_TOTAL_STAT_PERCENTAGE:
            # Aura::HandleModTotalPercentStat (SpellAuras.cpp:3542): every stat (misc -1) x(100 + amount)/100.
            a.update(type='stat_percent', stat=misc)
        elif aura == A_SPLIT_DAMAGE_FLAT:
            # Unit::CalculateDamageAbsorbAndResist (Unit.cpp:2512-2546): up to the amount of each hit is dealt to her
            # instead (party_hit).
            a.update(type='damage_transfer', school_mask=misc & SCHOOL_MASK_ALL, to='caster', per_hit=True)
            if native:
                a['native_per_hit'] = round(a['amount'] * paladin_defense.block_health_factor(), 2)
    sid = s['Id']
    if sid in auras():
        eff = next(e for e in spells._effects(s) if s[f'Effect{e}'] == E_AREA_AURA_PARTY)
        out['party_aura'] = {'radius_yd': aura_radius(cat, ch, s, eff), 'membership_op': 'party_aura',
                             # SPELL_ATTR_EX2_USE_SHAPESHIFT_BAR: the 1.12 client shows her auras on the stance bar.
                             'stance_bar': bool(s['AttributesEx2'] & SPELL_ATTR_EX2_USE_SHAPESHIFT_BAR)}
        for a in out['auras']:
            a.setdefault('area', {})['membership_op'] = 'party_aura'
    if is_greater(s):
        # TARGET_UNIT_RAID_AND_CLASS (Spell.cpp:2638-2672) blesses every group member of the target's class within the
        # radius. Followers have no WoW class, so her party counts as one class (decision D3, October 16): the plugin
        # blesses the target and every follower within the radius, one Symbol of Kings for the cast.
        eff = next(e for e in spells._effects(s) if s[f'EffectImplicitTargetA{e}'] == TARGET_UNIT_RAID_AND_CLASS)
        out['greater_blessing'] = {'applies_to': 'party', 'status': GREATER_PARTY,
                                   'class_radius_yd': cat.radius_yd(s, eff)}
    immune = [a for a in out['auras'] if a['type'] == 'mechanic_immunity']
    if immune:
        out['immunity'] = {'mechanics': [a['mechanic'] for a in immune], 'names': [a['name'] for a in immune],
                           'duration_ms': immune[0]['duration_ms'], 'purges_existing': immune[0]['purges_existing']}
    transfer = next((a for a in out['auras'] if a['type'] == 'damage_transfer'), None)
    if transfer:
        out['transfer'] = {'per_hit': transfer['amount'], 'school_mask': transfer['school_mask'], 'to': 'caster',
                           'event_op': 'party_hit', **({'native_per_hit': transfer['native_per_hit']}
                                                       if 'native_per_hit' in transfer else {})}
    return out


def aura_radius(cat, ch, s, eff) -> float:
    """AreaAura::AreaAura (SpellAuras.cpp:306-308): the effect radius with her SPELLMOD_RADIUS."""
    return F.apply_spell_mod(s, SPELLMOD_RADIUS, cat.radius_yd(s, eff), ch.mods)


# --- the group rule -------------------------------------------------------------------------------------------------

def _listed(cat, req) -> list:
    """The WoW spells on the spell's target: hers (caster.auras) for a self cast, else target.auras."""
    t = req['target']
    ids = req.get('caster', {}).get('auras', []) if t['kind'] == 'self' else t.get('auras', [])
    if not isinstance(ids, list):
        raise ValueError('target.auras must be an array of spell IDs')
    return [i for i in ids if isinstance(i, int) and not isinstance(i, bool) and i in cat.spells]


def _amounts(ch, s) -> dict:
    """(effect, aura, sign) -> |amount| of each aura effect, as IsSimilarAuraEffect pairs them (SpellMgr.h:1556)."""
    out = {}
    for e in spells._effects(s):
        if s[f'EffectApplyAuraName{e}']:
            value = int(F.effect_value(s, e, ch.level, None, maximum=True, mods=ch.mods))
            out.setdefault((s[f'Effect{e}'], s[f'EffectApplyAuraName{e}'], value < 0), abs(value))
    return out


def stronger(cat, ch, new, existing) -> bool:
    """IsSimilarExistingAuraStronger (SpellMgr.h:1564) or a higher rank of the same spell (Spell.cpp:4992-4994,
    Unit.cpp:5031): the existing aura of hers stays and the new one does not land."""
    if cat.chain(new['Id']) == cat.chain(existing['Id']) and new['Id'] != existing['Id'] and \
            cat.ranks.get(existing['Id'], {}).get('rank', 0) > cat.ranks.get(new['Id'], {}).get('rank', 0):
        return True
    mine, theirs = _amounts(ch, new), _amounts(ch, existing)
    return any(theirs[key] > value for key, value in mine.items() if key in theirs)


def stacking(cat, ch, s, req, out) -> dict:
    """spell_group 8 Aura / 7 Blessing, rule UNIQUE_PER_CASTER (Unit::RemoveNoStackAurasDueToAuraHolder,
    Unit.cpp:4985-5040): her new aura or blessing removes her other one of the group on that unit (replaced_auras, when
    the plugin lists the unit's auras; stack_spells names the whole group). An aura is a caster-source spell
    (IsSpellWithCasterSourceTargetsOnly), so it always replaces; a blessing is bounced by a stronger similar blessing of
    hers there or a higher rank - SPELL_FAILED_AURA_BOUNCED on another unit (Spell::CheckCast), not applied on herself."""
    group = 'Aura' if s['Id'] in auras() else 'Blessing' if s['Id'] in blessings() else None
    if group is None:
        return out
    rule, members = paladin.groups()[group]
    out.update(stack=group, stack_rule=rule, stack_spells=sorted(members & cat.spells.keys()))
    replaced = []
    for sid in _listed(cat, req):
        if sid not in members:
            continue
        if group == 'Blessing' and stronger(cat, ch, s, cat.spell(sid)):
            if req['target']['kind'] != 'self':
                raise CastError('SPELL_FAILED_AURA_BOUNCED')
            out.update(bounced_by=sid, auras=[])
            return out
        replaced.append(sid)
    if replaced:
        out['replaced_auras'] = replaced
    return out


# --- the party aura -------------------------------------------------------------------------------------------------

def party_aura(cat, ch, req) -> dict:
    """op party_aura: which followers her active aura reaches now - AreaAura::Update (SpellAuras.cpp:410-470, 584-620).

    request: spell (her aura), caster (caster.auras lists it while she has it), members: [{id, distance_yd, alive,
    level, party (default true), friendly (default true)}]. A member is in it while she has the aura, it is alive, in her
    party, friendly and within the radius, and the aura has a rank for its level (SelectAuraRankForLevel: none more
    than ten levels above it). The members' auras are hers: her amounts (SPELLMOD_ALL_EFFECTS talents included); a
    damage shield takes only the holder's own SPELLMOD_DAMAGE (Unit::DealMeleeDamage, Unit.cpp:2285), none for a
    follower. Answer: radius_yd, caster_has_aura, auras (what each member in it carries, with Skyrim effects), members:
    [{id, in_aura, reason?}]."""
    sid = req.get('spell')
    if sid not in auras() or sid not in cat.spells:
        raise ValueError('party_aura: spell must be a Paladin aura')
    s = cat.spell(sid)
    caster = req.get('caster', {})
    held = sid in caster.get('auras', [])
    members = req.get('members', [])
    if not isinstance(members, list) or len(members) > MAX_MEMBERS:
        raise ValueError(f'members must be an array of at most {MAX_MEMBERS} units')
    resolved = spells.resolve(cat, ch, {'op': 'resolve', 'spell': sid, 'seed': req.get('seed', 0), 'caster': caster,
                                        'target': {'kind': 'self'}}, feral_dispatch=False, triggered=True)
    describe(cat, ch, s, resolved, caster)
    shared = []
    for a in resolved['auras']:
        a = {k: v for k, v in a.items() if k != 'area'}
        if a['type'] == 'damage_shield':
            a['amount'] = max(0, int(F.effect_value(s, a['eff'], ch.level, None, maximum=True, mods=ch.mods)))
        shared.append({**a, 'spell': sid, 'from_caster': True})
    eff = next(e for e in spells._effects(s) if s[f'Effect{e}'] == E_AREA_AURA_PARTY)
    radius = aura_radius(cat, ch, s, eff)
    rows = []
    for m in members:
        if not isinstance(m, dict) or 'id' not in m:
            raise ValueError('members require an id')
        reason = None
        if not held:
            reason = 'caster_lost_aura'
        elif m.get('dead', not m.get('alive', True)):
            reason = 'dead'
        elif not m.get('party', True):
            reason = 'not_party'
        elif not m.get('friendly', True):
            reason = 'not_friendly'
        elif number(m.get('distance_yd', 0), 'members.distance_yd') > radius:
            reason = 'out_of_range'
        elif 'level' in m and too_low(cat, s, int(number(m['level'], 'members.level'))):
            reason = 'low_level'
        rows.append({'id': m['id'], 'in_aura': reason is None, **({'reason': reason} if reason else {})})
    return {'spell': sid, 'name': s['SpellName'], 'rank': s['Rank1'], 'radius_yd': radius, 'caster_has_aura': held,
            'events': [], 'auras': shared, 'members': rows}


# --- incoming damage ------------------------------------------------------------------------------------------------

def taken_flat(ch, school: int, native: bool) -> float:
    """SPELL_AURA_MOD_DAMAGE_TAKEN on her (Blessing of Sanctuary: -10 to -24 a hit) for this school, in the hit's units:
    WoW, or Skyrim health at the white-hit scale her block value uses (paladin_defense.block_health_factor)."""
    flat = sum(a['amount'] for a in ch.auras if a['aura'] == A_MOD_DAMAGE_TAKEN and a['misc'] & (1 << school))
    return flat * paladin_defense.block_health_factor() if native and flat else flat


def party_hit(cat, ch, req) -> dict:
    """op party_hit: a follower's hit under her blessings, for the plugin to apply (the follower's own hits never reach
    the WoW defence table; Skyrim rolled them).

    request: caster (her: immunities from caster.auras), member: {id, auras: [her spells on it]}, target: the attacker
    (level, actor_values: its native absorption of a Holy proc), attack: {damage (Skyrim health with caster.skyrim, else
    WoW), kind: 'melee'|'ranged'|'spell'|'periodic', school 0-6, outcome: 'hit'|'crit'|'crush'|'glance'|'block'},
    ctx: {caster_alive (default true), self_damage (the member hurt itself)}.
    1. Blessing of Sanctuary's SPELL_AURA_MOD_DAMAGE_TAKEN: the flat amount off every hit and tick (MeleeDamageBonusTaken
       / SpellDamageBonusTaken; an NPC attacker's coefficient is 1, Unit.cpp:6992).
    2. On a blocked melee attack its PROC_TRIGGER_DAMAGE: the member deals the amount (no spell power of its own) as Holy
       to the attacker (procs; binary and positive: no miss or partial resist; the attacker's native absorption).
    3. Blessing of Sacrifice's SPELL_AURA_SPLIT_DAMAGE_FLAT: up to the amount of what is left goes to her instead, as
       unmitigated damage of the hit's school (DealDamageMods: her absorbs only). Not when the member hurt itself, she is
       dead, or she is immune to the school - then CMaNGOS returns before the absorb is written (Unit.cpp:2537-2541) and
       the member takes the whole hit.
    Answer: member_damage (what the member takes), events (damage_reduced, damage_transfer: amount she takes), procs."""
    from service import native_values
    caster = req.get('caster', {})
    member, attack = req.get('member'), req.get('attack')
    if not isinstance(member, dict) or 'id' not in member or not isinstance(attack, dict):
        raise ValueError('party_hit requires member {id, auras} and attack {damage, school}')
    ids = member.get('auras', [])
    if not isinstance(ids, list) or any(isinstance(i, bool) or not isinstance(i, int) or i not in cat.spells for i in ids):
        raise ValueError('member.auras requires catalog spell IDs')
    kind = attack.get('kind', 'melee')
    if kind not in ('melee', 'ranged', 'spell', 'periodic'):
        raise ValueError("attack.kind must be 'melee', 'ranged', 'spell' or 'periodic'")
    school = attack.get('school', 0)
    if isinstance(school, bool) or school not in range(7):
        raise ValueError('attack.school must be a school from 0 to 6')
    damage = number(attack.get('damage', 0), 'attack.damage')
    if damage < 0:
        raise ValueError('attack.damage must be nonnegative')
    ctx = req.get('ctx', {})
    native = 'skyrim' in caster
    scale = paladin_defense.block_health_factor() if native else 1.0
    rng = random.Random(f"{req.get('seed', 0)}:party_hit")
    out = {'member': member['id'], 'kind': kind, 'school': school, 'damage_in': damage, 'events': [], 'procs': []}

    def rows(aura):
        for sid in ids:
            s = cat.spell(sid)
            for e in spells._effects(s):
                if s[f'Effect{e}'] == E_APPLY_AURA and s[f'EffectApplyAuraName{e}'] == aura:
                    yield s, e, int(F.effect_value(s, e, ch.level, None, maximum=True, mods=ch.mods))

    for s, e, amount in rows(A_MOD_DAMAGE_TAKEN):
        if damage > 0 and s[f'EffectMiscValue{e}'] & (1 << school):
            reduced = min(damage, -amount * scale) if amount < 0 else -amount * scale
            damage -= reduced
            out['events'].append({'type': 'damage_reduced', 'spell': s['Id'], 'amount': _units(reduced, native)})
    if kind == 'melee' and attack.get('outcome') == 'block':
        for s, e, amount in rows(A_PROC_TRIGGER_DAMAGE):
            row = next((r for r in cat.raw['spell_proc_event'] if r['entry'] in (s['Id'], cat.chain(s['Id']))), {})
            if not ((row.get('procFlags') or s['ProcFlags']) & paladin_defense.PROC_TAKE_MELEE
                    and row.get('procEx', 0) & paladin_defense.PROC_EX_BLOCK):
                continue
            from service import skyrim
            event = ({'type': 'absorb', 'mana': 0} if native_values.absorbs(req.get('target', {}), s['School'], rng)
                     else {'type': 'damage', 'amount': amount, 'resisted': 0, 'crit': False, 'school': s['School']})
            out['procs'].append({'source_spell': s['Id'], 'spell': s['Id'], 'target': 'attacker', 'cooldown_ms': 0,
                                 'cost': 0, 'skill': skyrim.spell_skill(s, cat.balance), 'events': [event], 'auras': []})
    for s, e, amount in rows(A_SPLIT_DAMAGE_FLAT):
        if damage <= 0 or not s[f'EffectMiscValue{e}'] & (1 << school) or ctx.get('self_damage', False) \
                or not ctx.get('caster_alive', True):
            continue
        transfer = min(damage, amount * scale)
        if paladin_defense.immune(ch, 1 << school):
            out['events'].append({'type': 'damage_transfer', 'spell': s['Id'], 'amount': 0, 'school': school,
                                  'to': 'caster', 'from': member['id'], 'caster_immune': True})
            break
        damage -= transfer
        out['events'].append({'type': 'damage_transfer', 'spell': s['Id'], 'amount': _units(transfer, native),
                              'school': school, 'to': 'caster', 'from': member['id']})
    out['member_damage'] = _units(damage, native)
    if not out['procs']:
        out.pop('procs')
    return out


def _units(value, native):
    return round(value, 2) if native else int(value)


# --- server hooks ---------------------------------------------------------------------------------------------------

def light_bonus(cat, ch, req, linked) -> tuple:
    """Blessing of Light on the healed unit (target.auras; her own caster.auras for a self cast): Unit::
    SpellHealingBonusTaken (Unit.cpp:7275-7280) adds effect 0's amount to Holy Light's heal (19968), effect 1's to
    Flash of Light's (19993), after her healing done and before the crit (SpellEffects.cpp:1876-1878, Spell.cpp:1234).

    WoWBridge custom rule (labelled; ch.custom_rules): CMaNGOS's BlessingOfLight script (Paladin.cpp:119-133) finds no
    spell to affect - 19977's spell_affect rows and Spell.dbc EffectItemType are empty, so Aura::isAffectedOnSpell
    (SpellAuras.cpp:698-714) is false for every heal. The script's own note says the bonus "is penalized since 2.3.0":
    in 1.12 it is the full amount, as the 1.12.1 tooltip promises ("up to 400"), not through 19968's coefficient."""
    if not ch.custom_rules or linked not in (HOLY_LIGHT_HEAL, FLASH_OF_LIGHT_HEAL):
        return 0, None
    eff = 1 if linked == HOLY_LIGHT_HEAL else 2
    total, source = 0, None
    for sid in _listed(cat, req):
        if sid in light_spells():
            total += int(F.effect_value(cat.spell(sid), eff, ch.level, None, maximum=True, mods=ch.mods))
            source = sid
    return total, source


def melee_threat(cat, ch, out) -> dict:
    """Unit::ApplyTotalThreatModifier (Unit.cpp:8638) on her white swing (physical) and each proc's damage (its spell's
    school): Blessing of Salvation's -30% on every school, Righteous Fury on a seal's Holy procs (paladin_defense.threat;
    a Paladin's only)."""
    if ch.cls != F.CLASS_PALADIN:
        return out
    paladin_defense.threat(ch, {'School': 0}, out)
    for proc in out.get('procs', []):
        if proc.get('spell') in cat.spells:
            paladin_defense.threat(ch, cat.spell(proc['spell']), proc)
    return out
