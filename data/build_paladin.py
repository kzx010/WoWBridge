#!/usr/bin/env python3
"""Paladin manifest: every learnable rank with its Spell.dbc row and acquisition provenance, and the closure of the
spells those ranks reach, each link with its evidence (a Spell.dbc trigger, a ClassicDB row or a CMaNGOS code line).

Run python -m data.build_paladin. Inputs are read-only: the 1.12.1 client DBCs, ClassicDB z2815, the builders' outputs
data/progression.json and data/talents.json, and the pinned CMaNGOS source (mangos-classic @8ec338a). Every cited
source line is checked for its expected text, so a citation cannot drift from the code it names.
"""
import argparse
import json
import re
from pathlib import Path

from data import dbc
from data.build_catalog import DBC_DIR, DEFAULT_DUMP, REPO, sha256
from data.sql_dump import tables

CMANGOS = Path(__file__).resolve().parents[2] / 'WoW/cmangos/mangos-classic'
CMANGOS_COMMIT = '8ec338a1704e7dcb1c0213eb7ed58f9231ade40f'   # README pin; the checkout is that commit's source ZIP
DEFAULT_OUT = REPO / 'data/paladin_manifest.json'
KEY, CLASS_ID, FAMILY = 'paladin', 2, 10                      # CLASS_PALADIN, SPELLFAMILY_PALADIN
DBC_INPUTS = ('Spell', 'SpellCastTimes', 'SpellDuration', 'SpellRange', 'SpellRadius', 'SpellIcon', 'SkillLineAbility',
              'CharBaseInfo', 'ChrRaces', 'ChrClasses', 'CreatureDisplayInfo', 'CreatureModelData')

EFFECTS = 'src/game/Spells/SpellEffects.cpp'
PROCS = 'src/game/Spells/UnitAuraProcHandler.cpp'
SPELL = 'src/game/Spells/Spell.cpp'
AURAS = 'src/game/Spells/SpellAuras.cpp'
MGR, MGR_H = 'src/game/Spells/SpellMgr.cpp', 'src/game/Spells/SpellMgr.h'
UNIT = 'src/game/Entities/Unit.cpp'
SCRIPT = 'src/game/Spells/Scripts/Scripting/ClassScripts/Paladin.cpp'
QUESTS = 'src/game/Globals/ObjectMgr.cpp'
BINDINGS = 'sql/scriptdev2/spell.sql'                          # spell_scripts: the core's script names by spell id
GROUPS = 'sql/updates/mangos/z2828_01_mangos_spell_groups.sql'  # spell_group(_spell): newer than the z2815 dump

# Spell::EffectDummy (SpellIconID 156): Holy Shock rank -> (harmful, helpful) payload.
HOLY_SHOCK = {20473: (25912, 25914), 20929: (25911, 25913), 20930: (25902, 25903)}
# Unit::HandleDummyAuraProc: Seal of Righteousness rank -> its melee proc ("Rank 1.5" is 21084).
SEAL_OF_RIGHTEOUSNESS = {20154: 25742, 21084: 25741, 20287: 25740, 20288: 25739, 20289: 25738, 20290: 25737,
                         20291: 25736, 20292: 25735, 20293: 25713}
# Paladin.cpp JudgementOf{Light,Wisdom}Intermediate: judgement aura -> what its proc's intermediate casts.
JUDGEMENT_PAYLOADS = {20185: 20267, 20344: 20341, 20345: 20342, 20346: 20343,    # Light: heal the attacker
                      20186: 20268, 20354: 20352, 20355: 20353}                  # Wisdom: mana to the attacker
INTERMEDIATES = {5373: 'spell_judgement_of_light_intermediate', 1826: 'spell_judgement_of_wisdom_intermediate'}
# Unit::HandleDummyAuraProc (SPELLFAMILY_GENERIC): Eye for an Eye ranks -> the returned damage.
EYE_FOR_AN_EYE = {9799: 25997, 25988: 25997}
SEAL_FLAGS = 0x0A000200        # SpellMgr.h IsSealSpell
INVULNERABILITY, FORBEARANCE = 25, 25771
CAST_SPELL = 15                # ScriptMgr.h SCRIPT_COMMAND_CAST_SPELL
# Ids the Paladin code paths name that 1.12.1 lacks (later content); each is checked absent from Spell.dbc.
LATER_IDS = ((38320, SCRIPT, 'GetAura(38320', 'Improved Blessing of Light (a TBC libram aura)'),
             (43743, PROCS, 'GetAura(43743', 'Improved Seal of Righteousness (a TBC item aura)'),
             (27158, UNIT, 'case 27158:', 'Seal of the Crusader rank 7 (TBC)'),
             (34976, SCRIPT, 'HasAura(34976)', 'a TBC aura Divine Intervention refuses'))


class Source:
    """The pinned CMaNGOS checkout; cite() names the one line holding all of the needles."""

    def __init__(self, root=CMANGOS):
        self.root, self.lines, self.cited = Path(root), {}, set()

    def text(self, path):
        if path not in self.lines:
            self.lines[path] = (self.root / path).read_text(errors='replace').splitlines()
        return self.lines[path]

    def cite(self, path, *needles, after=None):
        lines, start = self.text(path), 0
        if after:
            anchors = [n for n, line in enumerate(lines) if after in line]
            if len(anchors) != 1:
                raise ValueError(f'{path}: anchor {after!r} found {len(anchors)} times')
            start = anchors[0]
        found = [n for n, line in enumerate(lines[start:], start) if all(x in line for x in needles)]
        if not found or (len(found) > 1 and not after):
            raise ValueError(f'{path}: {needles!r} found {len(found)} times')
        self.cited.add(path)
        return f'{path}:{found[0] + 1}'

    def mentions(self, number):
        """Whether any src/game file names this number (a script id, a spell id)."""
        pattern = re.compile(rf'\b{number}\b')
        return any(pattern.search(p.read_text(errors='replace'))
                   for p in sorted((self.root / 'src/game').rglob('*')) if p.suffix in ('.cpp', '.h'))


def u32(value):
    return value & 0xFFFFFFFF


def summary(s, client):
    """A Spell.dbc row as the service reads it: effects, cost, timing, range, procs and requirements."""
    duration, rng = client['durations'].get(s['DurationIndex']), client['ranges'].get(s['RangeIndex'])
    cast = client['cast_times'].get(s['CastingTimeIndex'])
    effects = []
    for i in range(3):
        if not s['Effect'][i]:
            continue
        radius = client['radii'].get(s['EffectRadiusIndex'][i])
        effects.append({'index': i, 'effect': s['Effect'][i], 'aura': s['EffectAura'][i],
                        'base_points': s['EffectBasePoints'][i], 'base_dice': s['EffectBaseDice'][i],
                        'die_sides': s['EffectDieSides'][i], 'dice_per_level': s['EffectDicePerLevel'][i],
                        'points_per_level': s['EffectRealPointsPerLevel'][i], 'misc': s['EffectMiscValue'][i],
                        'trigger_spell': s['EffectTriggerSpell'][i],
                        'targets': [s['ImplicitTargetA'][i], s['ImplicitTargetB'][i]],
                        'radius_yd': radius['Radius'] if radius else 0, 'period_ms': s['EffectAuraPeriod'][i],
                        'chain_targets': s['EffectChainTargets'][i], 'item_type': u32(s['EffectItemType'][i]),
                        'mechanic': s['EffectMechanic'][i], 'multiple_value': s['EffectAmplitude'][i]})
    lo, hi = (u32(m) for m in s['SpellClassMask'])
    return {'name': s['Name_lang'], 'rank_name': s['NameSubtext_lang'], 'school': s['School'],
            'category': s['Category'], 'dispel': s['DispelType'], 'mechanic': s['Mechanic'],
            'attributes': [u32(s[k]) for k in ('Attributes', 'AttributesEx', 'AttributesExB', 'AttributesExC',
                                                'AttributesExD')],
            'family': s['SpellClassSet'], 'family_flags': lo | hi << 32,
            'levels': {'base': s['BaseLevel'], 'spell': s['SpellLevel'], 'max': s['MaxLevel']},
            'power': {'type': s['PowerType'], 'cost': s['ManaCost'], 'cost_pct': s['ManaCostPct'],
                      'per_level': s['ManaCostPerLevel'], 'per_second': s['ManaPerSecond'],
                      'per_second_per_level': s['ManaPerSecondPerLevel']},
            'cast_ms': cast['Base'] if cast else 0, 'cooldown_ms': s['RecoveryTime'],
            'category_cooldown_ms': s['CategoryRecoveryTime'],
            'gcd': {'category': s['StartRecoveryCategory'], 'ms': s['StartRecoveryTime']},
            'duration_ms': {'base': duration['Duration'], 'per_level': duration['DurationPerLevel'],
                            'max': duration['MaxDuration']} if duration else None,
            'range_yd': [rng['RangeMin'], rng['RangeMax']] if rng else None,
            'reagents': [{'item': item, 'count': count}
                         for item, count in zip(s['Reagent'], s['ReagentCount']) if item > 0],
            'totems': [t for t in s['Totem'] if t],
            'proc': {'flags': u32(s['ProcFlags']), 'chance': s['ProcChance'], 'charges': s['ProcCharges']},
            'target_creature_type': s['TargetCreatureType'], 'targets': s['Targets'],
            'aura_state': {'caster': s['CasterAuraState'], 'target': s['TargetAuraState']},
            'interrupt': {'cast': s['InterruptFlags'], 'aura': s['AuraInterruptFlags'],
                          'channel': s['ChannelInterruptFlags']},
            'equipped': {'class': s['EquippedItemClass'], 'subclass_mask': s['EquippedItemSubclass'],
                         'inventory_types': s['EquippedItemInvTypes']},
            'stack': s['CumulativeAura'], 'max_targets': s['MaxTargets'], 'speed': s['Speed'],
            'stance_bar_order': s['StanceBarOrder'], 'icon': client['icons'][s['SpellIconID']]['TextureFilename'],
            'effects': effects}


def outgoing(s, spells, sql, src):
    """Every link out of one spell: its Spell.dbc triggers, ClassicDB rows and the CMaNGOS code paths that cast or
    apply another spell for it."""
    sid, family, icon, effects, auras = s['ID'], s['SpellClassSet'], s['SpellIconID'], s['Effect'], s['EffectAura']
    paladin, out = family == FAMILY, []

    def add(to, kind, source, *evidence, **extra):
        if to not in spells:
            raise ValueError(f'{sid} links to {to}, absent from Spell.dbc')
        out.append({'from': sid, 'to': to, 'kind': kind, 'source': source, 'evidence': list(evidence), **extra})

    for i, to in enumerate(s['EffectTriggerSpell']):
        if to:
            note = {'note': 'CMaNGOS replaces this trigger (proc_override)'} if paladin and icon == 241 else {}
            add(to, 'trigger', 'dbc', f'Spell.dbc {sid} EffectTriggerSpell[{i}]', **note)
    if paladin and 77 in effects and icon in (70, 242):   # Spell::EffectScriptEffect: the heal is a second spell
        heal, test = (19968, 'SpellIconID == 70') if icon == 70 else (19993, 'SpellIconID  == 242')
        add(heal, 'script_effect', 'cmangos', src.cite(EFFECTS, test),
            src.cite(EFFECTS, f'CastCustomSpell(unitTarget, {heal}'),
            note='heals for the rank\'s own rolled value; the rank id rides along as basepoints[1]')
    if sid in HOLY_SHOCK:
        harm, heal = HOLY_SHOCK[sid]
        line = src.cite(EFFECTS, f'case {sid}: hurt = {harm}; heal = {heal}')
        add(harm, 'dummy', 'cmangos', src.cite(EFFECTS, 'case 156:'), line, note='enemy target')
        add(heal, 'dummy', 'cmangos', src.cite(EFFECTS, 'case 156:'), line, note='friendly target (CanAssistSpell)')
    if paladin and icon == 561:                           # Judgement of Command's dummy names the damage spell
        for i in range(3):
            if effects[i] == 3:
                add(s['EffectBasePoints'][i] + s['EffectBaseDice'][i], 'dummy', 'cmangos', src.cite(EFFECTS, 'case 561:'),
                    src.cite(EFFECTS, 'LookupEntry<SpellEntry>(m_currentBasePoints[eff_idx])'),
                    note=f'Spell.dbc effect {i} value (SpellEntry::CalculateSimpleValue)')
    if sid in SEAL_OF_RIGHTEOUSNESS:
        add(SEAL_OF_RIGHTEOUSNESS[sid], 'dummy_proc', 'cmangos',
            src.cite(PROCS, 'Seal of Righteousness - melee proc dummy'),
            src.cite(PROCS, f'case {sid}: spellId = {SEAL_OF_RIGHTEOUSNESS[sid]}'),
            note='each melee hit; damage from weapon speed and the seal\'s effect 0 (one- or two-handed formula)')
    # spell_judgement: Judgement removes the caster's seal and casts the seal's effect 2 value.
    if paladin and (u32(s['SpellClassMask'][0]) & SEAL_FLAGS) and auras[2] == 4:
        judgement = s['EffectBasePoints'][2] + s['EffectBaseDice'][2]
        if judgement > 1:
            add(judgement, 'judgement', 'cmangos', src.cite(MGR_H, hex(SEAL_FLAGS)[2:].upper().rjust(16, '0')),
                src.cite(SCRIPT, 'CalculateSimpleValue(EFFECT_INDEX_2)'), src.cite(BINDINGS, "(20271,'spell_judgement')"),
                via=20271, note='Spell.dbc effect 2 value; cast by Judgement, which consumes the seal')
    if sid in JUDGEMENT_PAYLOADS:
        via = s['EffectTriggerSpell'][0]
        if via not in INTERMEDIATES:
            raise ValueError(f'{sid}: judgement aura without its intermediate trigger')
        to = JUDGEMENT_PAYLOADS[sid]
        add(to, 'script', 'cmangos', src.cite(SCRIPT, f'case {sid}: triggerSpell = {to}'),
            src.cite(BINDINGS, f"({via},'{INTERMEDIATES[via]}')"), via=via,
            note='cast by the judged target on its attacker')
    if s['Mechanic'] == INVULNERABILITY and sid != FORBEARANCE:   # Spell::cast adds Forbearance as a precast spell
        if family not in (0, FAMILY):
            raise ValueError(f'{sid}: invulnerability outside the generic and Paladin cases')
        test = 'm_spellInfo->Id != 25771' if paladin else 'else if (m_spellInfo->Mechanic == MECHANIC_INVULNERABILITY)'
        add(FORBEARANCE, 'precast', 'cmangos', src.cite(SPELL, test), note='MECHANIC_INVULNERABILITY')
    if paladin and icon == 241 and 42 in auras:           # Illumination
        add(20272, 'proc_override', 'cmangos', src.cite(PROCS, 'SpellIconID == 241'),
            src.cite(PROCS, 'trigger_spell_id = 20272'),
            note='on a critical heal: mana equal to the base cost of the healing rank (Holy Shock: its rank)')
    if sid in EYE_FOR_AN_EYE:
        add(EYE_FOR_AN_EYE[sid], 'dummy_proc', 'cmangos', src.cite(PROCS, f'case {sid}:'),
            src.cite(PROCS, f'triggered_spell_id = {EYE_FOR_AN_EYE[sid]};'),
            note='magic critical hits taken; capped at half the paladin\'s maximum health')
    for row in sql['dbscripts_on_spell']:
        if row['id'] == sid:
            if row['command'] != CAST_SPELL or 77 not in effects:
                raise ValueError(f'{sid}: unexpected dbscripts_on_spell command {row["command"]}')
            add(row['datalong'], 'db_script', 'classicdb', f'ClassicDB dbscripts_on_spell {sid}: {row["comments"]}',
                src.cite(EFFECTS, 'ScriptsStart(SCRIPT_TYPE_SPELL', after='void Spell::EffectScriptEffect'),
                src.cite('src/game/DBScripts/ScriptMgr.h', 'SCRIPT_COMMAND_CAST_SPELL', f'= {CAST_SPELL},'))
    for row in sql['spell_learn_spell']:
        if row['entry'] == sid:
            add(row['SpellID'], 'learn', 'classicdb', f'ClassicDB spell_learn_spell {sid}',
                src.cite(MGR, 'FROM spell_learn_spell'))
    return out


def closure(ranks, spells, sql, src):
    reached, links, todo = set(), [], sorted(ranks)
    while todo:
        sid = todo.pop()
        if sid in reached:
            continue
        reached.add(sid)
        for link in outgoing(spells[sid], spells, sql, src):
            links.append(link)
            todo.append(link['to'])
    return reached, sorted(links, key=lambda l: (l['from'], l['to'], l['kind']))


def handlers(reached, spells, src):
    """Code paths that shape a reached spell without casting another one."""
    def having(test):
        return sorted(sid for sid in reached if test(spells[sid]))

    lay_on_hands = having(lambda s: 67 in s['Effect'])
    if any(not spells[sid]['AttributesEx'] & 0x2 for sid in lay_on_hands):
        raise ValueError('Lay on Hands without SPELL_ATTR_EX_USE_ALL_MANA')
    return [
        {'spells': lay_on_hands, 'behaviour': 'heals the target for the caster\'s maximum health and costs all of '
         'the caster\'s remaining mana (ranks 2-3 restore their effect 1 mana by Spell.dbc)',
         'evidence': [src.cite(EFFECTS, 'uint32 heal = m_caster->GetMaxHealth();'),
                      src.cite(SPELL, 'spellInfo->HasAttribute(SPELL_ATTR_EX_USE_ALL_MANA)')]},
        {'spells': [20233, 20236], 'behaviour': 'Improved Lay on Hands\' armor bonus goes on the Lay on Hands target',
         'evidence': [src.cite(PROCS, 'case 20233: // Improved Lay on Hands'),
                      src.cite(PROCS, 'if (target == nullptr)')]},
        {'spells': having(lambda s: 153 in s['EffectAura']), 'behaviour': 'SPLIT_DAMAGE_FLAT: up to the aura amount '
         'of each hit taken is dealt to the living caster instead (Blessing of Sacrifice); no payload spell',
         'evidence': [src.cite(UNIT, 'GetAurasByType(SPELL_AURA_SPLIT_DAMAGE_FLAT)')]},
        {'spells': having(lambda s: 43 in s['EffectAura']), 'behaviour': 'PROC_TRIGGER_DAMAGE: the aura\'s own spell '
         'deals its amount to the attacker on the proc (Holy Shield on block, Blessing of Sanctuary); no payload spell',
         'evidence': [src.cite(PROCS, 'SpellNonMeleeDamage spellDamageInfo(this, victim, spellInfo->Id')]},
        {'spells': having(lambda s: s['ID'] in (21082, 20162, 20305, 20306, 20307, 20308)),
         'behaviour': 'Seal of the Crusader: its effect 1 attack speed is offset by a main-hand damage reduction '
                      '(weapon DPS kept) and a base DPS correction; x1.4 damage done by the spells its dummy affects',
         'evidence': [src.cite(AURAS, 'm_modAttackBaseDPSPct[BASE_ATTACK], 40'), src.cite(SCRIPT, 'float reduction ='),
                      src.cite(UNIT, 'DoneTotalMod *= 1.4f')]},
        {'spells': having(lambda s: s['SpellClassSet'] == FAMILY and u32(s['SpellClassMask'][0]) & SEAL_FLAGS),
         'behaviour': 'an active seal sets AURA_STATE_JUDGEMENT, the caster state Judgement requires',
         'evidence': [src.cite(AURAS, 'ModifyAuraState(AURA_STATE_JUDGEMENT, true)')]},
        {'spells': having(lambda s: s['SpellClassSet'] == FAMILY and u32(s['AttributesExC']) & 0x40000 and
                          6 in s['Effect']),
         'behaviour': 'the caster\'s melee hits refresh his judgements (SPELL_ATTR_EX3_ALWAYS_HIT) on the victim',
         'evidence': [src.cite(UNIT, 'RefreshHolder(); // TODO: Judgement')]},
        {'spells': [20467, 20963, 20964, 20965, 20966], 'behaviour': 'Judgement of Command deals half damage unless '
         'the target is stunned', 'evidence': [src.cite(SCRIPT, 'spell->SetDamage(uint32(spell->GetDamage() / 2))'),
                                               src.cite(BINDINGS, "(20467,'spell_judgement_of_command')")]},
        {'spells': [19968], 'behaviour': 'Holy Light\'s heal is instant (its own Spell.dbc cast time is ignored)',
         'evidence': [src.cite(MGR, 'if (spellInfo->Id == 19968)')]},
        {'spells': sorted(HOLY_SHOCK), 'behaviour': 'Holy Shock needs a target it can assist or attack; an enemy '
         'must be in front', 'evidence': [src.cite(SPELL, 'SpellIconID == 156')]},
        {'spells': having(lambda s: s['Mechanic'] == 23), 'behaviour': 'MECHANIC_TURN is single-target: a new Turn '
         'Undead ends the previous one', 'evidence': [src.cite(MGR_H, 'case MECHANIC_TURN:')]},
        {'spells': having(lambda s: 19 in s['Effect']), 'behaviour': 'ADD_EXTRA_ATTACKS (Reckoning): extra '
         'immediate swings', 'evidence': [src.cite(EFFECTS, 'void Spell::EffectAddExtraAttacks')]},
        {'spells': [19977, 19978, 19979, 25890], 'behaviour': 'Blessing of Light adds its effect amounts to Holy '
         'Light (effect 0) and Flash of Light (effect 1) healing taken',
         'evidence': [src.cite(SCRIPT, 'SCRIPT_LOCATION_SPELL_HEALING_TAKEN'),
                      src.cite(BINDINGS, "(25890,'spell_blessing_of_light')")]},
        {'spells': [19752], 'behaviour': 'Divine Intervention cannot target a flag carrier (23333/23335)',
         'evidence': [src.cite(SCRIPT, 'target->HasAura(23333)'),
                      src.cite(BINDINGS, "(19752,'spell_divine_intervention')")]},
    ]


def stacking_groups(reached, src):
    """spell_group/spell_group_spell rows (SpellStacking.h SpellGroupRule) holding reached spells."""
    text = (src.root / GROUPS).read_text()
    groups_sql, members_sql = text.split('INSERT INTO spell_group(')[1].split('INSERT INTO spell_group_spell(')
    rules = {1: 'unique', 2: 'unique_per_caster'}
    groups = {int(g): {'name': name, 'rule': rules[int(rule)], 'spells': []}
              for g, rule, name in re.findall(r"\((\d+),(\d+),'([^']*)'\)", groups_sql)}
    for g, sid in re.findall(r'\((\d+),(\d+)\)', members_sql):
        groups[int(g)]['spells'].append(int(sid))
    src.cite('src/game/Spells/SpellStacking.h', 'UNIQUE_PER_CASTER', '= 2')
    src.cite(GROUPS, 'INSERT INTO spell_group_spell')
    return {str(g): {**v, 'spells': sorted(v['spells'])} for g, v in sorted(groups.items())
            if set(v['spells']) & reached}


def quest_rows(sql, wanted, src):
    """The rewarding quests and everything CMaNGOS requires before them (ObjectMgr::LoadQuests prevQuests: PrevQuestId,
    and NextQuestId naming the quest), plus class quests rewarding an item a chain quest requires."""
    by_id = {q['entry']: q for q in sql['quest_template']}
    prev = {e: [q['PrevQuestId']] if q['PrevQuestId'] else [] for e, q in by_id.items()}
    for e, q in by_id.items():
        if abs(q['NextQuestId']) in by_id:
            prev[abs(q['NextQuestId'])].append(-e if q['NextQuestId'] < 0 else e)
    rewards = {}
    for q in sql['quest_template']:
        if q['RequiredClasses'] & 1 << (CLASS_ID - 1):
            for i in range(1, 5):
                if q[f'RewItemId{i}']:
                    rewards.setdefault(q[f'RewItemId{i}'], set()).add(q['entry'])
    src.cite(QUESTS, 'qinfo->prevQuests.push_back(qinfo->PrevQuestId)')
    src.cite(QUESTS, 'qNextItr->second->prevQuests.push_back(signedQuestId)')
    seen, todo = set(), sorted(wanted)
    while todo:
        e = todo.pop()
        if e in seen:
            continue
        seen.add(e)
        todo += [abs(p) for p in prev[e]]
        todo += [r for i in range(1, 5) for r in rewards.get(by_id[e][f'ReqItemId{i}'], ())]
    return {str(e): {'title': q['Title'], 'min_level': q['MinLevel'], 'level': q['QuestLevel'],
                     'races': q['RequiredRaces'], 'classes': q['RequiredClasses'], 'prev_quests': sorted(set(prev[e])),
                     'exclusive_group': q['ExclusiveGroup'], 'reward_spell': q['RewSpell'],
                     'reward_spell_cast': q['RewSpellCast'], 'money': q['RewOrReqMoney'], 'source_item': q['SrcItemId'],
                     'required_items': [[q[f'ReqItemId{i}'], q[f'ReqItemCount{i}']]
                                        for i in range(1, 5) if q[f'ReqItemId{i}']],
                     'reward_items': [q[f'RewItemId{i}'] for i in range(1, 5) if q[f'RewItemId{i}']]}
            for e, q in sorted(by_id.items()) if e in seen}


def build(dump=DEFAULT_DUMP, dbc_dir=DBC_DIR, progression=REPO / 'data/progression.json',
          talents=REPO / 'data/talents.json', cmangos=CMANGOS):
    src = Source(cmangos)
    client = {name: dbc.read(Path(dbc_dir) / f'{name}.dbc') for name in DBC_INPUTS}
    spells = dbc.by_id(client['Spell'])
    lookups = {'cast_times': dbc.by_id(client['SpellCastTimes']), 'durations': dbc.by_id(client['SpellDuration']),
               'ranges': dbc.by_id(client['SpellRange']), 'radii': dbc.by_id(client['SpellRadius']),
               'icons': dbc.by_id(client['SpellIcon'])}
    sql = tables(Path(dump), {
        'quest_template': None, 'playercreateinfo': None, 'playercreateinfo_spell': ('race', 'class', 'Spell'),
        'item_template': ('entry', 'name', 'RequiredLevel', 'AllowableClass', 'AllowableRace', 'Quality') +
                         tuple(f'spellid_{i}' for i in range(1, 6)),
        'dbscripts_on_spell': ('id', 'command', 'datalong', 'comments'), 'spell_learn_spell': None,
        'creature_template': ('Entry', 'Name', 'ModelId1')},
        filters={'playercreateinfo': lambda r: r['class'] == CLASS_ID,
                 'playercreateinfo_spell': lambda r: r['class'] == CLASS_ID})
    book = json.loads(Path(progression).read_text())['classes'][KEY]
    tree = json.loads(Path(talents).read_text())['classes'][KEY]
    mask = 1 << (CLASS_ID - 1)
    if dbc.by_id(client['ChrClasses'])[CLASS_ID]['SpellClassSet'] != FAMILY:
        raise ValueError('ChrClasses names another spell family for the Paladin')

    # Races: ClassicDB playercreateinfo and the client's CharBaseInfo must agree.
    races = sorted({r['race'] for r in sql['playercreateinfo']})
    if races != sorted(r['RaceID'] for r in client['CharBaseInfo'] if r['ClassID'] == CLASS_ID):
        raise ValueError('playercreateinfo and CharBaseInfo disagree on Paladin races')
    names = dbc.by_id(client['ChrRaces'])
    created = {}
    for r in sql['playercreateinfo_spell']:
        created.setdefault(r['race'], []).append(r['Spell'])

    # Ranks: the progression book (trainer, starting, quest, item, talent) and the talent trees.
    talent_of = {sid: (t, tr, n) for tr in tree['trees'] for t in tr['talents']
                 for n, sid in enumerate(t['rank_spells'], 1)}
    talent_chain = {sid: talent_of[sid][0]['id'] for sid in talent_of}
    items = {r['entry']: r for r in sql['item_template']}
    quests = {q['entry']: q for q in sql['quest_template']}
    ranks = []
    for row in book['spells'] + book['excluded']:
        sid = row['spell']
        rank = {k: row[k] for k in ('spell', 'name', 'rank_name', 'chain', 'rank', 'previous_spell', 'required_spell',
                                    'required_level', 'tab', 'skill_line', 'skill', 'passive', 'source')}
        provenance = {}
        if row['source'] == 'starting':
            provenance['starting_races'] = sorted(race for race, ids in created.items() if sid in ids)
        if row.get('trainer_options'):
            provenance['trainer'] = row['trainer_options']
        if sid in talent_of:
            t, tr, n = talent_of[sid]
            provenance['talent'] = {'id': t['id'], 'name': t['name'], 'tab': tr['tab'], 'tier': t['tier'],
                                    'column': t['column'], 'rank': n, 'max_rank': t['max_rank']}
        elif row['source'] == 'talent':       # a trainer upgrade of a talent-granted chain
            rank['source'] = 'talent_trainer'
            provenance['talent_chain'] = talent_chain[row['chain']]
            provenance['trainer'] = tree['trainer_options'][str(sid)]
        for e in row.get('quests', []):
            q = quests[e]
            taught = [q['RewSpell']] if q['RewSpell'] else []
            if q['RewSpellCast']:
                taught += [spells[q['RewSpellCast']]['EffectTriggerSpell'][i] for i in range(3)
                           if spells[q['RewSpellCast']]['Effect'][i] == 36]
            if sid not in taught:
                raise ValueError(f'quest {e} does not teach {sid}')
            provenance.setdefault('quests', []).append({'quest': e, 'teaching_spell': q['RewSpellCast'] or q['RewSpell']})
        for e in row.get('items', []):
            teaching = [items[e][f'spellid_{i}'] for i in range(1, 6) if items[e][f'spellid_{i}'] and
                        sid in [spells[items[e][f'spellid_{i}']]['EffectTriggerSpell'][k] for k in range(3)
                                if spells[items[e][f'spellid_{i}']]['Effect'][k] == 36]]
            if len(teaching) != 1:
                raise ValueError(f'item {e} does not teach {sid}')
            it = items[e]
            provenance.setdefault('items', []).append({
                'item': e, 'name': it['name'], 'required_level': it['RequiredLevel'], 'allowable_class': it['AllowableClass'],
                'allowable_race': it['AllowableRace'], 'teaching_spell': teaching[0]})
        if not provenance:
            raise ValueError(f'{sid}: no provenance')
        ranks.append({**rank, 'provenance': provenance})
    ids = {r['spell'] for r in ranks}
    if len(ids) != len(ranks) or set(talent_of) - ids:
        raise ValueError('duplicate ranks or talent ranks missing from the progression book')
    race_mask = sum(1 << (r - 1) for r in races)
    abilities = {}
    for a in client['SkillLineAbility']:
        abilities.setdefault(a['Spell'], []).append(a)
    for sid in ids:                            # no other class's rank; open to every Paladin race
        rows = abilities.get(sid, [])
        if not rows or spells[sid]['SpellClassSet'] not in (0, FAMILY) or any(
                a['ClassMask'] and not a['ClassMask'] & mask or a['ExcludeClass'] & mask or
                a['RaceMask'] and a['RaceMask'] & race_mask != race_mask or a['ExcludeRace'] & race_mask for a in rows):
            raise ValueError(f'{sid} is not a rank of every Paladin')

    reached, links = closure(ids, spells, sql, src)
    later = []
    for sid, path, needle, what in LATER_IDS:
        if sid in spells:
            raise ValueError(f'{sid} exists in 1.12.1')
        later.append({'ref': sid, 'evidence': [src.cite(path, needle)],
                      'reason': f'{what}; absent from 1.12.1 Spell.dbc'})
    if src.mentions(2689) or spells[20154]['Effect'][2]:
        raise ValueError('an unresolved entry below no longer holds')
    unresolved = [
        {'ref': 2689, 'spells': sorted(s for s in reached if 112 in spells[s]['EffectAura'] and
                                       2689 in spells[s]['EffectMiscValue']),
         'evidence': [], 'reason': 'Illumination effect 1 is OVERRIDE_CLASS_SCRIPTS 2689, which no CMaNGOS src/game file '
                                   'handles: inert'},
        {'ref': 20154, 'spells': [20154], 'evidence': [src.cite(SCRIPT, 'GetEffIndex() != 2')],
         'reason': 'the starting Seal of Righteousness has no effect 2: Judgement finds no seal, casts nothing and '
                   'leaves it active (21084 and later ranks judge)'},
        *later]

    mounts = []
    displays, models = dbc.by_id(client['CreatureDisplayInfo']), dbc.by_id(client['CreatureModelData'])
    creatures = {c['Entry']: c for c in sql['creature_template']}
    for sid in sorted(reached):
        s = spells[sid]
        if 78 in s['EffectAura']:              # SPELL_AURA_MOUNTED: misc is the creature
            entry = s['EffectMiscValue'][s['EffectAura'].index(78)]
            display = creatures[entry]['ModelId1']
            mounts.append({'spell': sid, 'creature': entry, 'creature_name': creatures[entry]['Name'], 'display': display,
                           'model': models[displays[display]['ModelID']]['ModelName'],
                           'speed_percent': s['EffectBasePoints'][s['EffectAura'].index(32)] + 1})

    rank_items = {i['item'] for r in ranks for i in r['provenance'].get('items', [])}
    rank_quests = {q['quest'] for r in ranks for q in r['provenance'].get('quests', [])}
    order = lambda r: (r['tab'], r['chain'], r['rank'], r['spell'])
    # Everything that cites the source runs before provenance lists the cited files.
    shaped, groups = handlers(reached, spells, src), stacking_groups(reached, src)
    chains = quest_rows(sql, rank_quests, src)
    return {
        'schema': 1,
        'provenance': {'client_build': '1.12.1.5875', 'dump_sha256': sha256(dump),
                       'progression_sha256': sha256(progression), 'talents_sha256': sha256(talents),
                       'dbc_sha256': {name: sha256(Path(dbc_dir) / f'{name}.dbc') for name in DBC_INPUTS},
                       'cmangos_commit': CMANGOS_COMMIT,
                       'cmangos_sha256': {p: sha256(src.root / p) for p in sorted(src.cited)}},
        'class': {'id': CLASS_ID, 'key': KEY, 'name': book['name'], 'family': FAMILY,
                  'races': {str(r): {'name': names[r]['Name_lang'], 'create_spells': sorted(created[r])} for r in races},
                  'race_mask': race_mask},
        'ranks': sorted(ranks, key=order),
        'counts': {'ranks': len(ranks), 'talents': len({t['id'] for t, _, _ in talent_of.values()}),
                   'talent_ranks': len(talent_of), 'reached': len(reached), 'payloads': len(reached - ids),
                   'links': len(links), 'code_links': sum(l['source'] != 'dbc' for l in links),
                   'unresolved': len(unresolved)},
        'spells': {str(sid): summary(spells[sid], lookups) for sid in sorted(reached)},
        'links': links,
        'handlers': shaped,
        'unresolved': unresolved,
        'stacking_groups': groups,
        'quests': chains,
        'items': sorted(rank_items),
        'mounts': mounts,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dump', type=Path, default=DEFAULT_DUMP)
    parser.add_argument('--dbc-dir', type=Path, default=DBC_DIR)
    parser.add_argument('--cmangos', type=Path, default=CMANGOS)
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    manifest = build(args.dump, args.dbc_dir, cmangos=args.cmangos)
    args.out.write_text(json.dumps(manifest, indent=1, sort_keys=True) + '\n')
    print(f'{args.out}: {json.dumps(manifest["counts"], sort_keys=True)}')


if __name__ == '__main__':
    main()
