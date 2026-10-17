"""wowbridge service: newline-delimited JSON over loopback TCP (protocol v1). One request -> one response.

ops: hello | stats | resolve | tick_area | damage_taken | melee | rage_from_damage_taken | known_spells | talents |
party_aura | party_hit | judged_hit (the Paladin's aura membership, her blessings on a follower's hit, and her
judgements' payloads for a follower striking a judged unit).
With --trace, each request/response is appended to a JSONL log.
usage: python -m service.server [--port 7878] [--catalog build/catalog.json] [--trace build/service_trace.jsonl]
"""
import argparse, asyncio, json, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from service import character, feral, formulas as F, melee, native_values, paladin, paladin_auras, paladin_defense, paladin_talents, racials, skyrim, spells, talent_effects  # noqa: E402
from service.catalog import Catalog, DEFAULT  # noqa: E402
from service.progression import Progression, DEFAULT as DEFAULT_PROGRESSION  # noqa: E402
from service.talents import book as talent_book  # noqa: E402

PROTOCOL = 1
MAX_LINE = 64 * 1024
MAX_TARGETS = 64


class Service:
    def __init__(self, catalog: Catalog, trace: Path | None, progression: Progression | None = None):
        self.cat = catalog
        self.trace = open(trace, 'a') if trace else None
        self._chars: dict[tuple, character.Character] = {}
        self._progression = progression
        self._perks = None

    def perks(self):
        if self._perks is None:
            from service.perks import Perks
            self._perks = Perks(self.cat, self._progression)
        return self._perks

    def convert(self, out, caster, skill):
        out = skyrim.convert(out, caster, skill, self.cat.balance)
        return self.perks().finish(caster, out) if 'perks' in caster else out

    def char(self, caster: dict) -> character.Character:
        """Every op's character: caster.class (a ChrClasses ID or name; a Druid when absent, as older plugins send
        none) and caster.race (the class's default race when absent) choose the base rows, formulas and racials."""
        feral.state(caster)
        ranks = ()
        cls = character.class_id(caster.get('class', self.cat.raw['character'].get('class', 11)))
        if 'perks' in caster:
            _, definition = self.perks().progression.class_definition(caster.get('class', cls))
            cls = definition['id']
        if 'talents' in caster:
            _, definition = talent_book().class_definition(caster.get('class', cls))
            cls = definition['id']
            ranks = tuple(sorted(talent_book().allocation(definition, caster['talents'], caster.get('level', 60)).items()))
        key = (int(caster.get('level', 60)), tuple(sorted(int(i) for i in caster.get('items', []))),
               tuple(sorted(set(int(i) for i in caster.get('auras', [])))), feral.form_name(caster),
               int(caster['race']) if 'race' in caster else character.default_race(self.cat.raw['character'], cls),
               cls, ranks,
               bool(caster.get('indoors', False)), 'talents' in caster,
               json.dumps(caster.get('equipment_modifiers', {}), sort_keys=True, allow_nan=False),
               json.dumps(caster['skyrim'], sort_keys=True, allow_nan=False) if 'skyrim' in caster else None,
               json.dumps(caster['actor_values'], sort_keys=True, allow_nan=False) if 'actor_values' in caster else None)
        if key not in self._chars:
            self._chars[key] = character.build(self.cat, list(key[1]), level=key[0], auras=key[2], form=key[3], race=key[4],
                                               cls=cls, talents=dict(ranks) if key[8] else None, indoors=key[7], equipment_modifiers=json.loads(key[9]),
                                               skyrim=json.loads(key[10]) if key[10] is not None else False,
                                               actor_values=json.loads(key[11]) if key[11] is not None else None)
        ch = self._chars[key]
        return self.perks().augment(ch, caster) if 'perks' in caster else ch

    def weapon_tooltips(self, caster: dict, weapons: dict) -> dict:
        """Each WoW weapon she carries as her swing would deal it in her hand now (melee.weapon_tooltip): her skills,
        gear and perks, untempered (each copy's own tempering scales its tooltip). weapons: entry -> native damage."""
        if 'skyrim' not in caster or not isinstance(weapons, dict) or len(weapons) > 256:
            return {}
        modifiers = caster.get('equipment_modifiers', {})
        held = int(modifiers.get('weapon_entry', 0) or 0)
        worn = [int(i) for i in caster.get('items', []) if int(i) != held]
        out = {}
        for entry, damage in weapons.items():
            item = self.cat.item(int(entry))
            if not item or item['class'] != 2:
                continue
            skill = 'TwoHanded' if item['InventoryType'] == 17 else 'Marksman' if item['InventoryType'] in (15, 25, 26) else 'OneHanded'
            ch = self.char({**caster, 'items': worn + [int(entry)], 'equipment_modifiers': {
                **modifiers, 'weapon_entry': int(entry), 'weapon_skill': skill, 'temper_multiplier': 1, 'weapon_damage': damage}})
            if tooltip := melee.weapon_tooltip(ch):
                out[str(int(entry))] = tooltip
        return out

    def spell_profiles(self, ch, sids) -> dict:
        """The interface's spell tooltip profiles: resolve's numbers (talent_effects.tooltip), her Skyrim skill for the
        spell and its cast time at her casting speed. The book's answer and the costs answer give the same ones."""
        out = {}
        for sid in sorted(set(sids)):
            spell = self.cat.spell(sid)
            profile = {**talent_effects.tooltip(self.cat, ch, spell), 'skill': skyrim.cast_skill(spell, self.cat.balance, ch)}
            profile['cast_ms'] = int(profile['cast_ms'] / ch.cast_speed())
            out[str(sid)] = profile
        return out

    def handle(self, req: dict) -> dict:
        op = req.get('op')
        if 'talents' in req.get('caster', {}) and 'indoors' in req.get('ctx', {}):
            req = {**req, 'caster': {**req['caster'], 'indoors': req['ctx']['indoors']}}
        if op == 'talents':
            return talent_book().handle(req)
        if op == 'known_spells':
            if self._progression is None:
                self._progression = Progression()
            out = self._progression.known_spells(req)
            if 'talents' in req.get('caster', {}):
                # Every spell the book shows, the ranks still to learn too (their tooltips show the same numbers).
                ch = self.char({**req['caster'], 'class': out['class_id']})
                out['spell_tooltips'] = self.spell_profiles(ch, list(out['known_spells']) +
                                                            [row['spell'] for row in out.get('unlearned_spells', [])])
            return out
        if op == 'hello':
            return {'protocol': PROTOCOL, 'service': 'wowbridge', 'catalog': self.cat.raw['provenance'],
                    'slice': self.cat.slice}
        if op == 'stats':
            ch = self.char(req.get('caster', {}))
            # A Paladin's talent-set sheet values (paladin_talents.profile): defense skill, speeds, mechanic resists.
            extra = {'paladin': paladin_talents.profile(self.cat, ch)} if ch.cls == F.CLASS_PALADIN else {}
            return {**extra, 'max_mana': ch.max_mana, 'native_health_bonus': ch.native_health_bonus, 'stats': ch.stats, 'regen': ch.regen(),
                    'attack_rating': ch.weapon_skill_value(),  # the weapon skill used by melee.attack_table
                    'spell_crit': {str(s): ch.spell_crit(s) for s in range(7)}, 'spell_hit': ch.spell_hit(),
                    'spell_damage': ch.spell_damage(0x7E), 'healing': ch.healing_done(0x7E), 'items': ch.items,
                    'form': feral.profile(self.cat, ch), 'resources': dict(feral.RESOURCES),
                    'resist': {str(s): round(ch.resistance(s)) for s in range(1, 7)},   # the pane shows whole points
                    'dodge_bonus': ch.total(49), 'melee_crit': ch.melee_crit(),
                    'racials': racials.passives(self.cat, ch, 'skyrim' in req.get('caster', {})),
                    # The class-aware character sheet (StatSystem.cpp), WoW units.
                    'class': ch.cls, 'race': ch.race, 'max_health': ch.max_health,
                    'attack_power': ch.attack_power(), 'ranged_attack_power': ch.ranged_attack_power(),
                    'defense': {'dodge': ch.dodge(), 'parry': ch.parry(), 'block': ch.block_chance(),
                                'block_value': ch.block_value(), 'can_parry': ch.can_parry(),
                                'can_block': ch.can_block(), 'shield': ch.shield_entry},
                    'health_regen': ch.health_regen(), 'racial_spells': racials.actives(self.cat, ch)}
        if op == 'native_defenses':
            ch = self.char(req.get('caster', {}))
            target = req['target']

            def report(data, resist, level, creature, armor):
                return {'actor_values': data,
                        'external': {str(av): native_values.external(data, av) for av in native_values.VALUES},
                        'resist': {str(s): resist(s) for s in range(1, 7)},
                        'resist_percent': {str(s): F.effective_resist_percent(ch.level, level, resist(s), False, creature)
                                           for s in range(1, 7)},
                        'binary_resist_percent': {str(s): F.effective_resist_percent(ch.level, level, resist(s), True, creature)
                                                  for s in range(1, 7)},
                        'disease_resist_percent': native_values.resist_percent(data, 3, 3),
                        'absorb_chance': native_values.absorb_chance(data), 'armor': armor,
                        'regen_multipliers': native_values.regen_multipliers(data)}

            own = ch.actor_values or native_values.snapshot({})
            return {'player': report(own, ch.resistance, ch.level, False, native_values.external(own, 39) * 8),
                    'target': report(native_values.snapshot(target['actor_values']),
                                     lambda s: native_values.target_resistance(target, s, ch.level), target['level'],
                                     not target.get('is_player', False), melee.target_armor(self.cat, target)),
                    'regen': ch.regen()}
        if op == 'resolve':
            if len(req.get('targets', [])) > MAX_TARGETS:
                raise ValueError('too many targets')
            if req['spell'] not in self.cat.spells:
                raise spells.CastError('SPELL_FAILED_UNKNOWN')
            ch = self.char(req.get('caster', {}))
            out = paladin_talents.finish(self.cat, ch, req, talent_effects.finish(self.cat, ch, req, spells.resolve(self.cat, ch, req)))
            paladin.spell_procs(self.cat, ch, req, out)   # her judged targets' Light / Wisdom on her damaging hits
            paladin_defense.threat(ch, self.cat.spell(req['spell']), out)
            if 'talents' in req.get('caster', {}):
                out['tooltip'] = talent_effects.tooltip(self.cat, ch, self.cat.spell(req['spell']))
            return self.convert(out, req.get('caster', {}), skyrim.cast_skill(self.cat.spell(req['spell']), self.cat.balance, ch))
        if op == 'item_use':
            from service import equipment
            caster = req.get('caster', {})
            out = equipment.use(self.cat, self.char(caster), req)
            out['uses'] = [self.convert(part, caster, skyrim.spell_skill(self.cat.spell(part['spell']), self.cat.balance))
                           for part in out['uses']]
            return out
        if op == 'incoming_hit':
            caster = req.get('caster', {})
            from service import equipment
            ch = self.char(caster)
            out = equipment.incoming(self.cat, ch, req, talent_effects.incoming(self.cat, ch, req))
            return self.convert(paladin_defense.incoming(self.cat, ch, req, out), caster, 'Alteration')
        if op == 'party_aura':   # which followers her active aura reaches (paladin_auras.party_aura)
            ch = self.char(req.get('caster', {}))
            out = paladin_auras.party_aura(self.cat, ch, req)
            return self.convert(out, req.get('caster', {}), skyrim.cast_skill(self.cat.spell(out['spell']), self.cat.balance, ch))
        if op == 'judged_hit':   # a follower's hit on her judged target: its Light / Wisdom payload (paladin.judged_hit)
            attacker = self.char({'class': F.CLASS_PALADIN, 'race': 1, 'level': req.get('attacker', {}).get('level', 1)})
            return self.convert(paladin.judged_hit(self.cat, attacker, req), req.get('caster', {}), 'Restoration')
        if op == 'party_hit':    # a follower's hit under her Sanctuary / Sacrifice (paladin_auras.party_hit)
            ch = self.char(req.get('caster', {}))
            return self.convert(paladin_auras.party_hit(self.cat, ch, req), req.get('caster', {}), 'Alteration')
        if op == 'tick_area':
            if len(req.get('targets', [])) > MAX_TARGETS:
                raise ValueError('too many targets')
            ch = self.char(req.get('caster', {}))
            out = paladin_defense.threat(ch, self.cat.spell(req['spell']), spells.tick_area(self.cat, ch, req))
            return self.convert(out, req.get('caster', {}), skyrim.spell_skill(self.cat.spell(req['spell']), self.cat.balance))
        if op == 'damage_taken':
            if 'school' in req:
                ch = self.char(req.get('caster', {}))
                defence = paladin_defense.immune_damage(ch, req['school'], racials.resist_damage(ch, req))
                return skyrim.convert({**spells.damage_taken({**req, 'damage': defence['damage']}), **defence},
                                      req.get('caster', {}), 'Alteration')
            return skyrim.convert(spells.damage_taken(req), req.get('caster', {}), 'Alteration')
        if op == 'skyrim_costs':   # her spells' magicka costs as resolve would charge them (the interface's numbers)
            caster = req.get('caster', {})
            ch = self.char(caster)
            costs = {}
            for sid in self.cat.class_spells:
                spell = self.cat.spell(sid)
                wow = spells._cost(spell, ch)
                power = {0: 'mana', 1: 'rage', 3: 'energy'}.get(spell['PowerType'], 'mana')
                if power == 'rage':
                    wow /= 10
                if (feral.FORM_SPELLS.get(sid) or feral.TALENT_FORM_SPELLS.get(sid)) == ch.form:
                    wow = 0
                out = skyrim.convert({'spell': sid, 'cost': wow, 'power': power, 'events': [], 'auras': []}, caster,
                                     skyrim.cast_skill(spell, self.cat.balance, ch), self.cat.balance)
                costs[str(sid)] = out['cost']
            profiles = {}
            if any(a.get('item_use') for a in ch.auras):
                profiles = {str(sid): talent_effects.tooltip(self.cat, ch, self.cat.spell(sid))
                            for sid in self.cat.class_spells}
            if 'talents' in caster:
                if self._progression is None:
                    self._progression = Progression()
                _, definition = self._progression.class_definition(ch.cls)
                _, talents = talent_book().class_definition(ch.cls)
                spell_ids = {row['spell'] for row in definition['spells'] + definition.get('excluded', []) +
                             talents.get('granted_spellbook', [])}
                profiles = self.spell_profiles(ch, spell_ids)
            else:
                for profile in profiles.values():
                    profile['cast_ms'] = int(profile['cast_ms'] / ch.cast_speed())
            from service import equipment
            profiles.update({str(sid): equipment.use_tooltip(self.cat, ch, self.cat.spell(sid))
                             for sid in self.cat.item_use_spells if not equipment.use_support(self.cat, sid)})
            return {'costs': costs, 'spell_tooltips': profiles, 'weapon_tooltips': self.weapon_tooltips(caster, req.get('weapons', {}))}
        if op == 'rage_from_damage_taken':
            return {'rage': F.reward_rage(int(req['damage']), int(req['victim_level']), False)}
        if op == 'melee':
            ch = self.char(req.get('caster', {}))
            paladin_defense.check_swing(ch)
            from service import equipment
            out = equipment.melee_procs(self.cat, ch, req, paladin_talents.melee(self.cat, ch, req, paladin.melee_procs(
                self.cat, ch, req, talent_effects.finish(self.cat, ch, req, melee.swing(self.cat, ch, req)))))
            paladin_defense.swing_timing(ch, out)
            paladin_auras.melee_threat(self.cat, ch, out)
            return self.convert(out, req.get('caster', {}),
                                'OneHanded' if ch.form in ('cat', 'bear', 'dire_bear') else skyrim.melee_skill(self.cat, ch))
        raise ValueError(f'unknown op {op!r}')

    def respond(self, line: bytes) -> dict:
        try:
            req = json.loads(line)
            if not isinstance(req, dict):
                raise ValueError('request must be an object')
            res = {'id': req.get('id'), 'ok': True, **self.handle(req)}
        except (spells.CastError, melee.MeleeError) as e:
            res = {'id': req.get('id'), 'ok': False, 'error': str(e)}
        except Exception as e:  # malformed input never kills the service
            res = {'id': (req.get('id') if isinstance(locals().get('req'), dict) else None), 'ok': False, 'error': f'BAD_REQUEST: {e}'}
        if self.trace:
            self.trace.write(json.dumps({'t': time.time(), 'req': line.decode('utf-8', 'replace')[:2048], 'res': res}) + '\n')
            self.trace.flush()
        return res

    async def client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        peer = writer.get_extra_info('peername')
        print(f'client connected {peer}', flush=True)
        try:
            discarding = False
            while True:
                try:
                    line = await reader.readuntil(b'\n')
                except asyncio.LimitOverrunError as e:
                    # readuntil leaves data buffered; discard only this line,
                    # including further chunks until its newline arrives.
                    await reader.readexactly(e.consumed)
                    if not discarding:
                        writer.write(b'{"ok":false,"error":"BAD_REQUEST: line too long"}\n')
                        await writer.drain()
                    discarding = True
                    continue
                except asyncio.IncompleteReadError as e:
                    line = e.partial
                if not line:
                    break
                if discarding:
                    discarding = False
                    continue
                if len(line) > MAX_LINE:
                    writer.write(b'{"ok":false,"error":"BAD_REQUEST: line too long"}\n')
                    await writer.drain()
                    continue
                writer.write((json.dumps(self.respond(line)) + '\n').encode())
                await writer.drain()
        except ConnectionError:
            pass
        finally:
            writer.close()
            print(f'client disconnected {peer}', flush=True)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=7878)
    ap.add_argument('--catalog', default=str(DEFAULT))
    ap.add_argument('--trace', help='append detailed request/response records to this JSONL file')
    ap.add_argument('--progression', type=Path, help=f'class progression data (default {DEFAULT_PROGRESSION})')
    inputs = ap.add_mutually_exclusive_group()
    inputs.add_argument("--resist-table", type=Path, help="locally extracted partial resistance JSON")
    inputs.add_argument("--cmangos-source", type=Path, help="local mangos-classic source folder or Unit.cpp")
    a = ap.parse_args()
    if a.resist_table or a.cmangos_source:
        try:
            F.configure_resists(table=a.resist_table, source=a.cmangos_source)
            F.partial_resist_distribution()
        except (OSError, ValueError) as exc:
            ap.error(str(exc))
    svc = Service(Catalog(Path(a.catalog)), Path(a.trace) if a.trace else None, Progression(a.progression) if a.progression else None)
    server = await asyncio.start_server(svc.client, '127.0.0.1', a.port, limit=MAX_LINE + 1)
    print(f'wowbridge service on 127.0.0.1:{a.port} (protocol {PROTOCOL})', flush=True)
    async with server:
        await server.serve_forever()


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
