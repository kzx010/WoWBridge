"""General-tab racials of the playable races (the Druid's Night Elf and Tauren, the Paladin's Human and Dwarf); Classic
rules and explicit Skyrim equivalents. data/racial_data.json holds each racial's CMaNGOS evidence and its plan."""
import random
import json
from functools import lru_cache
from pathlib import Path

from service import formulas as F
from service.skyrim_effects import ARMOR_BUFF, IMMUNE, RESIST_DISEASE, RESIST_POISON, aura_effects

SHADOWMELD = 20580
WAR_STOMP = 20549
PASSIVES = (20582, 20583, 20585)   # legacy Night Elf callers
PASSIVES_BY_RACE = {4: PASSIVES, 6: (20550, 20551, 20552)}   # the Druid's races
# Human (1) and Dwarf (3), the Paladin's races: playercreateinfo_spell (race, class 2) and racial_data.json racials.
SWORD_SPECIALIZATION, THE_HUMAN_SPIRIT, DIPLOMACY, MACE_SPECIALIZATION = 20597, 20598, 20599, 20864
GUN_SPECIALIZATION, FROST_RESISTANCE = 20595, 20596
PERCEPTION, STONEFORM, FIND_TREASURE = 20600, 20594, 2481
STONEFORM_LINKED = 20612   # SpellAuraHolder::HandleSpellSpecificBoosts (SpellAuras.cpp:5582): bleed/disease immunity
RACE_PASSIVES = {**PASSIVES_BY_RACE, 1: (SWORD_SPECIALIZATION, THE_HUMAN_SPIRIT, DIPLOMACY, MACE_SPECIALIZATION),
                 3: (GUN_SPECIALIZATION, FROST_RESISTANCE)}
RACE_ACTIVES = {4: (SHADOWMELD,), 6: (WAR_STOMP,), 1: (PERCEPTION,), 3: (STONEFORM, FIND_TREASURE)}
ALL_PASSIVES = set().union(*RACE_PASSIVES.values())
ALL_ACTIVES = set().union(*RACE_ACTIVES.values())
# Actives resolve_active names the auras of, for the plugin's carriers (skse/src/Carriers.cpp Racials; Cast's aura).
ACTIVE_AURAS = frozenset((PERCEPTION, STONEFORM, FIND_TREASURE))
# SharedDefines.h SpellMechanics, for the immunities these grant.
MECHANICS = {15: 'bleed'}
# Stoneform's dispel immunities (aura 41: SharedDefines.h DispelType 3 Disease, 4 Poison) as Skyrim's own immunity to
# them, Hevnoraak's (skyrim_effects RESIST_POISON / RESIST_DISEASE at 100).
DISPEL_IMMUNITY_NATIVE = {3: RESIST_DISEASE, 4: RESIST_POISON}


@lru_cache(maxsize=1)
def data():
    return json.loads((Path(__file__).resolve().parent.parent / 'data/racial_data.json').read_text())


def levelstats(race, level):
    return data()['levelstats_by_race'].get(str(race), {}).get(str(level))


def races(cls):
    """The races a class has CMaNGOS level stats for (player_levelstats via racial_data.json levelstats_by_class; the
    client's CharBaseInfo pairs). None for a class without them: it keeps the legacy rule (the Druid races' racials)."""
    rows = data().get('levelstats_by_class', {}).get(str(cls))
    return {int(race) for race in rows} if rows else None


def passive_spells(cls, race):
    """The racial passives a character of this class and race has: none for a race the class cannot be."""
    allowed = races(cls)
    if allowed is None:
        return PASSIVES_BY_RACE.get(race, ())
    return RACE_PASSIVES.get(race, ()) if race in allowed else ()


def active_spells(cls, race):
    allowed = races(cls)
    if allowed is None:
        return RACE_ACTIVES.get(race, ()) if race in PASSIVES_BY_RACE else ()
    return RACE_ACTIVES.get(race, ()) if race in allowed else ()


def health_multiplier(ch):
    # SPELL_AURA_MOD_INCREASE_HEALTH_PERCENT changes total health, including gear and stamina.
    value = 1.0
    for a in ch.auras:
        if a['aura'] == 133:
            value *= 1 + a['amount'] / 100
    return value



def validate(s, ch, req):
    from service.spells import CastError
    if s['Id'] not in ALL_ACTIVES and s['Id'] not in ALL_PASSIVES:
        return
    if s['Id'] not in (*active_spells(ch.cls, ch.race), *passive_spells(ch.cls, ch.race)):
        raise CastError('SPELL_FAILED_NOT_KNOWN')
    if s['Id'] in ALL_PASSIVES:
        raise CastError('SPELL_FAILED_NOT_KNOWN')   # automatic passives, never cast from bars
    if req['target'].get('kind') != 'self' and not (s['Id'] == WAR_STOMP and req.get('_racial_impact') is True):
        raise CastError('SPELL_FAILED_BAD_TARGETS')
    if s['Id'] != SHADOWMELD:
        return
    caster = req.get('caster', {})
    # Playtest rule: stationary Shadowmeld may drop combat even in combat.
    if not isinstance(caster.get('moving', False), bool):
        raise ValueError('caster.moving must be a boolean')
    if caster.get('moving', False):
        raise CastError('SPELL_FAILED_MOVING')


def passives(cat, ch, skyrim=False):
    out = []
    for sid in passive_spells(ch.cls, ch.race):
        if sid not in cat.spells:   # older catalogs remain readable; regeneration supplies racials
            continue
        s = cat.spell(sid)
        p = {'spell': sid, 'name': s['SpellName'], 'passive': True, 'duration_ms': -1}
        if sid == 20582:
            p.update(type='dodge', amount=F.effect_value(s, 1, ch.level, None, maximum=True))
            # Skyrim has no dodge table: armor gives .12% physical mitigation per point, capped at 80%.
            if skyrim:
                p['skyrim'] = [{'mgef': ARMOR_BUFF, 'magnitude': p['amount'] / .12}]
        elif sid in (20583, 20551):
            p.update(type='resistance', amount=F.effect_value(s, 1, ch.level, None, maximum=True), school_mask=8)
            if skyrim:
                p['skyrim'] = aura_effects(p)   # existing Nature -> poison resistance, /3
        elif sid == 20550:
            p.update(type='health_percent', amount=F.effect_value(s, 1, ch.level, None, maximum=True),
                     multiplier=health_multiplier(ch), max_health=ch.max_health)
            if skyrim:
                p['native_max_health_percent'] = p['amount']   # native carrier measures the live Skyrim maximum
        elif sid == 20552:
            p.update(type='skill', skill=s['EffectMiscValue1'], amount=F.effect_value(s, 1, ch.level, None, maximum=True))
            if skyrim:
                p['informational'] = True   # Skyrim has no Herbalism skill; do not grant Alchemy
        elif sid in (SWORD_SPECIALIZATION, MACE_SPECIALIZATION, GUN_SPECIALIZATION):
            # SPELL_AURA_MOD_SKILL_TALENT (Aura::HandleAuraModSkill -> Player::ModifySkillBonus): weapon skill on the
            # attack table melee.py rolls while the main hand is of one of these skills (Character.weapon_skill_value).
            skills = [s[f'EffectMiscValue{e}'] for e in (1, 2, 3) if s[f'EffectApplyAuraName{e}'] == 98]
            p.update(type='weapon_skill', skills=skills, amount=F.effect_value(s, 1, ch.level, None, maximum=True),
                     active=ch.weapon_skill_line in skills and ch.form not in ('cat', 'bear', 'dire_bear'))
            if sid == GUN_SPECIALIZATION:
                p['informational'] = True   # SkillRaceClassInfo gives no Dwarf of these classes Guns
            elif skyrim:
                p['wow_attack_table'] = True   # Skyrim's own One-Handed/Two-Handed skill is not raised
        elif sid == THE_HUMAN_SPIRIT:
            # SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE (SpellAuras.cpp:3542): Character.build scales total Spirit, which
            # Character.regen reads. Its share (total Spirit less the same Spirit without this aura) is bonus Spirit as
            # a Spirit buff is: natively one point of Fortify Magicka Rate a Spirit point (aura_effects' stat route,
            # skyrim_effects.STAT[6] 0x7A0FD), the plugin's "The Human Spirit" carrier (Carriers::Racials).
            p.update(type='stat_percent', stat=s['EffectMiscValue1'], amount=F.effect_value(s, 1, ch.level, None, maximum=True),
                     spirit=spirit_share(ch, sid))
            if skyrim:
                p['skyrim'] = aura_effects({'type': 'stat', 'stat': p['stat'], 'amount': p['spirit']})
        elif sid == DIPLOMACY:
            # Player::CalculateReputationGain: Skyrim keeps no WoW reputation.
            p.update(type='reputation_gain', amount=F.effect_value(s, 1, ch.level, None, maximum=True), informational=True)
        elif sid == FROST_RESISTANCE:
            p.update(type='resistance', amount=F.effect_value(s, 1, ch.level, None, maximum=True),
                     school_mask=s['EffectMiscValue1'])
            if skyrim:
                p['skyrim'] = aura_effects(p)   # frost resistance, /3 as gear's
        else:
            # Player::BuildPlayerRepop casts 20584 for Night Elves, not from this dummy passive.
            p.update(type='ghost', ghost_spell=20584, informational=True)
        out.append(p)
    return out


def spirit_share(ch, sid):
    """The Spirit one total-percentage aura of hers adds: Unit::GetTotalStatValue with and without it (formulas.stat_total
    on Character.stat_parts), as CMaNGOS truncates both."""
    index = 4   # SharedDefines.h Stats: STAT_SPIRIT
    if 'spi' not in ch.stat_parts:
        return 0
    pcts = {aura: [a['amount'] for a in ch.auras if a['aura'] == aura and a['misc'] in (index, -1)] for aura in (80, 137)}
    others = [a['amount'] for a in ch.auras if a['aura'] == 137 and a['misc'] in (index, -1) and a.get('spell') != sid]
    base, buffs = ch.stat_parts['spi']
    return F.stat_total(base, buffs, pcts[80], pcts[137]) - F.stat_total(base, buffs, pcts[80], others)


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
    resistance = ch.resistance(school, level, req.get('dispel', 0))
    pct = F.effective_resist_percent(level, ch.level, resistance, binary,
                                   target_is_creature=False)
    rng = random.Random(req.get('seed', 0))
    if ch.actor_values is not None:
        from service import native_values
        chance = native_values.absorb_chance(ch.actor_values)
        if chance > 0 and not req.get('periodic', False) and F.roll_chance(rng, chance):   # ticks never re-roll
            return {'damage': 0, 'resisted': 0, 'absorbed': damage, 'resistance': resistance,
                    'resist_percent': pct}
    fraction = pct / 100 if pct < 0 else (
        (1.0 if F.roll_chance(rng, pct) else 0.0) if binary else F.partial_resist_multiplier(
            pct, req.get('periodic', False), False, req.get('attacker_is_player', False), rng))
    resisted = int(damage * fraction)
    return {'damage': damage - resisted, 'resisted': resisted, 'resistance': resistance,
            'resist_percent': pct}


def resolve_stomp(cat, ch, req):
    from service import spells
    # The ordinary resolver supplies client timings and stun semantics. The cast has no selected target.
    out = spells.resolve(cat, ch, req, feral_dispatch=False)
    area = {'radius_yd': cat.radius_yd(cat.spell(WAR_STOMP), 1), 'hostile': True,
            'max_targets': cat.spell(WAR_STOMP)['MaxAffectedTargets'], 'channel': False}
    for aura in out['auras']:
        aura['area'] = area
    # Each victim is rolled separately at impact. The plugin requests these after the 500ms cast completes.
    if 'targets' in req:
        out['results'] = []
        out['auras'] = []
        rng = random.Random(req.get('seed',0))
        for target in req['targets']:
            if len(out['results']) >= area['max_targets']:
                break
            if target.get('dead', False) or target.get('friendly', False) or not target.get('attackable', True) or not target.get('los',True):
                continue
            if target.get('distance_yd', 0) > area['radius_yd']:
                continue
            # The caster-centric spell has a zero single-target range. Impact victims have already passed the area gate.
            victim = {**target, 'kind': 'unit', 'distance_yd': 0}
            # Generic racial validation accepts the cast itself, then the resolver rolls the victim.
            hit = spells.resolve(cat, ch, {**req, 'target': victim, '_racial_impact': True, 'seed': rng.getrandbits(32)}, feral_dispatch=False, triggered=True)
            out['results'].append({'id': target['id'], 'events': hit['events'], 'auras': hit['auras']})
    return out


def actives(cat, ch):
    """The racial spells this character casts (the bar's General tab) and their timings."""
    out = []
    for sid in active_spells(ch.cls, ch.race):
        if sid not in cat.spells:
            continue
        s = cat.spell(sid)
        out.append({'spell': sid, 'name': s['SpellName'], 'passive': False, 'icon': cat.icon(s),
                    'cooldown_ms': s['RecoveryTime'], 'duration_ms': cat.duration(s, ch.level)})
    return out


def resolve_active(cat, ch, req):
    """Perception, Stoneform and Find Treasure: CMaNGOS's own timings and auras (spells.resolve), each aura named for
    the plugin, which carries them natively (docs/racials.md):
    - Stoneform: armor_percent is Cast's existing armor share (her worn armor times the percentage, as Enrage's); each
      dispel immunity names Skyrim's own immunity to its type in its 'skyrim' list (Resist Poison / Resist Disease 100,
      Hevnoraak's); and while the aura lasts Carriers::Racials ends her Skyrim poisons, diseases and bleeding, as
      applying the immunities purges them (Unit::ApplySpellDispelImmunity with SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT,
      Aura::HandleModMechanicImmunity) and refuses new ones.
    - Perception: Carriers::Racials keeps Skyrim's Detect Life on her while the aura lasts (Skyrim's sight of hidden
      creatures; it has no stealth-detection value of its own).
    - Find Treasure: Cast's tracking aura (one tracker at a time, kept until cancelled)."""
    from service import spells
    out = spells.resolve(cat, ch, req, feral_dispatch=False)
    sid, s = req['spell'], cat.spell(req['spell'])
    auras = [(s, a) for a in out['auras']]
    if sid == STONEFORM:
        # SpellAuraHolder::HandleSpellSpecificBoosts (SpellAuras.cpp:5582) casts 20612 with Stoneform.
        linked = cat.spell(STONEFORM_LINKED)
        boost = spells.resolve(cat, ch, {**req, 'spell': STONEFORM_LINKED}, feral_dispatch=False, triggered=True)
        auras += [(linked, a) for a in boost['auras']]
    for spell, a in auras:
        misc = spell[f"EffectMiscValue{a['eff']}"]
        a['spell'] = spell['Id']
        if a['aura'] == 17:
            # Aura::HandleModStealthDetect (SpellAuras.cpp:2621): stealth detection strength of this stealth type.
            a.update(type='stealth_detection', stealth_type=misc)
        elif a['aura'] == 101:
            # SPELL_AURA_MOD_RESISTANCE_PCT: misc 1 is armor (SPELL_SCHOOL_MASK_NORMAL).
            a.update(type='armor_percent' if misc == 1 else 'resistance_percent', school_mask=misc)
        elif a['aura'] == 77:
            a.update(type='mechanic_immunity', mechanic=misc, name=MECHANICS.get(misc, str(misc)))
        elif a['aura'] == 41 and misc in DISPEL_IMMUNITY_NATIVE:
            a['skyrim'] = [{'mgef': DISPEL_IMMUNITY_NATIVE[misc], 'magnitude': IMMUNE}]
        if a['aura'] in (41, 77):
            # SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT (SpellMgr.h:1312): applying the immunity removes such effects.
            a['purges_existing'] = bool(spell['AttributesEx'] & 0x8000)
    out['auras'] = [a for _, a in auras]
    return out
