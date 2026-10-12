#!/usr/bin/env python3
"""Numeric oracle: real WoW 1.12 client + CMaNGOS combat log vs. the WoWBridge service.

usage: oracle.py collect PLAN [PLAN ...]   cast in the running reference client (tools/addons/WBOracle records)
       oracle.py compare [--n N]           parse WBOracleDB, simulate the service, write build/oracle/report.json

Fixture (see STATUS.md): Elunara, L60 druid, full Stormrage + Staff of Jordan + Idol of Rejuvenation, talents reset,
vs. the rooted, pacified, neutral L60 creature 11080 with no resistances, at melee distance.
"""
import json, math, random, re, shutil, sys, time
from collections import defaultdict
from pathlib import Path
from wowref import saved_variables

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT)); sys.path.insert(0, str(ROOT / 'tools'))
from service import character, spells  # noqa: E402
from service.catalog import Catalog  # noqa: E402

OUT = ROOT / 'evidence/oracle'      # raw combat-log runs + report.json (tracked evidence)
GEAR = [16897, 16898, 16899, 16900, 16901, 16902, 16903, 16904, 873, 22398]   # == /wbo stats in the client
TARGET = '[PH[ Combat Tester'
HOSTILE = {'kind': 'unit', 'level': 60, 'reaction': 'hostile', 'alive': True, 'distance_yd': 2, 'los': True}


def _cast(name, wait, n, target=None):
    """n casts in chunks of 20, each preceded by a mana refill (.modify acts on the selection, so clear it first)."""
    steps = []
    for i in range(n):
        if i % 20 == 0:
            steps += ['slash:/script ClearTarget()', 'slash:.modify mana 99999',
                      f'slash:/wbo target {target}' if target else 'slash:/script TargetUnit("player")']
        steps += [f'slash:/cast {name}', f'wait:{wait}']
    return steps


# Typing a slash command takes ~0.09 s/char + 0.6 s, so waits are padded to the cast time / GCD.
PLANS = {
    'wrath': _cast('Wrath', 1.0, 60, TARGET),
    'moonfire': [*_cast('Moonfire', 0.3, 40, TARGET), *_cast('Moonfire', 13, 3, TARGET)],
    'moonfire_direct': _cast('Moonfire', 0.3, 120, TARGET),
    'roots': _cast('Entangling Roots', 29, 3, TARGET),
    # HoT ticks on a full-health target are skipped server-side (SpellAuras.cpp: GetHealth() == GetMaxHealth()),
    # so wound Elunara by more than the 5 x 292 the HoT restores before each cast.
    'rejuvenation': [s for _ in range(2) for s in ('slash:/script ClearTarget()', 'slash:.modify mana 99999',
                     'slash:/script TargetUnit("player")', 'slash:.damage 1700', 'slash:/cast Rejuvenation', 'wait:16')],
    'healing_touch': _cast('Healing Touch', 1.6, 30),
    # Ground-targeted channel: place the 8 yd circle on the ground beside Elunara (the tester stands at her feet).
    # Hurricane has a 60 s cooldown: one channel per run.
    'hurricane': [*_cast('Hurricane', 0, 1, TARGET)[:-1], 'click:600,470', 'wait:3', 'shot:hurricane', 'wait:9'],
}

# 1.12 enUS GlobalStrings for the self-cast combat log lines WBOracle records.
PATTERNS = [
    ('hit', re.compile(r'^Your (?P<spell>.+?) (?P<verb>hits|crits) (?P<tgt>.+?) for (?P<n>\d+) (?P<school>\w+) damage\.(?: \((?P<res>\d+) resisted\))?')),
    ('tick', re.compile(r'^(?P<tgt>.+?) suffers (?P<n>\d+) (?P<school>\w+) damage from your (?P<spell>.+?)\.(?: \((?P<res>\d+) resisted\))?')),
    ('heal', re.compile(r'^Your (?P<spell>.+?) (?P<verb>critically heals|heals) (?P<tgt>.+?) for (?P<n>\d+)\.')),
    ('hot', re.compile(r'^You gain (?P<n>\d+) health from (?P<spell>.+?)\.')),
    ('miss', re.compile(r'^Your (?P<spell>.+?) (?:was resisted by|missed) (?P<tgt>.+?)\.')),
]


def parse(paths: list[Path]) -> dict:
    """WBOracleDB lines 'time|EVENT|text' (pooled over runs) -> {spell: {'hit': [], 'crit': [], 'miss': [], 'tick': [], ...}}."""
    lines = [m for p in paths for m in re.findall(r'"((?:[^"\\]|\\.)*)"', p.read_text())]
    out = defaultdict(lambda: defaultdict(list))
    for line in lines:
        t, kind, text = (line.split('|', 2) + ['', ''])[:3]
        for name, rx in PATTERNS:
            m = rx.match(text)
            if not m:
                continue
            d = m.groupdict()
            bucket = out[d['spell']]
            if name == 'miss':
                bucket['miss'].append(1)
            elif name in ('hit', 'heal'):
                crit = d['verb'] in ('crits', 'critically heals')
                bucket['crit' if crit else name].append(int(d['n']) + int(d.get('res') or 0))
            else:
                bucket[name].append(int(d['n']) + int(d.get('res') or 0))
            break
    return {k: dict(v) for k, v in out.items()}


def simulate(cat, ch, spell_key: str, n: int) -> dict:
    sid = cat.slice[spell_key]
    s = cat.spell(sid)
    positive = spells.is_positive(s)
    target = {'kind': 'self'} if positive else HOSTILE
    sim = defaultdict(list)
    for seed in range(1, n + 1):
        r = spells.resolve(cat, ch, {'spell': sid, 'seed': seed, 'target': target, 'ctx': {}})
        for ev in r['events']:
            if ev['type'] in ('miss', 'resist'):
                sim['miss'].append(1)
            elif ev['type'] in ('damage', 'heal'):
                key = 'crit' if ev['crit'] else ('hit' if ev['type'] == 'damage' else 'heal')
                sim[key].append(ev['amount'] + ev.get('resisted', 0))
        for a in r['auras']:
            for tk in a.get('ticks', []):
                sim['hot' if a['type'] == 'heal' else 'tick'].append(tk['amount'] + tk.get('resisted', 0))
        if any('area' in a for a in r['auras']):        # persistent area aura: ticks come from tick_area
            t = spells.tick_area(cat, ch, {'spell': sid, 'seed': seed, 'targets': [{'id': 1, 'level': 60}]})
            sim['tick'] += [x['amount'] + x['resisted'] for x in t['results'] if x['type'] == 'damage']
        sim['casts'].append(1)
    return {'name': s['SpellName'], **sim}


def ks_p(a: list, b: list) -> tuple[float, float]:
    """Two-sample Kolmogorov-Smirnov D and asymptotic p (conservative for integer-valued amounts)."""
    a, b = sorted(a), sorted(b)
    i = j = 0
    d = 0.0
    while i < len(a) and j < len(b):
        x = min(a[i], b[j])
        while i < len(a) and a[i] == x:
            i += 1
        while j < len(b) and b[j] == x:
            j += 1
        d = max(d, abs(i / len(a) - j / len(b)))
    if d == 0:
        return 0.0, 1.0
    ne = len(a) * len(b) / (len(a) + len(b))
    lam = (math.sqrt(ne) + 0.12 + 0.11 / math.sqrt(ne)) * d
    p = 2 * sum((-1) ** (k - 1) * math.exp(-2 * k * k * lam * lam) for k in range(1, 101))
    return d, max(0.0, min(1.0, p))


def binom_p(k: int, n: int, p: float) -> float:
    """Exact two-sided binomial test p-value."""
    pmf = [math.comb(n, i) * p ** i * (1 - p) ** (n - i) for i in range(n + 1)]
    return min(1.0, sum(x for x in pmf if x <= pmf[k] * (1 + 1e-9)))


def compare(n_sim: int) -> dict:
    cat = Catalog()
    # The reference server is CMaNGOS, so the port is validated with WoWBridge's labelled custom rules off; their
    # effect is reported per spell as 'custom_rule_delta' (service with rules on minus off).
    ch = character.build(cat, GEAR)
    ch.custom_rules = False
    ch_custom = character.build(cat, GEAR)
    runs = sorted(OUT.glob('*.lua'))
    obs_all = parse(runs)
    report = {'runs': [r.name for r in runs], 'fixture': {'gear': GEAR, 'int': ch.stats['int'], 'spi': ch.stats['spi'], 'mana': ch.max_mana},
              'spells': {}, 'pass': True}
    for key in cat.slice:
        sim = simulate(cat, ch, key, n_sim)
        obs = obs_all.get(sim['name'])
        if not obs:
            continue
        res, ok = {}, True
        for kind in ('hit', 'crit', 'tick', 'heal', 'hot'):
            o, s = obs.get(kind, []), sim.get(kind, [])
            if not o:
                continue
            row = {'n': len(o), 'obs_min': min(o), 'obs_max': max(o), 'obs_mean': round(sum(o) / len(o), 1)}
            if not s:
                row['error'] = 'service never produces this outcome'
                ok = False
            else:
                lo, hi = min(s), max(s)
                outside = [x for x in o if not lo <= x <= hi]
                d, p = ks_p(o, s)
                row.update(sim_min=lo, sim_max=hi, sim_mean=round(sum(s) / len(s), 1), outside=outside, ks_d=round(d, 3), ks_p=round(p, 4))
                ok &= not outside and p >= 0.01
            res[kind] = row
        # Rates per landed direct effect (casts without a direct effect, e.g. pure HoTs, are skipped).
        direct = 'hit' if obs.get('hit') or sim.get('hit') else 'heal'
        n_obs = len(obs.get(direct, [])) + len(obs.get('crit', []))
        n_sim_direct = len(sim.get(direct, [])) + len(sim.get('crit', []))
        if n_obs and n_sim_direct:
            p_crit = len(sim.get('crit', [])) / n_sim_direct
            k = len(obs.get('crit', []))
            res['crit_rate'] = {'obs': f'{k}/{n_obs}', 'sim': round(p_crit, 4), 'p': round(binom_p(k, n_obs, p_crit), 4)}
            ok &= res['crit_rate']['p'] >= 0.01
        if direct == 'hit' and n_obs:
            casts = n_obs + len(obs.get('miss', []))
            p_miss = len(sim.get('miss', [])) / len(sim['casts'])
            k = len(obs.get('miss', []))
            res['miss_rate'] = {'obs': f'{k}/{casts}', 'sim': round(p_miss, 4), 'p': round(binom_p(k, casts, p_miss), 4)}
            ok &= res['miss_rate']['p'] >= 0.01
        custom, base = simulate(cat, ch_custom, key, 200), simulate(cat, ch, key, 200)   # same seeds
        delta = {k: round(sum(custom[k]) / len(custom[k]) - sum(base[k]) / len(base[k]), 1)
                 for k in ('hit', 'heal', 'tick', 'hot') if custom.get(k) and base.get(k)}
        if any(delta.values()):
            res['custom_rule_delta'] = delta
        res['pass'] = bool(ok)
        report['spells'][sim['name']] = res
        report['pass'] &= bool(ok)
    return report


def main(argv: list[str]) -> None:
    if not argv:
        sys.exit(__doc__)
    OUT.mkdir(parents=True, exist_ok=True)
    if argv[0] == 'collect':
        import wowref
        steps = ['slash:/wbo clear']
        for plan in argv[1:]:
            steps += [f'slash:/wbo mark {plan}', *PLANS[plan]]
        wowref.main(steps + ['wait:3', 'slash:/run ReloadUI()', 'wait:8'])
        shutil.copy(saved_variables(), OUT / f'WBOracle_{time.strftime("%Y%m%d_%H%M%S")}_{"+".join(argv[1:])}.lua')
    elif argv[0] == 'compare':
        n = int(argv[argv.index('--n') + 1]) if '--n' in argv else 20000
        report = compare(n)
        (OUT / 'report.json').write_text(json.dumps(report, indent=1))
        for name, res in report['spells'].items():
            print(f"{'PASS' if res['pass'] else 'FAIL'} {name}")
            for k, v in res.items():
                if k != 'pass':
                    print(f'   {k:10} {v}')
        print('ORACLE', 'PASS' if report['pass'] else 'FAIL')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
