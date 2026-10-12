#!/usr/bin/env python3
"""API reference: what the real 1.12 client's getters answer, recorded in-game, for the wowcore API shim's tests.

usage: apiref.py gen               write the probe list (every getter FrameXML calls, with argument sets) into the
                                   WBOracle addon and install it into the client
       apiref.py collect [STATE..] put the client in each state, run `/wbo api STATE`, save -> evidence/apiref/
       apiref.py events            record the client's event sequence (login, HUD actions, /reload) -> evidence/events/
       apiref.py lua               from the recording, write wowcore's client defaults (wowcore/lua/wowapi_defaults.lua),
                                   the answers of systems WoWBridge does not model (wowcore/lua/wowapi_recorded.lua)
                                   and the reference-state fixture (wowcore/tests/fixtures/elunara.lua)
States: idle (no target), target (combat tester with Moonfire + Roots), self (self-targeted, Rejuvenation on).
"""
import re, shutil, subprocess, sys, time
from pathlib import Path
from wowref import saved_variables

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build/apiref'
ADDON = ROOT / 'tools/addons/WBOracle'
CLIENT_ADDON = ROOT.parent / 'WoW/client/Interface/AddOns/WBOracle'
OUT = ROOT / 'evidence/apiref'
TESTER = '[PH[ Combat Tester'

GETTER = re.compile(r'^(Get|Unit|Is|Has|Can(?!cel)|In(Cinematic|RepairMode)|Check|Spell(Is|Can)|Pet(Can|Has)|Taxi|Item[A-Z]|GuildControlGet|'
                    r'Tutorials|Loot|Logging|Resurrect|Num)')
NOT_PURE = {'GetQuestReward', 'GuildRoster', 'LootSlot', 'ItemTextNextPage', 'ItemTextPrevPage', 'LoggingChat',
            'LoggingCombat', 'CheckInbox', 'GetGossipText'}           # commands or toggles that share a getter prefix
UNITS = ['player', 'target', 'pet', 'party1', 'mouseover', 'targettarget']
PAIR = {'UnitIsUnit', 'UnitReaction', 'UnitCanAttack', 'UnitCanCooperate', 'UnitIsFriend', 'UnitIsEnemy'}
SLOTS = ['AmmoSlot', 'HeadSlot', 'NeckSlot', 'ShoulderSlot', 'ShirtSlot', 'ChestSlot', 'WaistSlot', 'LegsSlot',
         'FeetSlot', 'WristSlot', 'HandsSlot', 'Finger0Slot', 'Finger1Slot', 'Trinket0Slot', 'Trinket1Slot',
         'BackSlot', 'MainHandSlot', 'SecondaryHandSlot', 'RangedSlot', 'TabardSlot', 'Bag0Slot', 'Bag1Slot',
         'Bag2Slot', 'Bag3Slot']


def q(s: str) -> str:
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\r', '\\r') + '"'


def _signatures() -> dict[str, str]:
    """Each function's parameter list from its own usage error, as an earlier recording caught it."""
    try:
        recorded = load()
    except (FileNotFoundError, ValueError):
        return {}
    return {m[1]: m[2] for answers in recorded.values() for a in answers.values()
            for m in [re.search(r'Usage: (\w+)\(([^)]*)\)', a)] if m}


SIGNATURES: dict[str, str] = {}


def _from_signature(sig: str) -> list[str]:
    """Calls built from a usage text: unit parameters get unit tokens, others the indices 1 and 2."""
    params = [p.strip() for p in sig.replace('[', '').replace(']', '').split(',') if p.strip()]
    if not any('unit' in p.lower() for p in params):
        return []
    calls = ['']
    for p in params:
        values = [q(u) for u in ('player', 'target', 'mouseover')] if 'unit' in p.lower() else ['1', '2']
        calls = [f'{c}, {v}' if c else v for c in calls for v in values]
    return calls[:12]


def args_for(name: str) -> list[str]:
    """Argument lists (as Lua source) for one getter. Wrong arities are harmless: each probe runs under pcall."""
    if name in SIGNATURES and (calls := _from_signature(SIGNATURES[name])):
        return calls + ['', '1', '2']
    if name == 'GetPlayerMapPosition':
        return [q(u) for u in ('player', 'target')]
    if name in PAIR:
        return [f'{q(a)}, {q(b)}' for a, b in (('player', 'player'), ('player', 'target'), ('target', 'player'))]
    if name in ('UnitBuff', 'UnitDebuff'):
        return [f'{q(u)}, {i}' for u in ('player', 'target') for i in range(1, 4)] + [f'{q("target")}, 1, 1']
    if name.startswith('Unit'):
        return [q(u) for u in UNITS]
    if 'Action' in name and not name.startswith('GetPet') and name != 'GetActionBarToggles':
        return [str(i) for i in range(1, 14)] + ['61', '73']
    if name in ('GetSpellName', 'GetSpellTexture', 'GetSpellCooldown', 'IsSpellPassive', 'GetSpellAutocast'):
        return [f'{i}, "spell"' for i in range(1, 141)]
    if name == 'GetSpellTabInfo':
        return [str(i) for i in range(1, 6)]
    if name == 'UnitStat':
        return [f'{q(u)}, {i}' for u in ('player', 'target') for i in range(1, 6)]
    if name == 'UnitResistance':
        return [f'{q(u)}, {i}' for u in ('player', 'target') for i in range(0, 7)]
    if name == 'GetItemQualityColor':
        return [str(i) for i in range(-1, 8)]
    if name.startswith('GetPlayerBuff') and name != 'GetPlayerBuff':
        return [str(i) for i in range(0, 4)]
    if name == 'GetPlayerBuff':
        return [f'{i}, {q(f)}' for f in ('HELPFUL', 'HARMFUL', 'HELPFUL|PASSIVE') for i in range(0, 4)]
    if name == 'GetInventorySlotInfo':
        return [q(s) for s in SLOTS]
    if name.startswith('GetInventoryItem') or name == 'IsInventoryItemLocked':
        return [f'"player", {i}' for i in range(0, 24)]
    if name.startswith('GetContainer'):
        return [str(b) for b in range(-2, 5)] + [f'{b}, {s}' for b in (0, 1) for s in range(1, 5)]
    if name == 'GetBindingKey':
        return []                                            # emitted from Bindings.xml below
    if name == 'GetCVar' or name == 'GetCVarDefault':
        return []                                            # emitted from the interface's cvar names below
    return ['', '1', '2']


def gen() -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    probe = ROOT / 'build/wowcore/framexml_probe'
    subprocess.run([probe, '--dump', BUILD / 'ui', '--globals', BUILD / 'globals.txt'], cwd=BUILD,
                   capture_output=True, check=False)
    surface = subprocess.run([sys.executable, ROOT / 'tools/wowapi_surface.py', BUILD / 'ui', BUILD / 'globals.txt'],
                             capture_output=True, text=True, check=True).stdout
    names = sorted({line.split('\t')[0] for line in surface.splitlines()} |
                   {'GetBindingKey', 'GetCVar', 'GetNumBindings', 'GetBinding', 'GetTime', 'GetSpellName',
                    'UnitStat', 'UnitResistance', 'GetItemQualityColor', 'GetInventoryItemCount',
                    'GetInventoryItemQuality', 'UnitCreatureType', 'UnitIsVisible'})
    engine = set((BUILD / 'globals.txt').read_text().split())    # WoWee's own definitions: check those too
    names = sorted(set(names) | {n for n in engine if GETTER.match(n)})
    names = [n for n in names if (GETTER.match(n) or n.startswith('GetBinding')) and n not in NOT_PURE]
    fx = BUILD / 'ui/Interface/FrameXML'
    text = '\n'.join(p.read_text(errors='replace') for p in fx.iterdir() if p.suffix in ('.lua', '.xml'))
    cvars = sorted(set(re.findall(r'GetCVar(?:Default)?\("(\w+)"\)', text)) | set(re.findall(r'\bcvar\s*=\s*"(\w+)"', text)))
    bindings = re.findall(r'<Binding\s+name="([^"]+)"', (fx / 'Bindings.xml').read_text(errors='replace'))
    SIGNATURES.update(_signatures())
    probes = [f'{n}({a})' for n in names for a in args_for(n)]
    probes += [f'GetCVar({q(c)})' for c in cvars] + [f'GetCVarDefault({q(c)})' for c in cvars]
    probes += [f'GetBindingKey({q(b)})' for b in bindings] + [f'GetBinding({i})' for i in range(1, len(bindings) + 40)]
    body = ',\n'.join(f'  {q(p)}' for p in probes)
    ui = '\n'.join(p.read_text(errors='replace') for p in (BUILD / 'ui').rglob('*') if p.suffix in ('.lua', '.xml'))
    events = sorted(set(re.findall(r'RegisterEvent\("([A-Z_0-9]+)"\)', ui)))
    tips = ([f'SetAction({i})' for i in range(1, 13)] + [f'SetSpell({i}, "spell")' for i in range(1, 129)] +
            [f'SetUnit({q(u)})' for u in ('player', 'target')] + [f'SetInventoryItem("player", {i})' for i in range(0, 20)] +
            [f'SetBagItem(0, {i})' for i in range(1, 5)])
    (ADDON / 'WBOracleProbes.lua').write_text(f'-- Generated by tools/apiref.py gen; do not edit.\nWBORACLE_PROBES = {{\n{body}\n}}\n'
                                              f'WBORACLE_EVENTS = {{ {", ".join(q(e) for e in events)} }}\n'
                                              f'WBORACLE_TIPS = {{ {", ".join(q(t) for t in tips)} }}\n')
    shutil.copytree(ADDON, CLIENT_ADDON, dirs_exist_ok=True)
    print(f'{len(probes)} probes over {len(names)} getters, {len(cvars)} cvars, {len(bindings)} bindings, {len(events)} events')


SELF = 'slash:/script TargetUnit("player")'
STATES = {
    'idle': ['slash:/script ClearTarget()', 'wait:1'],
    'target': ['slash:.npc tempspawn 11080 3600000', 'wait:2', f'slash:/wbo target {TESTER}', 'slash:/cast Moonfire', 'wait:1.7', 'slash:/cast Entangling Roots', 'wait:2.2'],
    'self': [SELF, 'slash:/cast Rejuvenation', 'wait:1.7'],
}


EVENTS = ROOT / 'evidence/events'


def save_sv(dst: Path) -> None:
    """Copy the client's SavedVariables as standard Lua: LF line ends, and a carriage return inside a string (spell
    descriptions carry them) escaped, where the client writes it raw."""
    dst.write_bytes(saved_variables().read_bytes().replace(b'\r\n', b'\n').replace(b'\r', b'\\r'))


def events() -> None:
    """Record the client's event sequence: a fresh login, then the HUD actions, then a /reload (wowref drives it)."""
    import wowref
    EVENTS.mkdir(parents=True, exist_ok=True)
    subprocess.run(['pkill', '-x', 'WoW.exe'], check=False)
    time.sleep(5)
    wowref.main(['launch', 'login', 'wait:65',                          # the login log closes 60 s after PEW
                 'slash:/wbo events on', 'slash:/script ClearTarget()', 'wait:2',
                 'slash:.npc tempspawn 11080 3600000', 'wait:2', *STATES['target'], 'wait:3',
                 'slash:/script ClearTarget()', 'wait:2', *STATES['self'], 'wait:3',
                 'slash:/cast Healing Touch', 'wait:4', 'slash:/script ClearTarget()', 'wait:2',
                 'slash:/wbo events off', 'slash:/run ReloadUI()', 'wait:10'])
    save_sv(EVENTS / 'login_actions.lua')
    wowref.main(['wait:65', 'slash:/run ReloadUI()', 'wait:10'])
    save_sv(EVENTS / 'reload.lua')
    print('saved', EVENTS)


def _unlua(s: str) -> str:
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 'r': '\r', '0': '\0'}.get(m[1], m[1]), s, flags=re.S)


def load(path: Path = OUT / 'WBOracle_api.lua') -> dict[str, dict[str, str]]:
    """{state: {probe: answers}} from the saved WBOracleAPI table (answers as the addon serialized them)."""
    text = path.read_text(encoding='latin-1')
    text = text[text.index('WBOracleAPI = {'):]
    states, cur = {}, None
    for m in re.finditer(r'^\t\["(\w+)"\] = \{$|^\t\t\["((?:\\.|[^"\\])*)"\] = "((?:\\.|[^"\\])*)",$', text, re.M):
        if m[1]:
            cur = states.setdefault(m[1], {})
        else:
            cur[_unlua(m[2])] = _unlua(m[3])
    return states


def collect(states: list[str]) -> None:
    import wowref
    OUT.mkdir(parents=True, exist_ok=True)
    steps = ['slash:/run ReloadUI()', 'wait:10', 'slash:/wbo apiclear']   # reload: pick up a regenerated probe list
    for s in states:
        steps += [*STATES[s], f'slash:/wbo api {s}', 'wait:1', f'slash:/wbo tips {s}', 'wait:1']
    wowref.main(steps + ['slash:/script ClearTarget()', 'slash:/run ReloadUI()', 'wait:8'])
    save_sv(OUT / 'WBOracle_api.lua')
    print('saved', OUT / 'WBOracle_api.lua')


LUA = ROOT / 'wowcore/lua'
# WoWee's engine functions that are real (input, screen, frames, clock). Its other API globals are 3.x stand-ins: the
# recording replaces them, and the ones 1.12 does not have at all are removed.
ENGINE_KEEP = {'GetTime', 'GetCursorPosition', 'GetMouseFocus', 'GetScreenWidth', 'GetScreenHeight', 'GetFramerate',
               'CreateFrame'}
FIXTURE = ROOT / 'wowcore/tests/fixtures/elunara.lua'
HEADER = '-- Generated by tools/apiref.py lua from evidence/apiref (the 1.12.1 client, recorded); do not edit.\n'


def _values(answer: str) -> str:
    """A recorded answer as a Lua expression list (the addon wrote %q strings and plain numbers)."""
    answer = re.sub(r'(?<![\w"])function(?![\w"])', 'tostring', answer)   # some getters answer a function value
    return re.sub(r'(?<![\w"])(table|userdata)(?![\w"])', 'nil', answer)


def _count(answer: str) -> int:
    """How many values a recorded answer holds (commas inside strings do not separate values)."""
    return len(re.findall(r'"(?:\\.|[^"\\])*"|[^,"]+', answer.replace(', ', ','))) if answer else 0


def _strings(answer: str) -> list[str]:
    return [_unlua(m) for m in re.findall(r'"((?:\\.|[^"\\])*)"', answer)]


def _hand_written() -> set[str]:
    text = (LUA / 'wowapi.lua').read_text()
    names = set(re.findall(r'^function (\w+)\(', text, re.M))
    block = text[text.index('for _, name in ipairs({'):]
    return names | set(re.findall(r'"(\w+)"', block[:block.index('}) do')])) | ENGINE_KEEP


def _spellbook(idle: dict[str, str]) -> list[int]:
    """The recorded book as spell ids: name + rank + icon against Spell.dbc, preferring druid / night elf abilities."""
    sys.path.insert(0, str(ROOT))
    from data import dbc
    d = ROOT / 'build/dbc/DBFilesClient'
    if not (d / 'Spell.dbc').exists():
        subprocess.run([ROOT / 'build/wowcore/mpq_extract', ROOT.parent / 'WoW/client/Data', 'DBFilesClient\\*.dbc',
                        ROOT / 'build/dbc'], check=True, capture_output=True)
    spells, icons = dbc.read(d / 'Spell.dbc'), dbc.by_id(dbc.read(d / 'SpellIcon.dbc'))
    abilities: dict[int, list] = {}
    for a in dbc.read(d / 'SkillLineAbility.dbc'):
        abilities.setdefault(a['Spell'], []).append(a)
    ours = lambda r: any((a['ClassMask'] == 0 or a['ClassMask'] & 1024) and (a['RaceMask'] == 0 or a['RaceMask'] & 8)
                         for a in abilities.get(r['ID'], []))
    book = []
    for k in range(1, 200):
        answer = idle.get(f'GetSpellName({k}, "spell")', 'nil, nil')
        if answer == 'nil, nil':
            break
        name, rank = _strings(answer)
        if name == 'Attack':                           # its icon is the main-hand weapon's, so match by name alone
            book.append(6603)
            continue
        tex = _strings(idle[f'GetSpellTexture({k}, "spell")'])[0].lower()
        cands = [r for r in spells if r['Name_lang'] == name and r['NameSubtext_lang'] == rank and
                 icons.get(r['SpellIconID'], {}).get('TextureFilename', '').lower() == tex]
        good = sorted(r['ID'] for r in cands if ours(r)) or sorted(r['ID'] for r in cands)
        if not good:
            sys.exit(f'apiref lua: no spell matches book slot {k} {name} {rank}')
        book.append(good[0])
    return book


def _recorded(states: dict[str, dict[str, str]], skip: set[str]) -> tuple[str, list[str]]:
    """Recorded answers for every probed function the hand-written API does not define."""
    by: dict[str, dict[str, dict[str, str]]] = {}
    for state, answers in states.items():
        for probe, answer in answers.items():
            name, args = probe.split('(', 1)
            by.setdefault(name, {}).setdefault(args[:-1], {})[state] = answer
    out, varying, absent = [], [], []
    for name in sorted(by):
        if name in skip:
            continue
        if all('attempt to call global' in a for v in by[name].values() for a in v.values()):
            absent.append(name)                        # not in 1.12 at all
            continue
        table = []
        for args, per_state in sorted(by[name].items()):
            answers = set(per_state.values())
            if len(answers) > 1:
                varying.append(f'{name}({args})')
            answer = per_state.get('idle', next(iter(answers)))
            m = re.match(r'error: \[string ".*?"\]:1: (.*)$', answer, re.S)
            value = f'E({q(m[1])})' if m else ('{}' if answer == '' else f'{{ n = {_count(answer)}, {_values(answer)} }}')
            table.append(f'[{q(args)}] = {value}')
        out.append(f'{name} = R({q(name)}, {{ ' + ', '.join(table) + ' })')
    # The interface's commands that are not getters: each goes to the host as __WBHost("command", name, ...).
    surface = subprocess.run([sys.executable, ROOT / 'tools/wowapi_surface.py', BUILD / 'ui', BUILD / 'globals.txt'],
                             capture_output=True, text=True, check=True).stdout
    commands = sorted({line.split('\t')[0] for line in surface.splitlines()} - set(by) - skip)
    commands = [c for c in commands if c[0].isupper() and '_' not in c]   # FrameXML-internal names are not API
    out.append('-- Commands of systems WoWBridge does not model; the host sees each one.\n'
               'for _, name in ipairs({ ' + ', '.join(q(c) for c in commands) + ' }) do\n'
               '  _G[name] = function(...) return __WB.host("command", name, ...) end\nend')
    out.append('-- Not in 1.12 at all (WoWee defines these for later clients).\n'
               'for _, name in ipairs({ ' + ', '.join(q(n) for n in absent) + ' }) do _G[name] = nil end')
    return '\n'.join(out), varying


def _tip_lines(recorded: str) -> str:
    """A recorded tooltip as the host's tooltip lines: { { left=, r=, g=, b=, right=, rr=, rg=, rb=, wrap= }, ... }."""
    rows: dict[int, dict] = {}
    for side, idx, text, r, g, b in re.findall(r'\n([LR])(\d+)=(.*?)\|([\d.]+),([\d.]+),([\d.]+)(?=\n[LR]\d+=|$)', recorded, re.S):
        row = rows.setdefault(int(idx), {})
        if side == 'L':
            row.update(left=text, r=r, g=g, b=b)
        else:
            row.update(right=text, rr=r, rg=g, rb=b)
    out = []
    for _, row in sorted(rows.items()):
        parts = [f'left = {q(row.get("left", ""))}', f'r = {row.get("r", 1)}', f'g = {row.get("g", 1)}', f'b = {row.get("b", 1)}']
        if 'right' in row:
            parts += [f'right = {q(row["right"])}', f'rr = {row["rr"]}', f'rg = {row["rg"]}', f'rb = {row["rb"]}']
        if len(row.get('left', '')) > 40:
            parts.append('wrap = 1')
        out.append('{ ' + ', '.join(parts) + ' }')
    return '{ ' + ', '.join(out) + ' }'


def lua() -> None:
    recorded = load()
    # Each tooltip as the client gave it once its caches were warm: the fullest of the recorded states (the first
    # state recorded can lack names the client had not fetched yet - reagents, set pieces).
    tips = {}
    for name, st in recorded.items():
        if name.startswith('tooltip_'):
            for k, v in st.items():
                if len(v) > len(tips.get(k, '')):
                    tips[k] = v
    states = {k: v for k, v in recorded.items() if not k.startswith(('tooltip_', 'frames_'))}
    idle, target, me = states['idle'], states['target'], states['self']
    # Client defaults: the key bindings (every row of KeyBindingFrame, headers included) and the cvar defaults.
    rows = []
    for i in range(1, 400):
        a = idle.get(f'GetBinding({i})')
        if a is None or a == '""':
            break
        rows.append('{ ' + _values(a) + ' }')
    cvars = sorted({p[len('GetCVarDefault("'):-2] for p in idle if p.startswith('GetCVarDefault("')})
    listed = {_strings(r)[0] for r in rows}
    hidden = []
    for p, a in sorted(idle.items()):
        if p.startswith('GetBindingKey("') and a not in ('', 'nil') and p[15:-2] not in listed:
            hidden.append('{ ' + q(p[15:-2]) + ', ' + _values(a) + ' }')
    defaults = ', '.join(f'[{q(c)}] = {_values(idle[f"GetCVarDefault({q(c)})"])}' for c in cvars
                         if idle[f'GetCVarDefault({q(c)})'] not in ('nil', ''))
    (LUA / 'wowapi_defaults.lua').write_text(HEADER + '-- The client\'s default key bindings and cvar defaults.\n'
                                             f'__WB.defaultBindings = {{\n  ' + ',\n  '.join(rows) + '\n}\n'
                                             f'-- Bindings KeyBindingFrame does not list, with their keys.\n'
                                             f'__WB.defaultHiddenBindings = {{ ' + ', '.join(hidden) + ' }\n'
                                             f'__WB.cvarDefaults = {{ {defaults} }}\n')
    # Systems WoWBridge does not model (guild, mail, auction, battlegrounds ...): the client's own empty answers.
    body, varying = _recorded(states, _hand_written())
    (LUA / 'wowapi_recorded.lua').write_text(
        HEADER + '-- Answers of the systems WoWBridge does not model, exactly as the client gave them with none of\n'
        '-- them in use: the empty answer for each argument recorded, and its usage error for a missing one.\n'
        '-- A call with arguments never recorded gets the answer for the nearest recorded index.\n'
        'local function E(msg) return { err = msg } end\n'
        'local function R(name, answers)\n'
        '  return function(...)\n'
        '    local key = {}\n'
        '    for i = 1, select("#", ...) do\n'
        '      local v = select(i, ...)\n'
        '      key[i] = type(v) == "string" and string.format("%q", v) or tostring(v)\n'
        '    end\n'
        '    local a = answers[table.concat(key, ", ")]\n'
        '    if not a and select("#", ...) > 0 then a = answers["2"] or answers["1"] or answers[""] end\n'
        '    a = a or answers[""] or {}\n'
        '    if a.err then error(a.err, 2) end\n'
        '    return unpack(a, 1, a.n or 0)\n'
        '  end\n'
        'end\n' + body + '\n')
    # The fixture: the reference client's state in each recorded situation.
    book = _spellbook(idle)
    # Reagent names as the client printed them, against the reagent items Spell.dbc names for each book spell.
    from data import dbc
    spells = dbc.by_id(dbc.read(ROOT / 'build/dbc/DBFilesClient/Spell.dbc'))
    item_names = {}
    for k, sid in enumerate(book, 1):
        seen = [st.get(f'SetSpell({k}, "spell")', '') for name, st in recorded.items() if name.startswith('tooltip_')]
        m = next((m for m in (re.search(r'Reagents: (.*?)\|[\d.]+,', v) for v in seen) if m), None)
        items = [i for i in spells[sid]['Reagent'] if i]
        if m and items:
            names = [re.sub(r'\|c\w{8}|\|r| \(\d+\)', '', n) for n in m[1].split(', ')]
            item_names.update(zip(items, names))
    t = lambda state, probe: '{ ' + _values(state[probe]) + ' }'
    unit = lambda st, u: '\n'.join([
        f'    armor = {t(st, f"UnitArmor({q(u)})")}, damage = {t(st, f"UnitDamage({q(u)})")},',
        f'    rangedDamage = {t(st, f"UnitRangedDamage({q(u)})")}, attackPower = {t(st, f"UnitAttackPower({q(u)})")},',
        f'    rangedAttackPower = {t(st, f"UnitRangedAttackPower({q(u)})")}, rangedAttack = {t(st, f"UnitRangedAttack({q(u)})")},',
        f'    attackBothHands = {t(st, f"UnitAttackBothHands({q(u)})")}, defense = {t(st, f"UnitDefense({q(u)})")},',
        f'    attackSpeed = {_values(st[f"UnitAttackSpeed({q(u)})"]).split(",")[0]},',
        '    stats = { ' + ', '.join(t(st, f'UnitStat({q(u)}, {i})') for i in range(1, 6) if f'UnitStat({q(u)}, {i})' in st) + ' },',
        '    resistances = { ' + ', '.join(f'[{i}] = ' + t(st, f'UnitResistance({q(u)}, {i})') for i in range(0, 7)
                                         if f'UnitResistance({q(u)}, {i})' in st) + ' },'])
    P = '"player"'
    equip = ',\n'.join(f'    [{k}] = {{ link = {_values(idle[f"GetInventoryItemLink({P}, {k})"])}, '
                        f'texture = {_values(idle[f"GetInventoryItemTexture({P}, {k})"])},\n'
                        f'            tooltip = {_tip_lines(tips.get(f"SetInventoryItem({P}, {k})", ""))} }}'
                        for k in range(0, 24) if idle.get(f'GetInventoryItemLink({P}, {k})', '') not in ('', 'nil'))
    bag = ',\n'.join(f'    [{k}] = {{ {", ".join(n + " = " + v for n, v in zip(("texture", "count", "locked", "quality", "readable"), _values(idle[f"GetContainerItemInfo(0, {k})"]).split(", ")))}, '
                      f'link = {_values(idle[f"GetContainerItemLink(0, {k})"])},\n'
                      f'          tooltip = {_tip_lines(tips.get(f"SetBagItem(0, {k})", ""))} }}'
                      for k in range(1, 17) if idle.get(f'GetContainerItemInfo(0, {k})', '') not in ('', 'nil'))
    mana = lambda st: _values(st['UnitMana("player")'])
    fighting = 'UnitAffectingCombat("player")'
    crit = (re.findall(r'([\d.]+)% chance to crit', tips.get('SetSpell(1, "spell")', '')) or ['0'])[0]
    dodge = (re.findall(r'([\d.]+)% chance to dodge', tips.get('SetSpell(2, "spell")', '')) or ['0'])[0]
    combat = ', '.join(f'{k} = {"true" if v[fighting] == "1" else "false"}' for k, v in states.items())
    cvars_now = ', '.join(f'[{q(c)}] = {_values(idle[f"GetCVar({q(c)})"])}' for c in cvars
                          if idle[f'GetCVar({q(c)})'] not in ('nil', ''))
    FIXTURE.parent.mkdir(parents=True, exist_ok=True)
    FIXTURE.write_text(HEADER + f'''-- Elunara (level 60 night elf druid, Stormrage set) and the combat tester, as the reference client saw them in
-- the recorded states: idle (nothing targeted), target (tester with Moonfire + Entangling Roots), self (self-targeted,
-- Rejuvenation on). __WBFixture(state) writes __WB the way the SKSE plugin will.
local book = {{ {", ".join(map(str, book))} }}

local function elunara(mana)
  return {{
    name = "Elunara", level = 60, sex = 3, class = "Druid", classToken = "DRUID", race = "Night Elf", raceToken = "NightElf",
    factionGroup = "Alliance", isPlayer = true, relicSlot = true, creatureType = {_values(idle['UnitCreatureType("player")'])}, health = 3213, healthMax = 3213, power = mana,
    powerMax = 5284, powerType = 0, xp = 0, xpMax = 0, mapX = {_values(idle['GetPlayerMapPosition("player")']).replace(', ', ', mapY = ')}, characterPoints = {t(idle, 'UnitCharacterPoints("player")')},
    dispels = {{ Curse = true, Poison = true }},
    baseMana = 1244,   -- CMaNGOS player_classlevelstats, level 60 druid: Barkskin's 15% is 186, Rebirth's 85% 1057
    critChance = {crit}, dodgeChance = {dodge},
{unit(idle, "player")}
  }}
end

local function tester()
  return {{
    name = "[PH[ Combat Tester", level = 60, sex = 2, class = "Warrior", classToken = "WARRIOR",
    health = {_values(target['UnitHealth("target")'])}, healthMax = 100,  -- the client only ever showed a percent
    power = 0, powerMax = 0, powerType = 1, reaction = 4, attackable = true, combat = true, distance = 0,
    creatureType = {_values(target['UnitCreatureType("target")'])},
{unit(target, "target")}
    debuffs = {{
      {{ spell = 9835, texture = "Interface\\\\Icons\\\\Spell_Nature_StarFall", count = 1, dispel = "Magic" }},
      {{ spell = 9853, texture = "Interface\\\\Icons\\\\Spell_Nature_StrangleVines", count = 1, dispel = "Magic" }},
    }},
  }}
end

function __WBFixture(state)
  local WB = __WB
  WB.units, WB.tokens = {{}}, {{ player = 1 }}
  WB.units[1] = elunara(({{ idle = {mana(idle)}, target = {mana(target)}, self = {mana(me)} }})[state])
  WB.spellbook, WB.spellbookDirty = book, true
  WB.itemNames = {{ {", ".join(f"[{i}] = {q(n)}" for i, n in sorted(item_names.items()))} }}
  WB.itemCounts = {{}}
  -- Stormrage Raiment 8/8: Rejuvenation lasts 3 sec longer, Regrowth casts 0.2 sec faster (the host's spell
  -- modifiers, from the gameplay service).
  WB.spellMods = {{}}
  for _, id in ipairs({{ 774, 1058, 1430, 2090, 2091, 3627, 8910, 9839, 9840, 9841, 25299 }}) do WB.spellMods[id] = {{ duration = 3000 }} end
  for _, id in ipairs({{ 8936, 8938, 8939, 8940, 8941, 9750, 9856, 9857, 9858 }}) do WB.spellMods[id] = {{ castTime = -200 }} end
  WB.actions = {{ [1] = {{ spell = 9912 }}, [2] = {{ spell = 9835 }}, [3] = {{ spell = 25299 }}, [4] = {{ spell = 25297 }},
                 [5] = {{ spell = 9853 }}, [6] = {{ spell = 17402 }} }}
  WB.equipment = {{
{equip}
  }}
  WB.bags = {{ [0] = {{ size = 16,
{bag}
  }} }}
  WB.world = {{ zone = "Teldrassil", subzone = "Shadowglen", money = 0, bindLocation = "Teldrassil",
               defaultLanguage = "Common" }}
  WB.cvars = {{ {cvars_now} }}
  __WBDefaultBindings()
  WB.units[1].combat = ({{ {combat} }})[state]
  if state == "target" then
    WB.units[2] = tester()
    WB.tokens.target = 2
    WB.units[1].combat = true
  elseif state == "self" then
    WB.tokens.target = 1
    WB.units[1].target = 1
    WB.units[1].combat = true
    WB.units[1].buffs = {{ {{ spell = 25299, texture = "Interface\\\\Icons\\\\Spell_Nature_Rejuvenation", count = 1, dispel = "Magic",
                           expires = GetTime() + 11.329 }} }}
  end
end
''')
    # The client's login, as it raised it (evidence/events/login_actions.lua): every event up to two seconds after
    # PLAYER_ENTERING_WORLD, with arguments; ADDON_LOADED was WBOracle's own.
    sys.path.insert(0, str(ROOT))
    login, pew = [], None
    events_text = (EVENTS / 'login_actions.lua').read_text(encoding='latin-1')
    events_text = events_text[events_text.index('WBOracleEvents = {'):]
    events_text = events_text[:events_text.index('\n}')]
    for line in re.findall(r'^\t\[\d+\] = "(.*)",$', events_text, re.M):
        rec = _unlua(line)
        fields = re.findall(r'"(?:\\.|[^"\\])*"|[^|]+', rec)
        t, name, args = float(fields[0]), fields[1], fields[2:]
        if name == 'PLAYER_ENTERING_WORLD' and pew is None:
            pew = t
        if pew is not None and t > pew + 2:
            break
        if name != 'ADDON_LOADED':
            def lua_arg(a: str) -> str:
                if a.startswith('"') or a == 'nil' or re.fullmatch(r'-?\d+(\.\d+)?(e[-+]?\d+)?', a):
                    return a
                return '(0/0)'   # the client's non-finite numbers (UPDATE_TICKET passes an uninitialised -1.#QNAN)
            login.append('{ ' + ', '.join([q(name)] + [lua_arg(a) for a in args]) + ' }')
    (LUA / 'wowapi_login.lua').write_text(HEADER + '-- The events the client raises at login, in order, with their arguments (recorded).\n'
                                          'local LOGIN = {\n  ' + ',\n  '.join(login) + '\n}\n'
                                          '-- A tutorial the account has read is not raised again (__WBTutorialFlagged: the server\'s flags).\n'
                                          'function __WBLogin()\n  for _, e in ipairs(LOGIN) do\n'
                                          '    if e[1] ~= "TUTORIAL_TRIGGER" or not __WBTutorialFlagged(e[2]) then __WBFire(unpack(e, 1, table.maxn(e))) end\n'
                                          '  end\nend\n')
    print(f'{len(rows)} bindings, {len(cvars)} cvars, {body.count(chr(10)) + 1} recorded functions, '
          f'{len(book)} spells; answers that differ by state (hand-write these): {len(varying)}')
    for v in varying:
        print('  ', v)


if __name__ == '__main__':
    if sys.argv[1:2] == ['gen']:
        gen()
    elif sys.argv[1:2] == ['events']:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        events()
    elif sys.argv[1:2] == ['lua']:
        lua()
    elif sys.argv[1:2] == ['collect']:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        collect(sys.argv[2:] or list(STATES))
    else:
        sys.exit(__doc__)
