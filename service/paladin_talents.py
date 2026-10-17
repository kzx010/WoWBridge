"""The Paladin's 44 talents (1.12.1 Talent.dbc, data/talents.json) as CMaNGOS Classic @8ec338a runs them.

Most ranks are passive auras and spell modifiers. character.build applies them in talent mode as it does the Druid's
(Player::AddSpellMod with family-10 SpellFamilyFlags masks, formulas.SpellMods; Unit::GetTotalAuraModifier for the
sheet auras) and the paths that already read them do the rest:
  Holy        Divine Strength and Intellect (SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE, formulas.stat_total); Spiritual Focus
              (SPELLMOD_NOT_LOSE_CASTING_TIME: pushback_resist_pct, Spell::Delayed); Improved Seal of Righteousness
              (SPELLMOD_ALL_EFFECTS on Judgement of Righteousness, mask 0x400); Healing Light (ALL_EFFECTS on the Holy
              Light / Flash of Light rank, the linked heal's custom basepoint); Improved Lay on Hands' cooldown; Improved
              Blessing of Wisdom; Lasting Judgement (SPELLMOD_DURATION, paladin.judge); Holy Power (SPELL_AURA_MOD_SPELL_
              CRIT_CHANCE_SCHOOL, Character.spell_crit); Divine Favor (+100 SPELLMOD_CRITICAL_CHANCE on 19968, 19993 and
              Holy Shock, mask 0x206000).
  Protection  Improved Devotion and Concentration Aura, Improved Righteous Fury, Guardian's Favor, Improved Hammer of
              Justice (spell mods on those casts); Toughness (SPELL_AURA_MOD_BASE_RESISTANCE_PCT); Shield Specialization
              (Character.block_value); Anticipation (paladin_defense.defense_table); Redoubt and Reckoning
              (paladin_defense._crit_procs); Precision and One-Handed Weapon Specialization (the melee table and swing).
  Retribution Improved Blessing of Might; Benediction (SPELLMOD_COST); Improved Judgement (SPELLMOD_COOLDOWN); Improved
              Seal of the Crusader's Judgement of the Crusader (ALL_EFFECTS); Deflection; Conviction; Two-Handed Weapon
              Specialization; Vengeance and Vindication on her own hits and casts (talent_effects.finish).
  The talents that teach a spell (Consecration, Divine Favor, Holy Shock, Blessing of Kings, Blessing of Sanctuary, Holy
  Shield, Seal of Command, Sanctity Aura, Repentance) are gated by spells.resolve (SPELL_FAILED_NOT_KNOWN without the
  rank) and taught by talents.learned_spells and progression's talent rows.

This module holds the rest, each piece citing its routine:
- build: weapon-dependent talents (Precision, Conviction, One- and Two-Handed Weapon Specialization) count only while her
  main hand fits them (Player::_ApplyWeaponDependentAuraMods, UpdateWeaponHitChances); every class but the Druid's.
- Holy Shock (resolve): Spell::CheckCast's target rules and Spell::EffectDummy's heal-or-damage spell.
- finish / melee: Illumination (UnitAuraProcHandler's override of its trigger), Divine Favor's charge, Improved Lay on
  Hands' armor on the healed target, Vindication's debuff, and the procs of hits her triggered spells make (judgements,
  Holy Shock, Seal of Command) under Unit::IsTriggeredAtSpellProcEvent's triggered-spell rule.
- incoming: Eye for an Eye on a spell critical hit she takes.
- profile: the sheet values the stats op does not otherwise carry (defense skill, speeds, mechanic resistances).
- Labelled custom rules (Character.custom_rules; tools/oracle.py turns them off) where CMaNGOS drops an effect the
  1.12 tooltip promises: Improved Seal of the Crusader's attack power and Improved Seal of Righteousness on the seal.
  Each says what CMaNGOS does instead.
The Druid never reaches any of it: every entry point keys on a Paladin caster, a non-Druid class or these spell IDs.
"""
import random

from service import formulas as F
from service.progression import number

SPELLFAMILY_PALADIN = 10
# Spell.dbc rank IDs (data/talents.json rank_spells, data/paladin_manifest.json links; service/tests check both).
ILLUMINATION = (20210, 20212, 20213, 20214, 20215)
ILLUMINATION_MANA = 20272               # UnitAuraProcHandler.cpp:1442, the energize the override casts
DIVINE_FAVOR = 20216
# Spell::EffectDummy (SpellEffects.cpp:1439-1443): each Holy Shock rank's (damage, heal) spell.
HOLY_SHOCK = {20473: (25912, 25914), 20929: (25911, 25913), 20930: (25902, 25903)}
IMPROVED_LAY_ON_HANDS = (20234, 20235)
VINDICATION = (9452, 26016, 26021)
VENGEANCE = (20049, 20056, 20057, 20058, 20059)
EYE_FOR_AN_EYE = (9799, 25988)
EYE_FOR_AN_EYE_DAMAGE = 25997           # UnitAuraProcHandler.cpp:807
IMPROVED_SEAL_OF_RIGHTEOUSNESS = (20224, 20225, 20330, 20331, 20332)
IMPROVED_SEAL_OF_THE_CRUSADER = (20335, 20336, 20337)
LINKED_HEALS = (19968, 19993)           # Holy Light's and Flash of Light's code-linked heals (paladin.script_heal)
SEAL_OF_RIGHTEOUSNESS_FLAGS = 0x8000000
SEAL_OF_THE_CRUSADER_FLAGS = 0x200

# SpellAuraDefines.h
A_DUMMY, A_PROC_TRIGGER_SPELL, A_MOD_CRIT_PERCENT, A_MOD_HIT_CHANCE = 4, 42, 52, 54
A_MOD_DAMAGE_PERCENT_DONE, A_MOD_INCREASE_SPEED, A_MOD_ATTACK_POWER, A_ADD_FLAT_MODIFIER = 79, 31, 99, 107
A_MOD_MECHANIC_RESISTANCE, A_MOD_BASE_RESISTANCE_PCT, A_MOD_INCREASE_MOUNTED_SPEED, A_MOD_STAT_PERCENT = 117, 142, 172, 137
WEAPON_DEPENDENT = (A_MOD_CRIT_PERCENT, A_MOD_HIT_CHANCE, A_MOD_DAMAGE_PERCENT_DONE)
ITEM_CLASS_WEAPON = 2
SKILL_DEFENSE = 95
# SpellDefines.h SpellModOp / attributes
SPELLMOD_ATTACK_POWER, SPELLMOD_CHANCE_OF_SUCCESS = 3, 18
ATTR_IS_ABILITY, ATTR_EX3_NOT_A_PROC, ATTR_EX3_SUPPRESS_CASTER_PROCS = 0x10, 0x200, 0x10000
ATTR_EX3_CAN_PROC_FROM_PROCS = 0x4000000
# SpellMgr.h ProcFlags (the dealing side) and ProcFlagsEx
PROC_DEAL_MELEE_ABILITY, PROC_DEAL_RANGED_ABILITY, PROC_DEAL_HELPFUL_ABILITY = 0x10, 0x100, 0x400
PROC_DEAL_HARMFUL_ABILITY, PROC_DEAL_HELPFUL_SPELL, PROC_DEAL_HARMFUL_SPELL = 0x1000, 0x4000, 0x10000
PROC_EX_CRITICAL_HIT = 0x2
DMG_MELEE, DMG_RANGED = 2, 3
# SharedDefines.h Mechanics
MECHANICS = {1: 'charm', 2: 'disoriented', 3: 'disarm', 4: 'distract', 5: 'fear', 6: 'fumble', 7: 'root', 8: 'pacify',
             9: 'silence', 10: 'sleep', 11: 'snare', 12: 'stun', 13: 'freeze', 14: 'knockout', 15: 'bleed',
             16: 'bandage', 17: 'polymorph', 18: 'banish', 19: 'shield', 20: 'shackle', 21: 'mount', 22: 'persuade',
             23: 'turn', 24: 'horror', 25: 'invulnerability', 26: 'interrupt', 27: 'daze', 28: 'discovery',
             29: 'immune_shield', 30: 'sapped'}


def _is_paladin(ch) -> bool:
    return ch.cls == F.CLASS_PALADIN


# --- character.build -------------------------------------------------------------------------------------------

def fits_main_hand(cat, ch, s) -> bool:
    """Item::IsFitToSpellRequirements (Item.cpp:834) for her main hand: a spell wanting a weapon class (2) needs a
    main-hand weapon of a subclass in its mask (0 or -1: any); no weapon fits nothing. Other spells always fit."""
    if s['EquippedItemClass'] != ITEM_CLASS_WEAPON:
        return True
    item = cat.item(ch.weapon_entry) if ch.weapon_entry else None
    if not item or item['class'] != ITEM_CLASS_WEAPON:
        return False
    mask = s['EquippedItemSubClassMask']
    return not mask or bool(mask & (1 << item['subclass']))


def build(cat, ch) -> None:
    """character.build's last step for every class but the Druid (her sheet stays as it always was).

    Weapon-dependent auras count only while her main hand fits the spell: SPELL_AURA_MOD_DAMAGE_PERCENT_DONE (One- and
    Two-Handed Weapon Specialization: Aura::HandleModDamagePercentDone leaves a weapon spell to Player::_ApplyWeapon
    DependentAuraDamageMod, Player.cpp:7212), SPELL_AURA_MOD_CRIT_PERCENT (Conviction: _ApplyWeaponDependentAuraCritMod,
    Player.cpp:7191) and SPELL_AURA_MOD_HIT_CHANCE (Precision: Player::UpdateWeaponHitChances, StatSystem.cpp:518).

    Custom rule (labelled; counted only while ch.custom_rules): Improved Seal of the Crusader's SPELLMOD_ATTACK_POWER
    on the seal's attack power. CMaNGOS applies that op only in Aura::CalculateAuraEffectValue (SpellAuras.cpp:6085),
    which Spell.cpp:4978 calls for the stacking comparison alone, so the applied aura keeps its unmodified amount and
    the talent's first effect is lost; the 1.12 tooltip promises it. The extra amount is a separate aura row tagged
    custom_rule, which Character.total skips with custom rules off."""
    ch.auras[:] = [a for a in ch.auras if a['aura'] not in WEAPON_DEPENDENT or a['spell'] not in cat.spells
                   or fits_main_hand(cat, ch, cat.spell(a['spell']))]
    if not ch.talent_mode:
        return
    for a in list(ch.auras):
        if a['aura'] == A_MOD_ATTACK_POWER and a['spell'] in cat.spells and _crusader_seal(cat.spell(a['spell'])):
            extra = crusader_attack_power(cat.spell(a['spell']), a['amount'], ch.mods) - a['amount']
            if extra:
                ch.auras.append({**a, 'amount': extra, 'custom_rule': 'Improved Seal of the Crusader'})


def _crusader_seal(s) -> bool:
    return s['SpellFamilyName'] == SPELLFAMILY_PALADIN and bool(s['SpellFamilyFlags'] & SEAL_OF_THE_CRUSADER_FLAGS)


def crusader_attack_power(s, amount, mods) -> int:
    """Aura::CalculateAuraEffectValue (SpellAuras.cpp:6085-6094): SPELL_AURA_MOD_ATTACK_POWER's amount through the
    caster's SPELLMOD_ATTACK_POWER (Improved Seal of the Crusader: +5/10/15% on mask 0x200), int32 as the aura holds it."""
    return int(F.apply_spell_mod(s, SPELLMOD_ATTACK_POWER, amount, mods))


def righteousness_amount(ch, seal, amount) -> int:
    """paladin.righteousness_damage's seal amount (triggerAmount, UnitAuraProcHandler.cpp:1094-1137).

    Custom rule (labelled; only while ch.custom_rules): Improved Seal of Righteousness's +3..15% SPELLMOD_ALL_EFFECTS
    also on the seal. Its 1.12.1 mask (0x400) holds Judgement of Righteousness only; the seals (0x8000000) and their
    melee payloads (25713-25742, no family flags) fall outside it, so CMaNGOS raises the judgement and never the seal the
    tooltip names. The rule applies the same modifier as if the mask held the seal, i.e. to the aura amount the proc
    formula reads (as SPELLMOD_ALL_EFFECTS reaches it in WorldObject::CalculateSpellEffectValue, Object.cpp:2973)."""
    if not ch.custom_rules or not ch.talent_mode:
        return amount
    mods = [{**m, 'mask': SEAL_OF_RIGHTEOUSNESS_FLAGS} for m in ch.mods
            if m['spell'] in IMPROVED_SEAL_OF_RIGHTEOUSNESS and m['op'] == F.SPELLMOD_ALL_EFFECTS]
    if not mods:
        return amount
    return int(F.apply_spell_mod(seal, F.SPELLMOD_ALL_EFFECTS, amount, F.SpellMods(mods, family=ch.mods.family)))


# --- Holy Shock ----------------------------------------------------------------------------------------------------

def handles(sid: int) -> bool:
    return sid in HOLY_SHOCK


def _holy_shock_validate(cat, ch, s, req):
    """Spell::CheckCast (Spell.cpp:5234-5246): a unit target; one she can attack must be in front of her
    (SPELL_FAILED_UNIT_NOT_INFRONT), else one she can assist (SPELL_FAILED_BAD_TARGETS). Returns whether Spell::
    EffectDummy (SpellEffects.cpp:1450) heals (CanAssistSpell) or strikes. The parent's range, line of sight and life
    checks are spells._validate's, asked as the branch the target takes."""
    from service import spells
    t = req['target']
    if t['kind'] == 'self':
        friendly = True
    elif t['kind'] == 'unit':
        allied = t.get('friendly', t.get('reaction') == 'friendly')
        attackable, assistable = t.get('attackable', not allied), t.get('healable', allied)
        if attackable and not t.get('in_front', True):
            raise spells.CastError('SPELL_FAILED_UNIT_NOT_INFRONT')
        if not attackable and not assistable:
            raise spells.CastError('SPELL_FAILED_BAD_TARGETS')
        friendly = assistable
    else:
        raise spells.CastError('SPELL_FAILED_BAD_TARGETS')
    # The dummy's target mode (TARGET_UNIT, 25) is either side's: validate as the branch's own (21 friend, 6 enemy).
    spells._validate(cat, {**s, 'EffectImplicitTargetA1': 21 if friendly else 6}, ch, req)
    return friendly


def _magic_hit(ch, s, t, rng):
    """Unit::MagicSpellHitResult (Unit.cpp:2872), as spells.resolve rolls it: miss and full resist share one die side
    (a non-binary player spell is fully resisted by the distribution's 100% column, Unit.cpp:4018). Returns the failing
    event, or None for a hit (the native absorption is paladin._mitigated's, after it)."""
    from service import spells
    miss = F.spell_miss_chance(ch.level, t['level'], t.get('is_player', False),
                               F.apply_spell_mod(s, F.SPELLMOD_RESIST_MISS_CHANCE, ch.spell_hit(), ch.mods))
    pct = spells._resist_pct(ch, s, t, False)
    full = F.partial_resist_distribution()[int(pct * 100)][4] * 0.01 if pct > 0 else 0.0
    chance = miss + full
    if F.roll_chance(rng, chance):
        return {'type': 'resist' if rng.random() * chance >= miss else 'miss', 'spell': s['Id']}
    return None


def resolve(cat, ch, req) -> dict:
    """Holy Shock: the rank's cost, cooldown (its 30-second category: paladin_defense._category) and GCD, then
    Spell::EffectDummy (SpellEffects.cpp:1431-1453) casts the rank's heal on a unit she can assist, else its damage on
    the enemy, both TRIGGERED_OLD_TRIGGERED. The cast spell is in procs (holy_shock 'heal' or 'damage', like
    Judgement's payload): a heal is SpellHealingBonusDone with its 0.429 coefficient and a spell crit; a strike rolls
    MagicSpellHitResult, then paladin._holy_damage (coefficient, the target's Judgement of the Crusader, crit, partial
    resist). Divine Favor's +100% crit chance reaches both (mask 0x200000) and is spent by that roll (finish)."""
    from service import paladin, paladin_defense, spells
    s = cat.spell(req['spell'])
    friendly = _holy_shock_validate(cat, ch, s, req)
    out = spells.outcome(cat, ch, s)
    paladin_defense._category(cat, ch, s, out)
    hurt, heal = HOLY_SHOCK[s['Id']]
    pid = heal if friendly else hurt
    paladin._require(cat, pid)
    p = cat.spell(pid)
    t = req['target']
    rng = random.Random(f"{req.get('seed', 0)}:holy_shock")
    proc = {'source_spell': s['Id'], 'spell': pid, 'holy_shock': 'heal' if friendly else 'damage', 'school': p['School'],
            'target': 'caster' if t['kind'] == 'self' else 'target', 'cooldown_ms': 0, 'events': [], 'auras': []}
    if friendly:
        value, raw = paladin._value(ch, p, 1, rng)
        amount = spells._healing_done(ch, cat, p, 1, value, dot=False, raw_base=raw)
        proc['crit_chance'] = paladin.crit_chance(ch, p)
        crit = F.roll_chance(rng, proc['crit_chance'])
        proc['events'].append({'type': 'heal', 'amount': paladin.crit_amount(ch, p, int(amount)) if crit else amount,
                               'crit': crit, 'school': p['School'], 'spell': pid})
    else:
        target = {**t, 'kind': 'unit'}
        failed = _magic_hit(ch, p, target, rng)
        if failed:
            proc['events'].append(failed)
        else:
            value, raw = paladin._value(ch, p, 1, rng)
            event, proc['crit_chance'] = paladin._holy_damage(cat, ch, p, target, value, raw, rng)
            proc['events'].append(event)
    out['procs'] = [proc]
    return out


def linked_tooltips(cat, ch, spell):
    """The spells a Holy Shock rank's description quotes ($25912s1, $25914s1): their tooltip profiles, as a seal's
    (talent_effects.tooltip). None for any other spell."""
    if spell['Id'] not in HOLY_SHOCK:
        return None
    from service.talent_effects import tooltip
    return {str(sid): tooltip(cat, ch, cat.spell(sid)) for sid in HOLY_SHOCK[spell['Id']] if sid in cat.spells}


# --- after a cast or a swing -------------------------------------------------------------------------------------

def overridden(sid: int) -> bool:
    """A PROC_TRIGGER_SPELL aura whose trigger CMaNGOS replaces in code (Illumination's 18350 'Dummy Trigger' becomes
    20272: UnitAuraProcHandler.cpp:1410-1443): talent_effects.finish leaves its proc to this module."""
    return sid in ILLUMINATION


def _proc_row(cat, s):
    rows = {r['entry']: r for r in cat.raw['spell_proc_event']}
    return rows.get(s['Id'], rows.get(cat.chain(s['Id']), {}))


def _hit_flags(s, heal: bool) -> int:
    """Spell::PrepareMasksForProcSystem (Spell.cpp:808-881): the attacker's proc flag of a spell's hit - by DmgClass for
    melee and ranged spells, else helpful or harmful, 'ability' with SPELL_ATTR_IS_ABILITY; none with
    SPELL_ATTR_EX3_SUPPRESS_CASTER_PROCS."""
    if s['AttributesEx3'] & ATTR_EX3_SUPPRESS_CASTER_PROCS:
        return 0
    if s['DmgClass'] == DMG_MELEE:
        return PROC_DEAL_MELEE_ABILITY
    if s['DmgClass'] == DMG_RANGED:
        return PROC_DEAL_RANGED_ABILITY
    ability = s['Attributes'] & ATTR_IS_ABILITY
    if heal:
        return PROC_DEAL_HELPFUL_ABILITY if ability else PROC_DEAL_HELPFUL_SPELL
    return PROC_DEAL_HARMFUL_ABILITY if ability else PROC_DEAL_HARMFUL_SPELL


def _hits(cat, procs, melee=False):
    """The hits her payload casts made: (spell row, events, triggered) for each landed spell in the procs - a judgement
    (cast by spell_judgement without TRIGGERED_OLD_TRIGGERED, Paladin.cpp:109; Command's strike is triggered by its
    dummy, SpellEffects.cpp:1462), Holy Shock's spell, and on a swing her seal's proc (TriggerProccedSpell and Seal of
    Righteousness' CastCustomSpell: TRIGGERED_OLD_TRIGGERED)."""
    from service.paladin import is_seal
    out = []
    for p in procs:
        if not (p.get('judgement') or p.get('holy_shock') or (melee and is_seal(p.get('source_spell')))):
            continue
        groups = {}
        for e in p.get('events', []):
            groups.setdefault(e.get('spell', p['spell']), []).append(e)
        for sid, events in groups.items():
            if sid in cat.spells:
                out.append((cat.spell(sid), events, not (p.get('judgement') and sid == p['spell'])))
    return out


def _procs_from_hits(cat, ch, req, hits, rng):
    """Unit::ProcDamageAndSpell for the hits of her triggered casts (talent_effects.finish sees only the cast's own
    events): her PROC_TRIGGER_SPELL talents (Vengeance, Vindication) by Unit::IsTriggeredAtSpellProcEvent
    (UnitAuraProcHandler.cpp:514-684) and SpellMgr::IsSpellProcEventCanTriggeredBy (SpellMgr.cpp:843): the event's
    proc flag, spell_proc_event's school, family and mask, its procEx (none: a hit or crit; CRITICAL_HIT: a crit), a
    triggered spell only with SPELL_ATTR_EX3_NOT_A_PROC on it or SPELL_ATTR_EX3_CAN_PROC_FROM_PROCS on the aura
    (Vengeance), ProcChance or the PPM rate at her main hand's speed, SPELLMOD_CHANCE_OF_SUCCESS, and its cooldown."""
    from service import spells
    from service.paladin import is_seal
    from service.talent_effects import tooltip
    caster = req.get('caster', {})
    cooldowns = caster.get('proc_cooldowns', {})
    if not isinstance(cooldowns, dict):
        raise ValueError('caster.proc_cooldowns must be an object')
    procs = []
    for s, events, triggered in hits:
        landed = [e for e in events if e['type'] in ('damage', 'heal')]
        if not landed:
            continue
        heal = landed[0]['type'] == 'heal'
        crit = any(e.get('crit') for e in landed)
        flags = _hit_flags(s, heal)
        for a in ch.auras:
            if a['aura'] != A_PROC_TRIGGER_SPELL or is_seal(a['spell']) or overridden(a['spell']) or a['spell'] not in cat.spells:
                continue
            source = cat.spell(a['spell'])
            row = _proc_row(cat, source)
            if not (row.get('procFlags') or source['ProcFlags']) & flags:
                continue
            if triggered and not (s['AttributesEx3'] & ATTR_EX3_NOT_A_PROC or
                                  source['AttributesEx3'] & ATTR_EX3_CAN_PROC_FROM_PROCS):
                continue
            if row.get('SchoolMask') and not row['SchoolMask'] & (1 << s['School']):
                continue
            if row.get('SpellFamilyName') and row['SpellFamilyName'] != s['SpellFamilyName']:
                continue
            mask = int(row.get(f'SpellFamilyMask{a["eff"] - 1}', 0))
            if mask and not mask & int(s['SpellFamilyFlags']):
                continue
            if row.get('procEx', 0) & PROC_EX_CRITICAL_HIT and not crit:
                continue
            if number(cooldowns.get(str(source['Id']), 0), 'caster.proc_cooldowns value') > 0:
                continue
            chance = row.get('CustomChance') or source['ProcChance']
            if row.get('ppmRate'):
                chance = row['ppmRate'] * ch.melee_weapon()['delay'] / 600
            chance = F.apply_spell_mod(source, SPELLMOD_CHANCE_OF_SUCCESS, chance, ch.mods)
            if not F.roll_chance(rng, chance):
                continue
            trigger_id = source[f'EffectTriggerSpell{a["eff"]}']
            if trigger_id not in cat.spells:
                continue
            trigger = cat.spell(trigger_id)
            friendly = spells.is_positive(trigger)
            target = {'kind': 'self'} if friendly else {**req['target'], 'kind': 'unit'}
            result = spells.resolve(cat, ch, {**req, 'spell': trigger_id, 'target': target,
                                              'seed': f"{req.get('seed', 0)}:{s['Id']}:{trigger_id}"},
                                    feral_dispatch=False, triggered=True)
            procs.append({'source_spell': source['Id'], 'spell': trigger_id, 'cooldown_ms': int(row.get('Cooldown', 0) * 1000),
                          'events': result['events'], 'auras': result['auras'], 'target': 'caster' if friendly else 'target',
                          'school': trigger['School'], 'tooltip': tooltip(cat, ch, trigger), 'from_spell': s['Id']})
    return procs


def _illumination(cat, ch, req, s, out, rng):
    """Illumination: Unit::HandleProcTriggerSpellAuraProc (UnitAuraProcHandler.cpp:1410-1443, SpellIconID 241) replaces
    its 18350 trigger: on a critical heal it casts 20272 on her with basepoints = the original rank's Spell.dbc manaCost
    (unmodified by Benediction or other cost mods), at the aura's ProcChance (20-100%, SPELLMOD_CHANCE_OF_SUCCESS).
    The heal that crits is the linked 19968 / 19993 (Holy Light's and Flash of Light's custom-basepoint heal, which
    carries the rank's ID as basepoint 1 - SpellEffects.cpp:4160-4170 - the handler's 'original' rank) or Holy Shock's
    25914 / 25913 / 25903 (the handler maps it to 20473 / 20929 / 20930). spell_proc_event 20210 (ClassicDB z2815) gives
    family 10, mask 0xC0206000 (both linked heals, the ranks, Holy Shock) and procEx CRITICAL_HIT; 19968 is cast without
    TRIGGERED_OLD_TRIGGERED and Holy Shock's spells carry SPELL_ATTR_EX3_NOT_A_PROC, so both may proc it. Its effect-2
    class script 2689 has no handler (HandleOverrideClassScriptAuraProc returns with nothing cast): inert, as in
    CMaNGOS."""
    heals = []
    if ILLUMINATION_MANA not in cat.spells:   # a catalog older than the manifest: no trigger entry, no proc (as CMaNGOS)
        return []
    if out.get('linked_spell') in LINKED_HEALS:
        heals.append((out['linked_spell'], s['Id'], out.get('events', [])))
    for p in out.get('procs', []):
        if p.get('holy_shock') == 'heal':
            heals.append((p['spell'], next(r for r, (_, h) in HOLY_SHOCK.items() if h == p['spell']), p['events']))
    procs = []
    caster = req.get('caster', {})
    for healer, original, events in heals:
        if not any(e.get('type') == 'heal' and e.get('crit') for e in events):
            continue
        h = cat.spell(healer)
        for a in ch.auras:
            if a['aura'] != A_PROC_TRIGGER_SPELL or a['spell'] not in ILLUMINATION:
                continue
            source = cat.spell(a['spell'])
            row = _proc_row(cat, source)
            mask = int(row.get(f'SpellFamilyMask{a["eff"] - 1}', 0))
            if not (row.get('procFlags') or source['ProcFlags']) & _hit_flags(h, heal=True) or \
                    (row.get('SpellFamilyName') and row['SpellFamilyName'] != h['SpellFamilyName']) or \
                    (mask and not mask & int(h['SpellFamilyFlags'])):
                continue
            chance = F.apply_spell_mod(source, SPELLMOD_CHANCE_OF_SUCCESS, source['ProcChance'], ch.mods)
            if not F.roll_chance(rng, chance):
                continue
            amount = cat.spell(original)['ManaCost']
            if 'skyrim' in caster:
                # Her Skyrim magicka for that 'base cost': the rank's native price as she pays it (its rule's counterpart
                # cost at the WoW mana ratio, with her skill's and gear's discount), so a critical heal is free, as in WoW,
                # and never a profit (an undiscounted refund returned more than the heal cost).
                from service import skyrim
                rule = cat.balance.get(original)
                discount = skyrim.cost_factor(caster['skyrim'].get(skyrim.SKILLS[rule.skill], 50)) * \
                    skyrim.cost_multiplier(caster, rule.skill) if rule else 1
                amount = max(1, round(rule.base_cost * discount * amount / rule.wow_cost)) if rule and rule.cost and amount else 0
            procs.append({'source_spell': source['Id'], 'spell': ILLUMINATION_MANA, 'target': 'caster', 'cooldown_ms': 0,
                          'proc_chance': chance, 'original_spell': original, 'from_spell': healer,
                          'events': [{'type': 'power', 'power': 'mana', 'amount': amount}], 'auras': []})
    return procs


def _divine_favor(cat, ch, req, s, out) -> None:
    """Divine Favor's single charge: Player::ApplySpellMod (Player.h:2603-2626) spends a finite modifier the first time
    an affected spell applies it - here when 19968 / 19993 or a Holy Shock spell rolls its crit (Unit::
    CalculateSpellCritChance's SPELLMOD_CRITICAL_CHANCE) - and Player::RemoveSpellMods removes the aura with the last
    charge once that spell finishes. talent_effects.finish spends cost and cast-time charges only."""
    if DIVINE_FAVOR not in req.get('caster', {}).get('auras', []):
        return
    rolled = []
    if out.get('linked_spell') in LINKED_HEALS and out.get('events'):
        rolled.append(out['linked_spell'])
    for p in out.get('procs', []):
        if p.get('holy_shock') and any(e['type'] in ('damage', 'heal') for e in p['events']):
            rolled.append(p['spell'])
    mods = [m for m in ch.mods if m['spell'] == DIVINE_FAVOR and m['op'] == F.SPELLMOD_CRITICAL_CHANCE]
    if any(F.affects(m, cat.spell(sid)) for m in mods for sid in rolled):
        out['consumed_auras'] = sorted(set(out.get('consumed_auras', [])) | {DIVINE_FAVOR})


def _label(cat, req, out) -> None:
    """Name the talent procs the generic path leaves anonymous. Improved Lay on Hands' 20233 / 20236 goes on the Lay on
    Hands target (UnitAuraProcHandler.cpp:1504 for 20233; 20236 by the target rule at 1541: a helpful spell's
    PROC_FLAG_DEAL_HELPFUL_SPELL keeps pVictim): +15/30% armor from items (SPELL_AURA_MOD_BASE_RESISTANCE_PCT, misc 1)
    for two minutes. Vindication's 67 / 26017 / 26018: -5/10/15% Strength and Agility on the target for 10 s
    (SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE, misc 0 and 1). Vengeance's 20050-20055: +3..15% physical and holy damage
    done for 8 s (SPELL_AURA_MOD_DAMAGE_PERCENT_DONE, misc 1 and 2). Divine Favor's own aura carries its one charge."""
    for p in out.get('procs', []):
        if p.get('source_spell') in IMPROVED_LAY_ON_HANDS:
            p['target'] = 'caster' if req['target'].get('kind') == 'self' else 'target'
            for a in p['auras']:
                if a['aura'] == A_MOD_BASE_RESISTANCE_PCT:
                    a.update(type='armor_from_items_percent', school_mask=cat.spell(p['spell'])[f"EffectMiscValue{a['eff']}"])
        elif p.get('source_spell') in VINDICATION:
            for a in p['auras']:
                if a['aura'] == A_MOD_STAT_PERCENT:
                    a.update(type='stat_percent', stat=cat.spell(p['spell'])[f"EffectMiscValue{a['eff']}"], debuff=True)
        elif p.get('source_spell') in VENGEANCE:
            for a in p['auras']:
                if a['aura'] == A_MOD_DAMAGE_PERCENT_DONE:
                    a.update(type='damage_done_percent', school_mask=cat.spell(p['spell'])[f"EffectMiscValue{a['eff']}"])
    if req.get('spell') == DIVINE_FAVOR:
        for a in out.get('auras', []):
            if a['aura'] == A_ADD_FLAT_MODIFIER:
                a['charges'] = cat.spell(DIVINE_FAVOR)['ProcCharges']


def finish(cat, ch, req, out) -> dict:
    """server resolve, after talent_effects.finish: her talents' share of a cast (talent mode, a Paladin)."""
    caster = req.get('caster', {})
    if not _is_paladin(ch) or 'talents' not in caster or out.get('cost_deferred'):
        return out
    s = cat.spell(req['spell'])
    if ch.custom_rules and _crusader_seal(s):
        for a in out.get('auras', []):
            if a['aura'] == A_MOD_ATTACK_POWER:
                boosted = crusader_attack_power(s, a['amount'], ch.mods)
                if boosted != a['amount']:
                    a.update(amount=boosted, custom_rule='Improved Seal of the Crusader')
    _divine_favor(cat, ch, req, s, out)
    _label(cat, req, out)
    rng = random.Random(f"{req.get('seed', 0)}:paladin_talents")
    new = _illumination(cat, ch, req, s, out, rng) + _procs_from_hits(cat, ch, req, _hits(cat, out.get('procs', [])), rng)
    if new:
        _label(cat, req, {'procs': new})
        out['procs'] = out.get('procs', []) + new
    return out


def melee(cat, ch, req, out) -> dict:
    """server melee, after paladin.melee_procs: Vindication's debuff label and the procs her seal's own hits raise
    (Seal of Command's 20424 carries SPELL_ATTR_EX3_NOT_A_PROC: Vindication and Vengeance; Righteousness's payloads
    cannot crit and only Vengeance may proc from a triggered spell)."""
    if not _is_paladin(ch) or 'talents' not in req.get('caster', {}):
        return out
    _label(cat, req, out)
    rng = random.Random(f"{req.get('seed', 0)}:paladin_talents_melee")
    new = _procs_from_hits(cat, ch, req, _hits(cat, out.get('procs', []), melee=True), rng)
    if new:
        _label(cat, req, {'procs': new})
        out['procs'] = out.get('procs', []) + new
    return out


# --- a hit she takes ---------------------------------------------------------------------------------------------

def eye_for_an_eye(cat, ch, req, out, kind, outcome, taken, native) -> None:
    """Eye for an Eye on a spell crit she takes (incoming_hit with attack kind 'spell'): Unit::HandleDummyAuraProc
    (UnitAuraProcHandler.cpp:795-808), procFlags TAKE_HARMFUL_SPELL with procEx CRITICAL_HIT (spell_proc_event 9799),
    a magic spell only: basepoints = 15/30% x (damage + absorb), at most int32(her maximum health) / 2, cast as 25997
    at the attacker (TriggerProccedSpell: TRIGGERED_OLD_TRIGGERED), which rolls MagicSpellHitResult, takes her holy
    damage-done percentages (no coefficient), a spell crit and partial resist. With caster.skyrim the incoming damage is
    already Skyrim health: the share stays in those units (weapon_factor 1 keeps skyrim.convert from scaling it again)
    and the half-health cap is given as max_health_fraction for the plugin to apply to her native maximum."""
    if kind != 'spell' or outcome != 'crit' or req.get('ctx', {}).get('sitting'):
        return
    from service import paladin, spells
    if EYE_FOR_AN_EYE_DAMAGE not in cat.spells:
        return
    s = cat.spell(EYE_FOR_AN_EYE_DAMAGE)
    t = {**req['target'], 'kind': 'unit'}
    rng = random.Random(f"{req.get('seed', 0)}:eye_for_an_eye")
    for a in ch.auras:
        if a['aura'] != A_DUMMY or a['spell'] not in EYE_FOR_AN_EYE:
            continue
        source = cat.spell(a['spell'])
        chance = F.apply_spell_mod(source, SPELLMOD_CHANCE_OF_SUCCESS, source['ProcChance'], ch.mods)
        if not F.roll_chance(rng, chance):
            continue
        proc = {'source_spell': source['Id'], 'spell': s['Id'], 'target': 'target', 'cooldown_ms': 0, 'school': s['School'],
                'events': [], 'auras': []}
        if native:
            basepoints = a['amount'] * taken / 100
            proc.update(weapon_factor=1.0, max_health_fraction=0.5)
        else:
            basepoints = min(int(a['amount'] * int(taken) / 100), int(ch.max_health) // 2)
        failed = _magic_hit(ch, s, t, rng)
        if failed:
            proc['events'].append(failed)
            out.setdefault('procs', []).append(proc)
            continue
        value = basepoints if native else int(F.effect_value(s, 1, ch.level, rng, base_points=int(basepoints)))
        # SpellDamageBonusDone: no coefficient (0), her holy SPELL_AURA_MOD_DAMAGE_PERCENT_DONE, SPELLMOD_DAMAGE.
        dmg = (value + F.bonus_with_coeffs(s, 1, 0, ch.spell_damage(1 << s['School']), ch.mods)) * \
            ch.multiplier(A_MOD_DAMAGE_PERCENT_DONE, 1 << s['School'])
        dmg = F.apply_spell_mod(s, F.SPELLMOD_DAMAGE, dmg, ch.mods)
        dmg = max(0.0, dmg) if native else max(0, int(dmg))
        crit = F.roll_chance(rng, paladin.crit_chance(ch, s, t))
        if crit:
            dmg = dmg * paladin.crit_multiplier(ch, s) if native else paladin.crit_amount(ch, s, dmg)
        if native:
            # Native health keeps its fraction until skyrim.convert rounds it (weapon_factor 1).
            from service import native_values, spells
            if native_values.absorbs(t, s['School'], rng):
                event = {'type': 'absorb', 'mana': 0, 'school': s['School']}
            else:
                resisted = dmg * F.partial_resist_multiplier(spells._resist_pct(ch, s, t, False), False, False, True, rng)
                event = {'type': 'damage', 'amount': dmg - resisted, 'resisted': resisted, 'crit': crit,
                         'school': s['School'], 'spell': s['Id']}
        else:
            event = paladin._mitigated(ch, s, t, dmg, crit, rng)
        proc['events'].append(event)
        out.setdefault('procs', []).append(proc)


# --- the sheet ---------------------------------------------------------------------------------------------------

def profile(cat, ch) -> dict:
    """The stats op's 'paladin' block: sheet values her talents set that the shared sheet does not carry.
    defense_skill: level x 5 plus SPELL_AURA_MOD_SKILL(_TALENT) 95 (Anticipation; Unit::GetDefenseSkillValue).
    melee_hit_percent: SPELL_AURA_MOD_HIT_CHANCE fitting her main hand (Precision; UpdateWeaponHitChances).
    item_armor_multiplier: SPELL_AURA_MOD_BASE_RESISTANCE_PCT on armor, the BASE_PCT of UNIT_MOD_ARMOR - armor from
    items (Toughness; Player::UpdateArmor; Improved Lay on Hands' buff when listed in caster.auras).
    movement_speed_percent / mounted_speed_percent: the strongest positive SPELL_AURA_MOD_INCREASE_SPEED /
    _MOUNTED_SPEED (Pursuit of Justice; Unit::UpdateSpeed takes GetMaxPositiveAuraModifier: it does not stack).
    mechanic_resist_percent: SPELL_AURA_MOD_MECHANIC_RESISTANCE by mechanic (Unyielding Faith: fear, disoriented),
    added to a spell's resist chance against her (Unit::CalculateSpellResistChance, Unit.cpp:4028-4041)."""
    from service.paladin_defense import _skill_bonus
    armor = 1.0
    for a in ch.auras:
        if a['aura'] == A_MOD_BASE_RESISTANCE_PCT and a['misc'] & 1:
            armor *= (100.0 + a['amount']) / 100.0
    resist = {}
    for a in ch.auras:
        if a['aura'] == A_MOD_MECHANIC_RESISTANCE and a['misc'] in MECHANICS:
            name = MECHANICS[a['misc']]
            resist[name] = resist.get(name, 0) + a['amount']
    return {'defense_skill': ch.level * 5 + _skill_bonus(ch, SKILL_DEFENSE),
            'melee_hit_percent': ch.total(A_MOD_HIT_CHANCE),
            'item_armor_multiplier': round(armor, 8),
            'movement_speed_percent': max([a['amount'] for a in ch.auras if a['aura'] == A_MOD_INCREASE_SPEED] + [0]),
            'mounted_speed_percent': max([a['amount'] for a in ch.auras if a['aura'] == A_MOD_INCREASE_MOUNTED_SPEED] + [0]),
            'mechanic_resist_percent': resist}
