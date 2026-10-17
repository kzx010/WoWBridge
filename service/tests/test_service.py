"""Service tests: values hand-checked against the CMaNGOS formulas they port (see service/formulas.py)."""
import asyncio
import json
import statistics
import unittest

from service import character, formulas as F, melee, spells
from service.catalog import Catalog
from service.server import MAX_LINE, Service

STORMRAGE = [16897, 16898, 16899, 16900, 16901, 16902, 16903, 16904]
FULL = STORMRAGE + [873, 22398]
HOSTILE = {'kind': 'unit', 'level': 60, 'reaction': 'hostile', 'alive': True, 'distance_yd': 20, 'los': True}


class Fixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()
        cls.full = character.build(cls.cat, FULL)
        cls.naked = character.build(cls.cat, [])

    def cast(self, ch, name, seed=1, target=None, ctx=None):
        return spells.resolve(self.cat, ch, {'spell': self.cat.slice[name], 'seed': seed, 'target': target or HOSTILE, 'ctx': ctx or {}})


class Character(Fixture):
    def test_full_kit_stats(self):
        ch = self.full
        self.assertEqual((ch.stats['int'], ch.stats['spi']), (288, 224))
        self.assertEqual(ch.max_mana, 1244 + 20 + 268 * 15)                       # 5284
        self.assertAlmostEqual(ch.spell_crit(3), 3.33 + 288 / (12.41 + 0.79 * 60) + 2, places=4)
        self.assertEqual((ch.spell_damage(8), ch.healing_done(8)), (26, 301))
        r = ch.regen()
        self.assertAlmostEqual(r['per_sec'], (224 / 5 + 15) / 2 + 20 / 5, places=6)
        self.assertAlmostEqual(r['per_sec_casting'], 20 / 5 + (224 / 5 + 15) / 2 * 0.15, places=6)

    def test_naked_level60_baseline(self):
        self.assertEqual(self.naked.max_mana, 1244 + 20 + 80 * 15)               # int 100


class Spells(Fixture):
    def test_wrath_range_and_crit(self):
        dmg, crits = [], 0
        for seed in range(3000):
            e = self.cast(self.naked, 'wrath', seed)['events']
            if e[0]['type'] != 'damage':
                continue
            if e[0]['crit']:
                crits += 1
            else:
                dmg.append(e[0]['amount'])
        self.assertEqual((min(dmg), max(dmg)), (int(236 + 12.6), int(264 + 12.6)))   # no gear: no bonus
        rate = crits / (len(dmg) + crits) * 100
        self.assertAlmostEqual(rate, F.spell_crit_from_int(11, 60, 100), delta=1.5)

    def test_miss_rate_vs_level(self):
        self.assertEqual(F.spell_miss_chance(60, 60, False, 0), 4.0)
        self.assertEqual(F.spell_miss_chance(60, 63, False, 0), 17.0)
        misses = sum(self.cast(self.naked, 'wrath', s)['events'][0]['type'] == 'miss' for s in range(4000))
        self.assertAlmostEqual(misses / 40, 4.0, delta=1.0)

    def test_resistance(self):
        """A resistant target fully resists some of a player's non-binary spells (Unit.cpp:4018-4022) and partly resists
        the rest in fifths up to four (CMaNGOS: min(portion, 4) / NUM_SPELL_PARTIAL_RESISTS)."""
        resistant = {**HOSTILE, 'resist': {'3': 300}}   # nature
        outs = [self.cast(self.full, 'wrath', seed=s, target=resistant) for s in range(400)]
        kinds = [o['events'][0]['type'] for o in outs]
        self.assertGreater(kinds.count('resist'), 0)
        shares = {round(e['resisted'] / (e['amount'] + e['resisted']), 1) for o in outs for e in o['events'] if e['type'] == 'damage' and e['resisted']}
        self.assertTrue(shares <= {0.2, 0.4, 0.6, 0.8}, shares)

    def test_moonfire_direct_and_ticks(self):
        r = self.cast(self.full, 'moonfire', seed=3)
        dot = r['auras'][0]
        self.assertEqual((dot['per_tick'], len(dot['ticks']), dot['amplitude_ms']), (96 + int(26 * 0.13), 4, 3000))
        self.assertEqual([t['t_ms'] for t in dot['ticks']], [3000, 6000, 9000, 12000])

    def test_rejuvenation_set_bonus_and_idol(self):
        def rejuv(items):
            return self.cast(character.build(self.cat, items), 'rejuvenation', target={'kind': 'self'})['auras'][0]
        base, eight = rejuv([]), rejuv(STORMRAGE)
        self.assertEqual((base['duration_ms'], len(base['ticks'])), (12000, 4))
        self.assertEqual((eight['duration_ms'], len(eight['ticks'])), (15000, 5))   # Stormrage 8-pc (21871)
        idol = rejuv(STORMRAGE + [22398])
        self.assertEqual(idol['per_tick'] - eight['per_tick'], int(50 * 0.2))       # labelled custom idol rule

    def test_entangling_roots(self):
        r = self.cast(self.full, 'entangling_roots', seed=11)
        root, dot = r['auras']
        self.assertEqual((root['type'], root['duration_ms']), ('root', 27000))
        self.assertEqual(root['heartbeat']['interval_ms'], 13500)
        self.assertEqual(len(dot['ticks']), 9)
        # SpellMgr::IsSingleTargetSpell: a player's root (family flags 0x200) holds one unit; its damage aura is not one.
        self.assertEqual(root['single_target'], {'chain': 339, 'family': 7, 'flags': 0x200})
        self.assertNotIn('single_target', dot)
        with self.assertRaisesRegex(spells.CastError, 'ONLY_OUTDOORS'):
            self.cast(self.full, 'entangling_roots', ctx={'indoors': True})

    def test_category_cooldown(self):
        # Hurricane's minute is its category's (CategoryRecoveryTime 60000, RecoveryTime 0); Wrath has none.
        r = self.cast(self.full, 'hurricane', target={'kind': 'ground', 'distance_yd': 10, 'los': True})
        self.assertEqual((r['cooldown_ms'], r['channel_interrupt_flags'] & 0x4000), (60000, 0x4000))
        self.assertEqual(self.cast(self.full, 'wrath')['cooldown_ms'], 0)

    def test_facing_is_per_spell(self):
        # spell_facing lists Wrath and Moonfire; Entangling Roots may be cast at a target behind you.
        self.cast(self.full, 'entangling_roots', target={**HOSTILE, 'in_front': False})
        with self.assertRaisesRegex(spells.CastError, 'UNIT_NOT_INFRONT'):
            self.cast(self.full, 'moonfire', target={**HOSTILE, 'in_front': False})

    def test_validation_errors(self):
        for target, code in [({**HOSTILE, 'distance_yd': 31}, 'OUT_OF_RANGE'), ({**HOSTILE, 'los': False}, 'LINE_OF_SIGHT'),
                             ({**HOSTILE, 'reaction': 'friendly'}, 'BAD_TARGETS'), ({**HOSTILE, 'alive': False}, 'TARGETS_DEAD'),
                             ({**HOSTILE, 'in_front': False}, 'UNIT_NOT_INFRONT')]:
            with self.assertRaisesRegex(spells.CastError, code):
                self.cast(self.full, 'wrath', target=target)
        with self.assertRaisesRegex(spells.CastError, 'BAD_TARGETS'):
            self.cast(self.full, 'healing_touch', target=HOSTILE)

    def test_friendly_yet_attackable(self):
        # Skyrim lets her strike a townsperson: a friendly unit the plugin marks attackable takes harmful spells, and
        # still her heals; a neutral creature takes no heal (not friendly), as in WoW.
        townsperson = {**HOSTILE, 'reaction': 'friendly', 'attackable': True}
        self.assertIn('cost', self.cast(self.full, 'wrath', target=townsperson))
        self.assertIn('cost', self.cast(self.full, 'healing_touch', target=townsperson))
        with self.assertRaisesRegex(spells.CastError, 'BAD_TARGETS'):
            self.cast(self.full, 'healing_touch', target={**HOSTILE, 'reaction': 'neutral'})
        # A creature that is not hostile (a chicken) takes her heal when the plugin marks it healable, full health or not.
        self.assertIn('cost', self.cast(self.full, 'healing_touch', target={**HOSTILE, 'reaction': 'neutral', 'healable': True}))

    def test_hurricane_area_tick(self):
        r = self.cast(self.full, 'hurricane', target={'kind': 'ground', 'distance_yd': 20, 'los': True})
        area = r['auras'][0]
        self.assertEqual((area['duration_ms'], area['amplitude_ms'], area['area']['radius_yd']), (10000, 1000, 8.0))
        tick = spells.tick_area(self.cat, self.full, {'spell': 17402, 'seed': 5, 'targets': [{'id': 1, 'level': 60}, {'id': 2, 'level': 60}]})
        self.assertEqual({t['amount'] for t in tick['results'] if t['type'] == 'damage'}, {134 + int(26 * 0.03)})
        # Each tick re-rolls the hit per target (SpellAuras.cpp:4621); 4% miss at equal level -> ~4.26% incl. table.
        two = [{'id': 1, 'level': 60}, {'id': 2, 'level': 60}]
        res = [r for k in range(4000) for r in spells.tick_area(self.cat, self.full, {'spell': 17402, 'seed': k, 'targets': two})['results']]
        misses = sum(r['type'] == 'miss' for r in res) / len(res)
        self.assertAlmostEqual(misses, F.spell_miss_chance(60, 60, False, 0) / 100, delta=0.01)
        self.assertTrue(all(r.get('silent') for r in res if r['type'] == 'miss'))

    def test_partial_resist_table_and_damage_break(self):
        dist = F.partial_resist_distribution()
        self.assertEqual(len(dist), 10001)
        self.assertEqual(dist[10000], (0, 0, 0, 0, 10000))
        self.assertTrue(all(sum(row) == 10000 for row in dist[::250]))
        self.assertAlmostEqual(F.effective_resist_percent(60, 63, 0, False, True), 6.0)
        self.assertAlmostEqual(F.damage_break_chance(135, 60), 10.0)


class Melee(Fixture):
    MOB = {'level': 60, 'distance_yd': 3.0, 'alive': True, 'reaction': 'hostile', 'facing': True, 'in_front': True}

    def test_attack_power_weapon_and_crit(self):
        ch = self.full
        self.assertEqual(ch.weapon, {'min': 119.0, 'max': 180.0, 'delay': 3700})        # Staff of Jordan
        self.assertEqual(ch.attack_power(), 60 * 3 + 62 * 2 - 20)                      # 284
        self.assertAlmostEqual(ch.melee_crit(), 65 / 20.0)                              # agility / 20 at 60

    def test_same_level_table(self):
        c = melee.chances(self.full, self.MOB)
        self.assertEqual({k: round(v, 4) for k, v in c.items()},
                         {'miss': 5.0, 'dodge': 5.0, 'parry': 5.0, 'block': 5.0, 'glance': 10.0, 'crit': 3.25})
        behind = melee.chances(self.full, {**self.MOB, 'in_front': False})
        self.assertEqual((behind['parry'], behind['block']), (0.0, 0.0))

    def test_boss_level_table(self):
        c = melee.chances(self.full, {**self.MOB, 'level': 63})                       # 15 points of defense
        self.assertAlmostEqual(c['miss'], 5 + 1 + 5 * 0.2 + 5 * 0.4)                   # 9
        self.assertAlmostEqual(c['dodge'], 6.5)
        self.assertAlmostEqual(c['parry'], 5 + 15 * 0.6)                               # 14
        self.assertAlmostEqual(c['glance'], 40.0)
        self.assertAlmostEqual(c['crit'], 0.25)

    def test_armor(self):
        self.assertAlmostEqual(F.armor_reduced(1000, 3791, 60), 1000 * (1 - 0.68927 / 1.68927), places=0)
        self.assertEqual(F.armor_reduced(10, 1e9, 60), 2.5)                            # capped at 75%

    def test_swing_damage_range(self):
        hits = [melee.swing(self.cat, self.full, {'seed': s, 'target': self.MOB}) for s in range(400)]
        normal = [h['damage'] for h in hits if h['outcome'] == 'hit']
        lo = int(F.armor_reduced(int(119 + 284 / 14 * 3.7), 3791, 60))
        hi = int(F.armor_reduced(int(180 + 284 / 14 * 3.7), 3791, 60))
        self.assertTrue(normal and lo <= min(normal) and max(normal) <= hi)
        crits = [h['damage'] for h in hits if h['outcome'] == 'crit']
        self.assertTrue(crits and min(crits) >= 2 * lo)
        self.assertTrue(all(h['damage'] == 0 for h in hits if h['outcome'] in ('miss', 'dodge', 'parry')))
        self.assertEqual(hits[7], melee.swing(self.cat, self.full, {'seed': 7, 'target': self.MOB}))   # seeded

    def test_errors(self):
        for target, code in (({**self.MOB, 'distance_yd': 6.0}, 'ERR_BADATTACKPOS'), ({**self.MOB, 'facing': False}, 'ERR_BADATTACKFACING'),
                             ({**self.MOB, 'alive': False}, 'ERR_INVALID_ATTACK_TARGET'),
                             ({**self.MOB, 'reaction': 'friendly'}, 'ERR_INVALID_ATTACK_TARGET')):
            with self.assertRaises(melee.MeleeError) as e:
                melee.swing(self.cat, self.full, {'seed': 1, 'target': target})
            self.assertEqual(str(e.exception), code)
        # A townsperson she may strike (friendly, marked attackable): swung at.
        self.assertIn('outcome', melee.swing(self.cat, self.full, {'seed': 1, 'target': {**self.MOB, 'reaction': 'friendly', 'attackable': True}}))


class Server(Fixture):
    def test_oversized_line_then_hello(self):
        class RecordingWriter:
            def __init__(self):
                self.data = bytearray()
                self.closed = False

            def get_extra_info(self, name):
                return None

            def write(self, data):
                self.data.extend(data)

            async def drain(self):
                pass

            def close(self):
                self.closed = True

        async def run(fragmented):
            svc = Service(self.cat, None)
            reader = asyncio.StreamReader(limit=MAX_LINE + 1)
            writer = RecordingWriter()
            reader.feed_data(b'x' * (MAX_LINE + 2))
            if fragmented:
                task = asyncio.create_task(svc.client(reader, writer))
                await asyncio.sleep(0)
                reader.feed_data(b'x' * (MAX_LINE + 2))
                await asyncio.sleep(0)
            reader.feed_data(b'\n{"op":"hello","id":1}\n')
            reader.feed_eof()
            if fragmented:
                await task
            else:
                await svc.client(reader, writer)
            self.assertTrue(writer.closed)
            return [json.loads(line) for line in writer.data.splitlines()]

        for fragmented in (False, True):
            with self.subTest(fragmented=fragmented):
                self.assertEqual(asyncio.run(run(fragmented)), [
                    {'ok': False, 'error': 'BAD_REQUEST: line too long'},
                    {'id': 1, 'ok': True, 'protocol': 1, 'service': 'wowbridge',
                     'catalog': self.cat.raw['provenance'], 'slice': self.cat.slice},
                ])

    def test_tcp_roundtrip(self):
        async def run():
            svc = Service(self.cat, None)
            server = await asyncio.start_server(svc.client, '127.0.0.1', 0)
            port = server.sockets[0].getsockname()[1]
            reader, writer = await asyncio.open_connection('127.0.0.1', port)
            out = []
            for msg in [{'op': 'hello', 'id': 1}, {'op': 'resolve', 'id': 2, 'spell': 9912, 'seed': 1, 'target': HOSTILE},
                        {'op': 'resolve', 'id': 3, 'spell': 9912, 'target': {**HOSTILE, 'distance_yd': 99}}]:
                writer.write((json.dumps(msg) + '\n').encode())
                out.append(json.loads(await reader.readline()))
            writer.write(b'not json\n')
            out.append(json.loads(await reader.readline()))
            writer.close()
            server.close()
            return out
        hello, ok, err, bad = asyncio.run(run())
        self.assertEqual((hello['protocol'], ok['ok'], ok['id']), (1, True, 2))
        self.assertEqual((err['ok'], err['error']), (False, 'SPELL_FAILED_OUT_OF_RANGE'))
        self.assertTrue(bad['error'].startswith('BAD_REQUEST'))


if __name__ == '__main__':
    unittest.main()
