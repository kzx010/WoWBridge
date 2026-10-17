"""WoW gear hit effects; no additional mana cost or player spell knowledge gate.

Weapon procs use item_template spellppmRate, or the DBC chance; ProcChance=101
is a sentinel resolved to a labelled 1.8 PPM Skyrim fallback. Incoming gear keeps its DBC
chance. Area/chain effects apply to the struck actor; the bridge has one hit target.
"""
import random
from service import formulas as F, spells, talent_effects

WEAPON_PPM = 1.8
CATEGORY_KEY = 0x40000000   # separate from real spell IDs; plugin co-save and Lua cooldown table
ITEM_KEY = 0x20000000       # an item's own spell timer, by item entry (WorldObject::IsSpellReady)
ATTR_COOLDOWN_ON_EVENT = 0x02000000

# Only complete effects enter the native path. Scripted/charged procs must not
# turn into permanent flat buffs or lose one half of a compound use spell.
USE_AURAS = {3: 'periodic damage', 8: 'periodic heal', 12: 'stun', 13: 'spell power',
             15: 'damage shield', 22: 'armor/resistance', 24: 'periodic power restore',
             29: 'stat', 31: 'movement speed', 52: 'melee crit', 54: 'hit', 55: 'spell hit', 57: 'spell crit',
             65: 'spell haste', 69: 'absorb shield', 71: 'school spell crit', 99: 'attack power',
             124: 'ranged attack power', 135: 'healing power', 138: 'melee haste',
             140: 'ranged haste', 143: 'resistance', 158: 'block value'}
USE_EFFECTS = {2: 'direct damage', 10: 'direct heal', 30: 'power restore', 38: 'dispel'}


def use_spells(item):
    return [(n, item[f'spellid_{n}']) for n in range(1, 6)
            if item.get(f'spellid_{n}') and item.get(f'spelltrigger_{n}') == 0]


def use_support(cat, sid):
    """Audit every effect, including periodic dispel payloads, before any use."""
    s = cat.spell(sid)
    reasons = []
    if s['ProcCharges']:
        reasons.append('charged buff')
    for e in spells._effects(s):
        effect, aura = s[f'Effect{e}'], s[f'EffectApplyAuraName{e}']
        if s[f'EffectImplicitTargetA{e}'] not in (1, 6, 21, 37) or s[f'EffectImplicitTargetB{e}']:
            reasons.append('pet/world/area target')
        if effect in (6, 35):
            if aura == 23:
                linked = cat.spell(s[f'EffectTriggerSpell{e}'])
                if not all(linked[f'Effect{i}'] == 38 for i in spells._effects(linked)):
                    reasons.append('periodic scripted spell')
            elif aura not in USE_AURAS:
                reasons.append(f'unsupported aura {aura}')
            if aura in (24,) and s[f'EffectMiscValue{e}'] not in (0, 1, 3):
                reasons.append('unsupported power')
        elif effect not in USE_EFFECTS:
            reasons.append(f'unsupported effect {effect}')
        elif effect == 30 and s[f'EffectMiscValue{e}'] not in (0, 1, 3):
            reasons.append('unsupported power')
    return sorted(set(reasons))


def item_use_support(cat, item):
    reasons = [reason for _, sid in use_spells(item) for reason in use_support(cat, sid)]
    # Native enchantment charges are not item_template's independent charges.
    # Refuse these items as a whole instead of granting unlimited uses.
    if any(item.get(f'spellcharges_{n}', 0) for n, _ in use_spells(item)):
        reasons.append('limited item charges')
    return sorted(set(reasons))


def use_cooldown(item, n, s):
    """Player::AddCooldown @8ec338a starts the timers: zero overrides, -1 inherits, category zero inherits.

    WorldObject::IsSpellReady checks an item use against the item's own category only (zero: none), and the
    spell timer an item starts blocks only that item (CooldownData's item ID): Barbaric Belt and the Hauberks
    share Rage 9174 but only their 15 s category. SPELL_ATTR_COOLDOWN_ON_EVENT holds both timers until the
    aura fades (Spell.cpp AddCooldown(permanent); SpellAuraHolder removal starts them).
    """
    own, category, shared = (item.get(f'{field}_{n}', default) for field, default in
                             (('spellcooldown', -1), ('spellcategory', 0), ('spellcategorycooldown', -1)))
    return {'spell': s['Id'], 'item': item['entry'], 'cooldown_ms': own if own >= 0 else s['RecoveryTime'],
            'category': category or s['Category'], 'ready_category': category,
            'category_cooldown_ms': shared if shared >= 0 else s['CategoryRecoveryTime'],
            'gcd_ms': s['StartRecoveryTime'], **({'on_event': True} if s['Attributes'] & ATTR_COOLDOWN_ON_EVENT else {})}


def use_tooltip(cat, ch, s):
    profile = talent_effects.tooltip(cat, ch, s)
    # Health conversion is the normal spell-skill conversion; resource tokens
    # convert in FrameXML's formatter. Stats/AP keep WoW units for the formulas
    # and carry the same native equivalents as equip bonuses.
    from service.skyrim_effects import aura_effects
    lines = []
    labels = {0x49504: 'maximum magicka', 0x493AA: 'maximum health', 0x49507: 'maximum stamina',
              0x7A0F4: 'carry weight', 0x7A0FF: 'One-Handed damage %', 0x7A106: 'Two-Handed damage %',
              0x7A0FD: 'magicka regeneration %', 0xE5F55: 'armor', 0x10319D: 'armor reduction',
              0x48C8B: 'Fire resistance %', 0x48F45: 'Frost resistance %',
              0x49295: 'Shock resistance %', 0xB7A35: 'Magic resistance %'}
    if ch.native_mode:
        for e in spells._effects(s):
            aura, misc = s[f'EffectApplyAuraName{e}'], s[f'EffectMiscValue{e}']
            amount = profile['effects'][e - 1][1]
            if F.effect_value(s, e, ch.level, None, maximum=True) < 0:
                amount = -amount
            native = None
            if aura == 29:
                native = {'type': 'stat', 'stat': misc, 'amount': amount}
            elif aura in (22, 143):
                native = {'type': 'armor' if misc == 1 else 'resistance', 'school_mask': misc, 'amount': amount}
            if native:
                lines += [f"Skyrim: {effect['magnitude']:g} {labels[effect['mgef']]}"
                          for effect in aura_effects(native)]
            elif aura == 99:
                lines.append(f'Skyrim: {amount * .1:g}% melee damage')
            elif aura == 124:
                lines.append(f'Skyrim: {amount * .1:g}% Archery damage')
    profile['cast_ms'] = int(profile['cast_ms'] / ch.cast_speed())
    return {**profile, 'item_use': True, 'native_lines': lines}


def use(cat, ch, req):
    item = cat.item(req['item'])
    uses = use_spells(item or {})
    if not uses or item_use_support(cat, item):
        raise spells.CastError('SPELL_FAILED_SPELL_UNAVAILABLE')
    # WorldSession::HandleUseItemOpcode requires equippable items to be worn.
    if item['InventoryType'] and item['entry'] not in ch.items:
        raise spells.CastError('SPELL_FAILED_ITEM_NOT_READY')
    cooldowns = [use_cooldown(item, n, cat.spell(sid)) for n, sid in uses]
    active = req.get('caster', {}).get('proc_cooldowns', {})
    # The 1.12 client refuses an item whose cooldown runs with ERR_ITEM_COOLDOWN before it asks the server.
    if active.get(str(ITEM_KEY | item['entry']), 0) > 0 or any(
            cd['ready_category'] and active.get(str(CATEGORY_KEY | cd['ready_category']), 0) > 0 for cd in cooldowns):
        raise spells.CastError('ERR_ITEM_COOLDOWN')
    outcomes = []
    for _, sid in uses:
        s = cat.spell(sid)
        if s['StartRecoveryCategory'] == 133 and req.get('caster', {}).get('gcd_ms', 0) > 0:
            raise spells.CastError('ERR_ITEM_COOLDOWN')
        self_target = all(s[f'EffectImplicitTargetA{e}'] == 1 for e in spells._effects(s))
        target = ({'kind': 'self', 'level': ch.level, **({'actor_values': ch.actor_values} if ch.actor_values else {})}
                  if self_target else req.get('target', {'kind': 'self', 'level': ch.level}))
        spells._validate(cat, s, ch, {**req, 'target': target})
        out = spells.resolve(cat, ch, {**req, 'spell': sid, 'target': target},
                             feral_dispatch=False, triggered=True)
        out.update(cost=0, cooldown_ms=0, tooltip=use_tooltip(cat, ch, s),
                   item_use=True, item_gcd_category=s['StartRecoveryCategory'],
                   target='caster' if target['kind'] == 'self' else 'target')
        if s['AttributesEx'] & 0x44 and self_target:
            out['self_channel'] = True
        out.pop('reagents', None)   # these item effects are paid by the item
        outcomes.append(out)
    return {'item': item['entry'], 'requires_equipped': bool(item['InventoryType']),
            'uses': outcomes, 'item_cooldowns': cooldowns}


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
    equipped = {item[f'spellid_{n}'] for entry in ch.items for item in [cat.item(entry)]
                for n in range(1, 6) if item[f'spelltrigger_{n}'] == 1 and item[f'spellid_{n}']}
    for group in cat.raw['item_sets'].values():
        count = sum(1 for entry in group['ItemID'] if entry and entry in ch.items)
        equipped.update(sid for sid, threshold in zip(group['SetSpellID'], group['SetThreshold'])
                        if sid and threshold and count >= threshold)
    overrides = {r['entry']: r for r in cat.raw['spell_proc_event']}
    for a in ch.auras:
        if a['aura'] == 15 and a['spell'] in equipped:  # equipped retaliation; active shields run in Cast::Struck
            out.setdefault('procs', []).append({'spell': a['spell'], 'target': 'target', 'cost': 0,
                'events': [spells.native_damage_event(ch, cat.spell(a['spell']), req['target'], a['amount'], rng)], 'auras': []})
        elif a['aura'] == 42 and a['spell'] in equipped:
            s = cat.spell(a['spell'])
            row = overrides.get(s['Id'], overrides.get(cat.chain(s['Id']), {}))
            sid = s[f'EffectTriggerSpell{a["eff"]}']
            if not sid or not (row.get('procFlags') or s['ProcFlags']) & 0x28:  # incoming melee/ranged weapon hit
                continue
            if F.roll_chance(rng, row.get('CustomChance') or s['ProcChance']):
                proc = _trigger(cat, ch, req, sid, s['Id'])
                # Skullflame's drain uses EffectHealthLeech (9), outside the
                # class spell resolver. Preserve both halves of the drain.
                if sid == 18817 and not any(e['type'] in ('miss', 'resist', 'absorb') for e in proc['events']):
                    amount = abs(int(F.effect_value(cat.spell(sid), 1, ch.level, None)))
                    damage = spells.native_damage_event(ch, cat.spell(sid), req['target'], amount, rng, absorb=False)
                    proc['events'] = [damage, {'type': 'heal', 'amount': damage['amount'], 'target': 'caster'}]
                out.setdefault('procs', []).append(proc)
    return out
