"""WoW gear hit effects; no additional mana cost or player spell knowledge gate.

Weapon procs use item_template spellppmRate, or the DBC chance; ProcChance=101
is a sentinel resolved to a labelled 1.8 PPM Skyrim fallback. Incoming gear keeps its DBC
chance. Area/chain effects apply to the struck actor; the bridge has one hit target.
"""
import random
from service import formulas as F, spells, talent_effects

WEAPON_PPM = 1.8


def _trigger(cat, ch, req, sid, source=None):
    s = cat.spell(sid)
    friendly = spells.is_positive(s)
    target = {'kind': 'self'} if friendly else {**req['target'], 'kind': 'unit'}
    out = spells.resolve(cat, ch, {**req, 'spell': sid, 'target': target},
                         feral_dispatch=False, triggered=True)
    from service.skyrim import spell_skill
    out.update(cost=0, cast_ms=0, gcd_ms=0, skill=spell_skill(s), source_spell=source or sid,
               target='caster' if friendly else 'target',
               tooltip=talent_effects.tooltip(cat, ch, s))
    # Thunderfury's code-linked cyclone. Its Nature resistance aura already
    # resolves from the item spell; attack speed has a native equivalent.
    if sid == 21992 and any(e['type'] == 'damage' for e in out['events']):
        linked = cat.spell(27648)
        out['auras'].append({'type': 'attack_speed', 'amount': abs(F.effect_value(linked, 1, ch.level, None)),
                             'duration_ms': cat.duration(linked, ch.level)})
    # Sanctuary's defense skill is represented as flat armor in Skyrim.
    for aura in out['auras']:
        if aura.get('aura') == 30:
            aura.update(type='armor', amount=abs(F.effect_value(s, aura['eff'], ch.level, None)) * 8)
    return out


def melee_procs(cat, ch, req, out):
    if ch.form in ('cat', 'bear', 'dire_bear') or out.get('damage', 0) <= 0 or out.get('outcome') not in ('hit', 'crit', 'glance', 'block'):
        return out
    item = cat.item(ch.weapon_entry)
    if not item:
        return out
    rng = random.Random(f'{req.get("seed", 0)}:equipment')
    for n in range(1, 6):
        sid = item[f'spellid_{n}']
        if not sid or item[f'spelltrigger_{n}'] != 2:
            continue
        ppm = item.get(f'spellppmRate_{n}', 0)
        dbc_chance = cat.spell(sid)['ProcChance']
        chance = (ppm * ch.weapon['delay'] / 600 if ppm > 0 else
                  dbc_chance if dbc_chance <= 100 else WEAPON_PPM * ch.weapon['delay'] / 600)
        if F.roll_chance(rng, min(100, chance)):
            out.setdefault('procs', []).append(_trigger(cat, ch, req, sid))
    return out


def incoming(cat, ch, req, out):
    if not req.get('weapon_hit'):
        return out
    rng = random.Random(f'{req.get("seed", 0)}:equipment_incoming')
    for a in ch.auras:
        if a['aura'] == 15:  # Sulfuras: always retaliate on a weapon hit
            out.setdefault('procs', []).append({'spell': a['spell'], 'target': 'target', 'cost': 0,
                'events': [{'type': 'damage', 'amount': a['amount'], 'school': cat.spell(a['spell'])['School']}], 'auras': []})
        elif a['aura'] == 42 and a['spell'] in (18815, 18816):
            s = cat.spell(a['spell'])
            if F.roll_chance(rng, s['ProcChance']):
                sid = s[f'EffectTriggerSpell{a["eff"]}']
                proc = _trigger(cat, ch, req, sid, s['Id'])
                # Skullflame's drain uses EffectHealthLeech (9), outside the
                # class spell resolver. Preserve both halves of the drain.
                if sid == 18817 and not any(e['type'] in ('miss', 'resist') for e in proc['events']):
                    amount = abs(int(F.effect_value(cat.spell(sid), 1, ch.level, None)))
                    proc['events'] = [{'type': 'damage', 'amount': amount, 'school': 5},
                                      {'type': 'heal', 'amount': amount, 'target': 'caster'}]
                out.setdefault('procs', []).append(proc)
    return out
