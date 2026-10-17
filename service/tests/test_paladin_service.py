"""The class-aware service foundation: the Paladin's (and the Druid's unchanged) base rows, StatSystem.cpp formulas,
Human and Dwarf racials, class-aware percentage costs and caster.class on every op.

Expected values are hand computed from player_levelstats / player_classlevelstats rows and the cited CMaNGOS
formulas; the catalog is built in memory (data.build_catalog.build) because build/ may predate the class tables."""
import json
import unittest

from data.build_catalog import build
from service import character, formulas as F, melee, racials, skyrim, spells
from service.catalog import Catalog
from service.server import Service
from service.skyrim_balance import class_base_mana, spell_classes
from service.skyrim_effects import IMMUNE, MAGICKA_REGEN, RESIST_DISEASE, RESIST_POISON, RESISTS

STATS = ('str', 'agi', 'sta', 'int', 'spi')
# player_levelstats (race, class 2, level): str agi sta inte spi; player_classlevelstats (class 2): basehp basemana.
PALADIN_ROWS = {(1, 1): (22, 20, 22, 20, 22), (1, 20): (42, 31, 41, 32, 35), (1, 60): (105, 65, 100, 70, 78),
                (3, 1): (24, 16, 25, 19, 20), (3, 20): (44, 27, 44, 31, 33), (3, 60): (107, 61, 103, 69, 74)}
PALADIN_BASE = {1: (28, 59), 20: (191, 412), 60: (1381, 1512)}
# Hand-computed naked results: spirit after The Human Spirit (int32 of spirit x 1.05f), max health
# (basehp + 20 + (sta - 20) x 10), max mana (basemana + 20 + (int - 20) x 15, int <= 20 counts once), melee AP
# (level x 3 + str x 2 - 20), ranged AP (agi - 10), mana regen a second ((spi / 5 + 15) / 2), health regen a second
# (spi x 0.25), block value (int(str / 20 - 1)).
EXPECTED = {(1, 1): (23, 68, 79, 27, 10, 9.8, 5.75, 0), (1, 20): (36, 421, 612, 124, 21, 11.1, 9.0, 1),
            (1, 60): (81, 2201, 2282, 370, 55, 15.6, 20.25, 4),
            (3, 1): (20, 98, 78, 31, 6, 9.5, 5.0, 0), (3, 20): (33, 451, 597, 128, 17, 10.8, 8.25, 1),
            (3, 60): (74, 2231, 2267, 374, 51, 14.9, 18.5, 4)}


def rate(lo, hi, level):
    """The agility rate of Player::GetMeleeCritFromAgility / GetDodgeFromAgility at a level."""
    return lo * (60.0 - level) / 59.0 + hi * (level - 1.0) / 59.0


class PaladinService(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog(raw=build())
        cls.c = cls.cat.raw['character']

    def setUp(self):
        self.svc = Service(self.cat, None)

    def answer(self, req):
        return self.svc.respond(json.dumps(req).encode())

    def weapon(self, subclass, inventory=None):
        return next(e for e, it in sorted(self.cat.items.items()) if it['class'] == 2 and it['subclass'] == subclass
                    and it['InventoryType'] in ((inventory,) if inventory else character.INVTYPE_WEAPONS))

    # --- base rows -------------------------------------------------------------------------------------------

    def test_paladin_base_rows_human_and_dwarf(self):
        for (race, level), row in PALADIN_ROWS.items():
            with self.subTest(race=race, level=level):
                ls, cs = character.base_rows(self.c, 2, race, level)
                self.assertEqual(tuple(ls[k] for k in ('str', 'agi', 'sta', 'inte', 'spi')), row)
                self.assertEqual((cs['basehp'], cs['basemana']), PALADIN_BASE[level])
        # Above 60 a Paladin keeps her race's level-60 row; a race she cannot be falls back to the Human.
        self.assertEqual(character.base_rows(self.c, 2, 3, 75), character.base_rows(self.c, 2, 3, 60))
        self.assertEqual(character.base_rows(self.c, 2, 4, 20), character.base_rows(self.c, 2, 1, 20))

    def test_paladin_sheet_at_1_20_60(self):
        for (race, level), row in PALADIN_ROWS.items():
            spirit, health, mana, ap, rap, mp, hp, block_value = EXPECTED[(race, level)]
            agi, intellect = row[1], row[3]
            with self.subTest(race=race, level=level):
                ch = character.build(self.cat, [], level=level, cls=2, race=race)
                self.assertEqual(ch.stats, dict(zip(STATS, row[:4] + (spirit,))))
                self.assertEqual((ch.base_health, ch.base_mana), PALADIN_BASE[level])
                self.assertEqual((ch.max_health, ch.max_mana), (health, mana))
                self.assertEqual((ch.attack_power(), ch.ranged_attack_power()), (ap, rap))
                self.assertAlmostEqual(ch.melee_crit(), agi / rate(4.6, 20.0, level))
                self.assertAlmostEqual(ch.dodge(), 0.75 + agi / rate(4.6, 20.0, level))
                self.assertAlmostEqual(ch.spell_crit(1), 3.70 + intellect / (14.77 + 0.65 * level))
                self.assertAlmostEqual(ch.regen()['per_sec'], mp)
                self.assertAlmostEqual(ch.regen()['per_sec_casting'], 0)
                self.assertAlmostEqual(ch.health_regen()['per_sec'], hp)
                self.assertEqual(ch.health_regen()['per_sec_combat'], 0)
                # Block from creation (spell 107), Parry from the trainer at 8.
                self.assertEqual((ch.block_chance(), ch.block_value()), (5.0, block_value))
                self.assertEqual(ch.parry(), 5.0 if level >= 8 else 0.0)
                self.assertEqual(ch.resistance(4), 10 if race == 3 else 0)   # Dwarven Frost Resistance
                self.assertEqual(ch.weapon_skill_value(), level * 5)        # bare hands: no specialization

    def test_parry_from_level_8_and_block_value_with_a_shield(self):
        self.assertEqual(character.build(self.cat, [], level=7, cls=2, race=1).parry(), 0.0)
        self.assertEqual(character.build(self.cat, [], level=8, cls=2, race=1).parry(), 5.0)
        shield = next(e for e, it in self.cat.items.items() if it['InventoryType'] == character.INVTYPE_SHIELD)
        ch = character.build(self.cat, [shield], cls=2, race=1)
        self.assertEqual(ch.shield_entry, shield)
        block = self.cat.item(shield)['block']
        self.assertEqual(ch.block_value(), int(block + ch.stats['str'] / 20 - 1))
        druid = character.build(self.cat, [shield])
        self.assertEqual((druid.can_block(), druid.block_chance(), druid.can_parry(), druid.parry()), (False, 0, False, 0))

    # --- the Druid, unchanged ---------------------------------------------------------------------------------

    def test_druid_rows_are_the_legacy_rows_at_every_level_and_race(self):
        c = self.c
        for race in (None, 4, 6, 1):
            for level in list(range(1, 61)) + [0, 61, 100]:
                with self.subTest(race=race, level=level):
                    r = character.default_race(c, 11) if race is None else race
                    legacy = (racials.levelstats(r, level) or c['levelstats_by_level'].get(str(level), c['levelstats']),
                              c['classlevelstats_by_level'].get(str(level), c['classlevelstats']))
                    if race == 6 and level > 60:   # fixed: a Tauren past 60 keeps her own level-60 row (was a Night Elf's)
                        legacy = (racials.levelstats(6, 60), legacy[1])
                    self.assertEqual(character.base_rows(c, 11, r, level), legacy)
                    if race in (4, 6) and 1 <= level <= 60:
                        self.assertEqual(c['levelstats_by_class']['11'][str(race)][str(level)], legacy[0])
                        self.assertEqual(c['classlevelstats_by_class']['11'][str(level)], legacy[1])

    def test_druid_sheet_follows_the_druid_formulas(self):
        for race in (4, 6):
            for level in range(1, 61):
                ch = character.build(self.cat, [], level=level, race=race)
                ls = self.c['levelstats_by_class']['11'][str(race)][str(level)]
                cs = self.c['classlevelstats_by_class']['11'][str(level)]
                with self.subTest(race=race, level=level):
                    self.assertEqual(ch.stats, {'str': ls['str'], 'agi': ls['agi'], 'sta': ls['sta'],
                                                'int': ls['inte'], 'spi': ls['spi']})
                    self.assertEqual(ch.attack_power(), level * 3 + ls['str'] * 2 - 20)
                    self.assertAlmostEqual(ch.melee_crit(), ls['agi'] / rate(4.6, 20.0, level))
                    self.assertAlmostEqual(ch.regen()['per_sec'], (ls['spi'] / 5 + 15) / 2)
                    self.assertAlmostEqual(ch.health_regen()['per_sec'], ls['spi'] * 0.11 + 1)
                    self.assertAlmostEqual(ch.spell_crit(3), 3.33 + ls['inte'] / (12.41 + 0.79 * level))
                    self.assertEqual((ch.base_health, ch.base_mana), (cs['basehp'], cs['basemana']))
                    self.assertEqual(ch.max_mana, cs['basemana'] + min(ls['inte'], 20) + max(0, ls['inte'] - 20) * 15)
                    self.assertEqual(ch.weapon_skill_value(), level * 5)
                    self.assertEqual((ch.parry(), ch.block_chance()), (0.0, 0.0))
                    # Night Elf Quickness: SPELL_AURA_MOD_DODGE_PERCENT 1.
                    self.assertAlmostEqual(ch.dodge(), 0.75 + ls['agi'] / rate(4.6, 20.0, level) + (race == 4))
        ne = character.build(self.cat, [])
        self.assertEqual((ne.stats, ne.max_health, ne.max_mana, ne.attack_power()),
                         ({'str': 62, 'agi': 65, 'sta': 69, 'int': 100, 'spi': 110}, 1993, 2464, 284.0))

    def test_requests_without_a_class_are_the_druid(self):
        plain = self.answer({'op': 'stats', 'caster': {'level': 60}})
        druid = self.answer({'op': 'stats', 'caster': {'level': 60, 'class': 11}})
        named = self.answer({'op': 'stats', 'caster': {'level': 60, 'class': 'Druid'}})
        self.assertEqual(plain, druid)
        self.assertEqual(plain, named)
        self.assertEqual((plain['class'], plain['race'], plain['stats']['int']), (11, 4, 100))

    # --- caster.class on every op ---------------------------------------------------------------------------

    def test_every_op_honours_the_class(self):
        caster = {'class': 2, 'race': 1, 'level': 60}
        stats = self.answer({'op': 'stats', 'caster': caster})
        self.assertTrue(stats['ok'], stats)
        self.assertEqual((stats['class'], stats['race'], stats['max_mana'], stats['max_health']), (2, 1, 2282, 2201))
        self.assertEqual((stats['attack_power'], stats['ranged_attack_power']), (370.0, 55.0))
        self.assertEqual(stats['defense']['block_value'], 4)
        self.assertEqual(self.answer({'op': 'stats', 'caster': {**caster, 'class': 'paladin'}}), stats)
        # No race: the class's first (the Human).
        self.assertEqual(self.answer({'op': 'stats', 'caster': {'class': 2}})['race'], 1)
        # The plugin's shape: talents, perks and Skyrim skills.
        native = self.answer({'op': 'stats', 'caster': {**caster, 'talents': {}, 'perks': {}, 'skyrim': {'one_handed': 30}}})
        self.assertTrue(native['ok'], native)
        self.assertEqual(native['max_mana'], 2282)
        # A Paladin's percentage costs come from her create mana (Spell::CalculatePowerCost).
        kings = self.cat.spell(20217)
        cast = self.answer({'op': 'resolve', 'spell': 20217, 'seed': 1, 'caster': caster, 'target': {'kind': 'self'}})
        self.assertEqual(cast['cost'], kings['ManaCostPercentage'] * 1512 // 100)
        druid_cast = self.answer({'op': 'resolve', 'spell': 20217, 'seed': 1, 'caster': {'level': 60}, 'target': {'kind': 'self'}})
        self.assertEqual(druid_cast['cost'], kings['ManaCostPercentage'] * 1244 // 100)
        target = {'kind': 'unit', 'level': 60, 'reaction': 'hostile', 'alive': True, 'distance_yd': 2, 'facing': True}
        swing = self.answer({'op': 'melee', 'seed': 3, 'caster': {**caster, 'items': [self.weapon(7)]}, 'target': target})
        self.assertTrue(swing['ok'], swing)
        costs = self.answer({'op': 'skyrim_costs', 'caster': {**caster, 'skyrim': {'alteration': 40}}})
        self.assertTrue(costs['ok'], costs)
        for bad, error in (({'class': 'foo'}, 'BAD_REQUEST: caster.class must be a playable class ID or name'),
                           ({'class': 2, 'form': 'cat'}, 'BAD_REQUEST: caster.form: only a Druid shapeshifts'),
                           ({'class': True}, 'BAD_REQUEST: caster.class must be a playable class ID or name')):
            self.assertEqual(self.answer({'op': 'stats', 'caster': bad})['error'], error)

    # --- racials ---------------------------------------------------------------------------------------------

    def test_human_spirit_uses_single_precision(self):
        # Unit::GetTotalStatValue: 100 x 1.05f = 104.99999 -> 104, as CMaNGOS truncates it.
        self.assertEqual(F.stat_total(100, 0, (), (5,)), 104)
        self.assertEqual(F.stat_total(78, 0, (), (5,)), 81)
        self.assertEqual(F.stat_total(70, 8, (), (5,)), 81)            # buffs join before the percentage
        self.assertEqual(F.stat_total(50, 0, (10,), (10,)), 60)        # base percent, then total percent
        human = character.build(self.cat, [], cls=2, race=1)
        dwarf = character.build(self.cat, [], cls=2, race=3)
        self.assertEqual((human.stats['spi'], dwarf.stats['spi']), (81, 74))
        self.assertIn(racials.THE_HUMAN_SPIRIT, {a['spell'] for a in human.auras})

    def test_sword_and_mace_specialization_on_the_attack_table(self):
        sword, two_hand_sword, two_hand_mace, staff = self.weapon(7), self.weapon(8), self.weapon(5), self.weapon(10)
        target = {'level': 60, 'in_front': True}
        for items, race, skill in (([sword], 1, 305), ([two_hand_sword], 1, 305), ([two_hand_mace], 1, 305),
                                   ([staff], 1, 300), ([sword], 3, 300), ([], 1, 300)):
            with self.subTest(items=items, race=race):
                ch = character.build(self.cat, items, cls=2, race=race)
                self.assertEqual(ch.weapon_skill_value(), skill)
                table = melee.chances(ch, target)
                bonus = skill - 300
                # CalculateEffectiveMissChance / DodgeChance: 0.04 a point once the skill beats the defense.
                self.assertAlmostEqual(table['miss'], 5.0 - bonus * 0.04)
                self.assertAlmostEqual(table['dodge'], 5.0 - bonus * 0.04)
                self.assertAlmostEqual(table['glance'], 10.0)       # glancing uses the capped skill
                self.assertAlmostEqual(table['crit'], ch.melee_crit() + bonus * 0.2)
                # Against a player the crit uses GetSkillMaxForLevel, not the weapon skill.
                self.assertAlmostEqual(melee.chances(ch, {**target, 'is_player': True})['crit'], ch.melee_crit())
        stats = self.answer({'op': 'stats', 'caster': {'class': 2, 'race': 1, 'items': [sword]}})
        self.assertEqual(stats['attack_rating'], 305)
        specs = {p['spell']: p for p in stats['racials'] if p['type'] == 'weapon_skill'}
        self.assertEqual((specs[20597]['skills'], specs[20597]['active']), ([43, 55], True))
        self.assertEqual((specs[20864]['skills'], specs[20864]['active']), ([54, 160], False))

    def test_passive_metadata_and_frost_resistance(self):
        human = self.answer({'op': 'stats', 'caster': {'class': 2, 'race': 1, 'skyrim': {}}})
        self.assertEqual([p['spell'] for p in human['racials']], [20597, 20598, 20599, 20864])
        ps = {p['spell']: p for p in human['racials']}
        self.assertEqual((ps[20598]['type'], ps[20598]['stat'], ps[20598]['amount']), ('stat_percent', 4, 5))
        # Its share of her Spirit is a Spirit buff's native: Fortify Magicka Rate, a point a Spirit point.
        self.assertEqual(ps[20598]['skyrim'], [{'mgef': MAGICKA_REGEN, 'magnitude': ps[20598]['spirit']}])
        self.assertEqual((ps[20599]['type'], ps[20599]['amount'], ps[20599]['informational']), ('reputation_gain', 10, True))
        self.assertTrue(ps[20597]['wow_attack_table'])
        self.assertEqual([s['spell'] for s in human['racial_spells']], [20600])
        dwarf = self.answer({'op': 'stats', 'caster': {'class': 2, 'race': 3, 'skyrim': {}}})
        ps = {p['spell']: p for p in dwarf['racials']}
        self.assertEqual(set(ps), {20595, 20596})
        self.assertTrue(ps[20595]['informational'])
        self.assertEqual((ps[20596]['amount'], ps[20596]['school_mask']), (10, 16))
        self.assertEqual(ps[20596]['skyrim'], [{'mgef': RESISTS['frost_res'], 'magnitude': 10 / 3}])
        self.assertEqual(dwarf['resist']['4'], 10)
        self.assertEqual([s['spell'] for s in dwarf['racial_spells']], [20594, 2481])
        # Their Skyrim carriers exist: nothing reports a pending carrier any more.
        self.assertFalse(any('pending_carrier' in s for s in human['racial_spells'] + dwarf['racial_spells']))
        # A race the class cannot be has no racials; the Druid's races keep theirs.
        for caster in ({'race': 1}, {'race': 3}, {'class': 2, 'race': 4}, {'class': 2, 'race': 6}):
            out = self.answer({'op': 'stats', 'caster': caster})
            self.assertEqual((out['racials'], out['racial_spells']), ([], []), caster)
        self.assertEqual({p['spell'] for p in self.answer({'op': 'stats', 'caster': {}})['racials']}, {20582, 20583, 20585})

    def test_human_spirit_share_is_its_native_magnitude(self):
        # The share is total Spirit less the Spirit without the aura (Unit::GetTotalStatValue, truncated both ways):
        # naked Human Paladin rows 22 / 35 / 78 Spirit (player_levelstats) give 23 / 36 / 81.
        for level, share in ((1, 1), (20, 1), (60, 3)):
            with self.subTest(level=level):
                out = self.answer({'op': 'stats', 'caster': {'class': 2, 'race': 1, 'level': level, 'skyrim': {}}})
                p = next(p for p in out['racials'] if p['spell'] == racials.THE_HUMAN_SPIRIT)
                self.assertEqual((p['spirit'], p['skyrim']), (share, [{'mgef': MAGICKA_REGEN, 'magnitude': share}]))
        # Spirit gear joins the base the percentage scales (Player::_ApplyItemBonuses is BASE_VALUE).
        item = next(e for e, it in sorted(self.cat.items.items()) if any(
            it[f'stat_type{k}'] == 6 and it[f'stat_value{k}'] >= 20 for k in range(1, 11)))
        spirit = sum(self.cat.item(item)[f'stat_value{k}'] for k in range(1, 11) if self.cat.item(item)[f'stat_type{k}'] == 6)
        ch = character.build(self.cat, [item], cls=2, race=1)
        expected = F.stat_total(78 + spirit, 0, (), (5,)) - (78 + spirit)
        self.assertEqual((ch.stats['spi'], racials.spirit_share(ch, racials.THE_HUMAN_SPIRIT)), (78 + spirit + expected, expected))
        self.assertGreater(expected, 3)
        # Without a Skyrim caster the metadata names no native; a Dwarf has no such racial.
        out = self.answer({'op': 'stats', 'caster': {'class': 2, 'race': 1, 'level': 60}})
        self.assertNotIn('skyrim', next(p for p in out['racials'] if p['spell'] == racials.THE_HUMAN_SPIRIT))
        dwarf = character.build(self.cat, [], cls=2, race=3)
        self.assertEqual(racials.spirit_share(dwarf, racials.THE_HUMAN_SPIRIT), 0)

    def test_perception_stoneform_and_find_treasure_resolve(self):
        def cast(sid, race, **extra):
            return self.answer({'op': 'resolve', 'spell': sid, 'seed': 1, 'target': {'kind': 'self'},
                                'caster': {'class': 2, 'race': race, 'level': 60, **extra}})
        out = cast(racials.PERCEPTION, 1)
        self.assertTrue(out['ok'], out)
        self.assertEqual((out['cost'], out['gcd_ms'], out['cooldown_ms']), (0, 1500, 180000))
        self.assertNotIn('pending_carrier', out)
        self.assertEqual([(a['type'], a['amount'], a['duration_ms'], a['stealth_type']) for a in out['auras']],
                         [('stealth_detection', 50, 20000, 0)])
        out = cast(racials.STONEFORM, 3, skyrim={'alteration': 30})
        self.assertTrue(out['ok'], out)
        self.assertEqual((out['cooldown_ms'], out['skill']), (180000, 'Alteration'))
        auras = [(a['spell'], a['type'], a.get('amount'), a.get('dispel') or a.get('name'), a['duration_ms'])
                 for a in out['auras']]
        self.assertEqual(auras, [(20594, 'armor_percent', 10, None, 8000), (20594, 'dispel_immunity', None, 'Poison', 8000),
                                 (20612, 'mechanic_immunity', 0, 'bleed', 8000), (20612, 'dispel_immunity', None, 'Disease', 8000)])
        self.assertTrue(all(a['purges_existing'] for a in out['auras'] if a['aura'] in (41, 77)))
        # Skyrim's own immunity to each dispel type (Hevnoraak's 100 Resist Poison / Resist Disease); the armor share
        # is Cast's armor_percent and the bleed purge Carriers', so neither names a vanilla effect.
        self.assertEqual([a.get('skyrim') for a in out['auras']],
                         [None, [{'mgef': RESIST_POISON, 'magnitude': IMMUNE}], None,
                          [{'mgef': RESIST_DISEASE, 'magnitude': IMMUNE}]])
        self.assertEqual((RESIST_POISON, RESIST_DISEASE, IMMUNE), (0x0FF15E, 0x100E60, 100))
        self.assertFalse(any('pending_carrier' in a for a in out['auras']) or 'pending_carrier' in out)
        out = cast(racials.FIND_TREASURE, 3)
        self.assertEqual([(a['type'], a['misc'], a['mask'], a['duration_ms']) for a in out['auras']],
                         [('track_resources', 6, 32, -1)])
        # Known only by their own race and class; self-cast; passives are never cast.
        for sid, race in ((racials.PERCEPTION, 3), (racials.STONEFORM, 1), (racials.FIND_TREASURE, 1),
                          (racials.SHADOWMELD, 1), (racials.WAR_STOMP, 3), (racials.THE_HUMAN_SPIRIT, 1),
                          (racials.FROST_RESISTANCE, 3)):
            self.assertEqual(cast(sid, race)['error'], 'SPELL_FAILED_NOT_KNOWN', (sid, race))
        self.assertEqual(self.answer({'op': 'resolve', 'spell': racials.PERCEPTION, 'caster': {'race': 1},
                                      'target': {'kind': 'self'}})['error'], 'SPELL_FAILED_NOT_KNOWN')
        bad = self.answer({'op': 'resolve', 'spell': racials.STONEFORM, 'caster': {'class': 2, 'race': 3},
                           'target': {'kind': 'unit', 'level': 60, 'distance_yd': 2}})
        self.assertEqual(bad['error'], 'SPELL_FAILED_BAD_TARGETS')

    # --- class-aware percentage costs ------------------------------------------------------------------------

    def test_percentage_costs_use_the_casters_class_base_mana(self):
        classes = spell_classes()
        self.assertTrue(all(class_base_mana(self.cat, sid) == 1244 for sid, cls in classes.items() if cls == 11))
        paladin = [sid for sid, cls in classes.items() if cls == 2 and sid in self.cat.spells
                   and self.cat.spell(sid)['ManaCostPercentage'] > 0 and self.cat.spell(sid)['PowerType'] == 0]
        self.assertIn(20217, paladin)
        caster = {'class': 2, 'race': 1, 'level': 60, 'skyrim': {'alteration': 40, 'restoration': 40}}
        costs = self.answer({'op': 'skyrim_costs', 'caster': caster})['costs']
        for sid in paladin:
            s, rule = self.cat.spell(sid), self.cat.balance[sid]
            with self.subTest(spell=sid):
                self.assertEqual(class_base_mana(self.cat, sid), 1512)
                top = max((r for r in self.cat.ranks if self.cat.chain(r) == self.cat.chain(sid)),
                          key=lambda r: (self.cat.ranks[r]['rank'], r))
                t = self.cat.spell(top)
                self.assertEqual(rule.wow_cost, max(1, t['ManaCost'] + t['ManaCostPercentage'] * 1512 // 100 +
                                                    t['ManaCostPerlevel'] * (F.spell_rank_level(t, 60) - t['BaseLevel'])))
                # Her level-60 WoW cost is the rule's own: the native cost keeps the counterpart's price exactly.
                if sid == top:
                    skill = skyrim.spell_skill(s, self.cat.balance)
                    native = rule.base_cost * skyrim.cost_factor(caster['skyrim'].get(skyrim.SKILLS[skill], 50))
                    self.assertEqual(costs[str(sid)], max(1, round(native)))
                    ch = self.svc.char(caster)
                    self.assertEqual(spells._cost(s, ch), rule.wow_cost)


class Formulas(unittest.TestCase):
    """StatSystem.cpp / Player.cpp / Unit.cpp class tables, against literal CMaNGOS values."""

    def test_attack_power_by_class(self):
        level, strength, agility = 60, 100, 50
        expected = {1: 180 + 200 - 20, 2: 180 + 200 - 20, 11: 180 + 200 - 20, 4: 120 + 100 + 50 - 20,
                    3: 120 + 100 + 50 - 20, 7: 120 + 200 - 20, 5: 100 - 10, 8: 100 - 10, 9: 100 - 10}
        for cls, value in expected.items():
            self.assertEqual(F.attack_power_base(cls, level, strength, agility), value, cls)
        ranged = {3: 120 + 100 - 10, 4: 60 + 50 - 10, 1: 60 + 50 - 10, 2: 40, 11: 40, 5: 40, 7: 40, 8: 40, 9: 40}
        for cls, value in ranged.items():
            self.assertEqual(F.ranged_attack_power_base(cls, level, agility), value, cls)
        self.assertEqual(F.ranged_attack_power_base(11, level, agility, feral_form=True), 0)

    def test_dodge_regen_and_block_tables(self):
        self.assertAlmostEqual(F.dodge_from_agility(4, 60, 145), 10.0)      # rogue 1.1 .. 14.5
        self.assertAlmostEqual(F.dodge_from_agility(3, 60, 53), 2.0)        # hunter 1.8 .. 26.5
        self.assertAlmostEqual(F.dodge_from_agility(2, 1, 46), 10.0)        # paladin 4.6 at level 1
        self.assertEqual((F.BASE_DODGE[2], F.BASE_DODGE[11], F.BASE_DODGE[5]), (0.75, 0.75, 3.0))
        spirit = 100
        for cls, value in {11: 12, 2: 25, 3: 37.5, 8: 12, 5: 16.4, 4: 71, 7: 24.4, 9: 13.5, 1: 103.4}.items():
            self.assertAlmostEqual(F.health_regen_per_second(cls, spirit), value, msg=cls)
        self.assertEqual(F.mana_regen_per_second(2, 100), 17.5)
        self.assertEqual((F.shield_block_value(40, 105), F.shield_block_value(0, 10), F.shield_block_value(40, 100, 1.3)),
                         (44, 0, 57))


if __name__ == '__main__':
    unittest.main()
