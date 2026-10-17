"""Paladin auras and blessings (service/paladin_auras.py) against CMaNGOS Classic @8ec338a and the 1.12.1 Spell.dbc.

Expected values are hand computed from the Spell.dbc rows (catalog built in memory: data.build_catalog.build) and the
cited CMaNGOS routines; the Druid's outputs are unchanged (test_the_druid_never_reaches_these_paths and the
before/after snapshot in the worker report)."""
import json
import unittest

from data.build_catalog import build
from service import character, formulas as F, paladin, paladin_auras as A, paladin_defense, skyrim, spells
from service.catalog import Catalog
from service.server import Service
from service.skyrim_balance import (COUNTERPARTS, KINGS_REFERENCE, MANA_AS_SPENT, PALADIN_AURAS, PALADIN_DEFENSE, SPELLS)

PALADIN_60 = {'class': 2, 'race': 1, 'level': 60}
FRIEND = {'kind': 'unit', 'level': 60, 'reaction': 'friendly', 'friendly': True, 'alive': True, 'distance_yd': 5,
          'los': True, 'party': True}
HOSTILE = {'kind': 'unit', 'level': 60, 'reaction': 'hostile', 'alive': True, 'distance_yd': 5, 'los': True}
NATIVE = {'alteration': 50, 'restoration': 50, 'destruction': 50, 'one_handed': 50}
# Talent allocations (tier N needs 5N points in its tree): Protection to Kings, Improved Concentration and Sanctuary;
# Retribution to Improved Retribution Aura and Sanctity; Holy to Improved Blessing of Wisdom.
PROTECTION = {'1422': 5, '1421': 5, '1630': 3, '1423': 5, '1425': 2, '1442': 1, '1626': 3, '1431': 1}
RETRIBUTION = {'1401': 5, '1407': 5, '1403': 5, '1411': 5, '1405': 2, '1409': 1}
HOLY = {'1450': 5, '1449': 5, '1432': 5, '1446': 2}
DEVOTION_7, RETRIBUTION_5, CONCENTRATION, SANCTITY = 10293, 10301, 19746, 20218
FIRE_3, FROST_3, SHADOW_3 = 19900, 19898, 19896
MIGHT_1, MIGHT_7, WISDOM_6, KINGS, SALVATION, LIGHT_3, SANCTUARY_4 = 19740, 25291, 25290, 20217, 1038, 19979, 20914
FREEDOM, SACRIFICE_1, SACRIFICE_2 = 1044, 6940, 20729
G_MIGHT_1, G_MIGHT_2, G_KINGS, G_LIGHT, G_SANCTUARY = 25782, 25916, 25898, 25890, 25899
HOLY_LIGHT_9, FLASH_6, EXORCISM_6, RIGHTEOUS_FURY, DIVINE_SHIELD_2 = 25292, 19943, 10314, 25780, 1020


class PaladinAuras(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog(raw=build())
        cls.shield = next(e for e, it in sorted(cls.cat.items.items()) if it['InventoryType'] == character.INVTYPE_SHIELD)
        cls.sword = next(e for e, it in sorted(cls.cat.items.items())
                         if it['class'] == 2 and it['InventoryType'] in character.INVTYPE_WEAPONS and it['subclass'] == 7)

    def setUp(self):
        self.svc = Service(self.cat, None)

    def ask(self, req):
        return self.svc.respond(json.dumps(req).encode())

    def ok(self, out):
        self.assertTrue(out['ok'], out)
        return out

    def cast(self, sid, target=None, seed=1, **caster):
        return self.ask({'op': 'resolve', 'spell': sid, 'seed': seed, 'caster': {**PALADIN_60, **caster},
                         'target': target or {'kind': 'self'}})

    def error(self, out):
        self.assertFalse(out['ok'], out)
        return out['error']

    def by_type(self, out, kind):
        return [a for a in out['auras'] if a['type'] == kind]

    # --- data -------------------------------------------------------------------------------------------------------

    def test_groups_are_the_cmangos_spell_groups(self):
        # sql/updates/mangos/z2828_01_mangos_spell_groups.sql: group 7 Blessing and 8 Aura, rule 2 (UNIQUE_PER_CASTER).
        self.assertEqual(paladin.groups()['Aura'][0], 'unique_per_caster')
        self.assertEqual(paladin.groups()['Blessing'][0], 'unique_per_caster')
        chains = {self.cat.chain(s) for s in A.auras()}
        self.assertEqual({self.cat.spell(c)['SpellName'] for c in chains},
                         {'Devotion Aura', 'Retribution Aura', 'Concentration Aura', 'Fire Resistance Aura',
                          'Frost Resistance Aura', 'Shadow Resistance Aura', 'Sanctity Aura'})
        for chain in chains:   # every rank of every aura is in the group
            self.assertLessEqual({s for s in self.cat.ranks if self.cat.chain(s) == chain}, A.auras())
        names = {self.cat.spell(s)['SpellName'] for s in A.blessings() if s in self.cat.spells}
        self.assertIn('Blessing of Protection', names)
        self.assertNotIn('Blessing of Freedom', names)   # Freedom is outside group 7
        self.assertEqual([s for s in self.cat.ranks if self.cat.spell(s)['SpellName'] == 'Blessing of Freedom'],
                         list(A.BLESSING_OF_FREEDOM))
        self.assertNotIn(27143, self.cat.spells)          # the group's TBC Greater Blessing of Wisdom rank 3
        self.assertEqual(A.light_spells(), {19977, 19978, 19979, G_LIGHT})
        # Every Holy Light rank heals through 19968, every Flash of Light rank through 19993 (manifest script_effect).
        for sid in self.cat.ranks:
            name = self.cat.spell(sid)['SpellName']
            if name in ('Holy Light', 'Flash of Light') and self.cat.spell(sid)['SpellFamilyName'] == 10:
                self.assertEqual(paladin.script_heal(sid),
                                 A.HOLY_LIGHT_HEAL if name == 'Holy Light' else A.FLASH_OF_LIGHT_HEAL, sid)
        greater = sorted(s for s in A.blessings() if s in self.cat.spells and A.is_greater(self.cat.spell(s)))
        self.assertEqual(greater, [25782, G_LIGHT, 25894, 25895, G_KINGS, G_SANCTUARY, G_MIGHT_2, 25918])
        for sid in greater:
            s = self.cat.spell(sid)
            self.assertEqual((s['Reagent1'], s['ReagentCount1'], self.cat.duration(s, 60)), (A.SYMBOL_OF_KINGS, 1, 900000))

    # --- auras --------------------------------------------------------------------------------------------------------

    def test_devotion_is_a_party_aura_on_her(self):
        out = self.ok(self.cast(DEVOTION_7))
        (armor,) = out['auras']
        self.assertEqual((armor['type'], armor['amount'], armor['school_mask'], armor['duration_ms']), ('armor', 735, 1, -1))
        self.assertEqual(armor['area'], {'radius_yd': 30.0, 'party': True, 'channel': False, 'membership_op': 'party_aura'})
        self.assertEqual(out['party_aura'], {'radius_yd': 30.0, 'membership_op': 'party_aura', 'stance_bar': True})
        self.assertEqual((out['cost'], out['gcd_ms'], out['stack'], out['stack_rule']), (0, 1500, 'Aura', 'unique_per_caster'))
        self.assertEqual(set(out['stack_spells']), set(A.auras()))
        # Improved Devotion Aura 5/5: SPELLMOD_ALL_EFFECTS +25% (int(735 x 1.25)).
        talented = self.ok(self.cast(DEVOTION_7, talents=PROTECTION))
        self.assertEqual(talented['auras'][0]['amount'], 918)

    def test_each_aura_effect(self):
        ret = self.ok(self.cast(RETRIBUTION_5))['auras'][0]
        self.assertEqual((ret['type'], ret['amount'], ret['school']), ('damage_shield', 20, 1))
        self.assertEqual(self.ok(self.cast(RETRIBUTION_5, talents=RETRIBUTION))['auras'][0]['amount'], 30)
        conc = self.ok(self.cast(CONCENTRATION))
        self.assertEqual([(a['type'], a['amount']) for a in conc['auras']],
                         [('pushback_immunity', 35), ('mechanic_resistance', 0), ('mechanic_resistance', 0)])
        self.assertEqual([(a.get('mechanic'), a.get('name')) for a in conc['auras'][1:]], [(26, 'interrupt'), (9, 'silence')])
        # Improved Concentration Aura 3/3: a flat +15 on every effect (SPELLMOD_ALL_EFFECTS).
        conc = self.ok(self.cast(CONCENTRATION, talents=PROTECTION))
        self.assertEqual([a['amount'] for a in conc['auras']], [50, 15, 15])
        for sid, mask in ((FIRE_3, 4), (FROST_3, 16), (SHADOW_3, 32)):
            (res,) = self.ok(self.cast(sid))['auras']
            self.assertEqual((res['type'], res['amount'], res['school_mask'], res['exclusive']), ('resistance', 60, mask, True))
        self.assertEqual(self.error(self.cast(SANCTITY, talents={})), 'SPELL_FAILED_NOT_KNOWN')   # a talent
        (sanctity,) = self.ok(self.cast(SANCTITY, talents=RETRIBUTION))['auras']
        self.assertEqual((sanctity['type'], sanctity['amount'], sanctity['school_mask']), ('damage_done_percent', 10, 2))

    def test_her_aura_holds_on_her_sheet(self):
        # The aura's party effects are hers too: a Fire Resistance Aura in caster.auras, with or without talents.
        for extra in ({}, {'talents': {}}):
            stats = self.ok(self.ask({'op': 'stats', 'caster': {**PALADIN_60, 'auras': [FIRE_3], **extra}}))
            self.assertEqual(stats['resist']['2'], 60)
        ch = character.build(self.cat, [], cls=2, race=1, auras=[SANCTITY], talents=RETRIBUTION)
        self.assertAlmostEqual(ch.multiplier(79, 2), 1.1)   # Holy damage done x1.1 (spells._damage_done)
        self.assertEqual(ch.multiplier(79, 4), 1.0)

    def test_a_new_aura_replaces_her_other(self):
        out = self.ok(self.cast(RETRIBUTION_5, auras=[DEVOTION_7]))
        self.assertEqual(out['replaced_auras'], [DEVOTION_7])
        # A caster-source aura never bounces (IsSpellWithCasterSourceTargetsOnly): a lower rank replaces a higher one.
        out = self.ok(self.cast(465, auras=[DEVOTION_7]))
        self.assertEqual((out['replaced_auras'], out['auras'][0]['amount']), ([DEVOTION_7], 55))

    def test_party_aura_membership(self):
        members = [{'id': 1, 'distance_yd': 10, 'level': 46}, {'id': 2, 'distance_yd': 30.5}, {'id': 3, 'distance_yd': 5, 'level': 45},
                   {'id': 4, 'distance_yd': 5, 'dead': True}, {'id': 5, 'distance_yd': 5, 'party': False},
                   {'id': 6, 'distance_yd': 30}, {'id': 7, 'distance_yd': 2, 'friendly': False}]
        caster = {**PALADIN_60, 'talents': RETRIBUTION, 'auras': [RETRIBUTION_5]}
        out = self.ok(self.ask({'op': 'party_aura', 'spell': RETRIBUTION_5, 'caster': caster, 'members': members}))
        self.assertEqual(out['radius_yd'], 30.0)
        self.assertEqual([(m['id'], m['in_aura'], m.get('reason')) for m in out['members']],
                         [(1, True, None), (2, False, 'out_of_range'), (3, False, 'low_level'), (4, False, 'dead'),
                          (5, False, 'not_party'), (6, True, None), (7, False, 'not_friendly')])
        # The members carry her aura: her Improved Retribution Aura amount, the damage shield's school.
        (shield,) = out['auras']
        self.assertEqual((shield['type'], shield['amount'], shield['school'], shield['spell'], shield['from_caster']),
                         ('damage_shield', 30, 1, RETRIBUTION_5, True))
        self.assertNotIn('area', shield)
        # She no longer has it: everyone loses it (AreaAura::Update, the caster's HasAura check).
        lost = self.ok(self.ask({'op': 'party_aura', 'spell': RETRIBUTION_5, 'caster': PALADIN_60, 'members': members[:1]}))
        self.assertEqual(lost['members'], [{'id': 1, 'in_aura': False, 'reason': 'caster_lost_aura'}])
        self.assertFalse(self.ask({'op': 'party_aura', 'spell': MIGHT_7, 'caster': PALADIN_60, 'members': []})['ok'])
        # Skyrim units: Thorns' rate for the shield, Mark's armor rate for Devotion.
        native = self.ok(self.ask({'op': 'party_aura', 'spell': RETRIBUTION_5, 'members': members[:1],
                                   'caster': {**caster, 'skyrim': NATIVE}}))
        self.assertEqual(native['auras'][0]['amount'], round(30 * 8 / 18))
        devotion = self.ok(self.ask({'op': 'party_aura', 'spell': DEVOTION_7, 'members': members[:1],
                                     'caster': {**PALADIN_60, 'auras': [DEVOTION_7], 'skyrim': NATIVE}}))
        self.assertEqual(devotion['auras'][0]['skyrim'], [{'mgef': 0xE5F55, 'magnitude': 735 * 40 / 285}])

    def test_low_level_follows_select_aura_rank_for_level(self):
        s = self.cat.spell(DEVOTION_7)                     # spell level 60
        self.assertFalse(A.too_low(self.cat, s, 50))
        self.assertTrue(A.too_low(self.cat, s, 49))
        self.assertFalse(A.too_low(self.cat, self.cat.spell(CONCENTRATION), 1))   # no ranks: GetSpellRank 0
        self.assertFalse(A.too_low(self.cat, self.cat.spell(KINGS), 1))

    # --- blessings ----------------------------------------------------------------------------------------------------

    def test_blessing_effects(self):
        might = self.ok(self.cast(MIGHT_7, FRIEND))
        self.assertEqual([(a['type'], a['amount'], a['duration_ms']) for a in might['auras']], [('attack_power', 185, 300000)])
        self.assertEqual((might['stack'], might['stack_rule']), ('Blessing', 'unique_per_caster'))
        self.assertEqual(self.ok(self.cast(MIGHT_7, FRIEND, talents=RETRIBUTION))['auras'][0]['amount'], 222)
        (wisdom,) = self.ok(self.cast(WISDOM_6, FRIEND))['auras']
        self.assertEqual((wisdom['type'], wisdom['per_tick'], wisdom['amplitude_ms'], len(wisdom['ticks'])),
                         ('power_restore', 33, 5000, 60))
        self.assertEqual(self.ok(self.cast(WISDOM_6, FRIEND, talents=HOLY))['auras'][0]['per_tick'], 39)
        (kings,) = self.ok(self.cast(KINGS, FRIEND, talents=PROTECTION))['auras']
        self.assertEqual((kings['type'], kings['amount'], kings['stat']), ('stat_percent', 10, -1))
        (salvation,) = self.ok(self.cast(SALVATION, FRIEND))['auras']
        self.assertEqual((salvation['type'], salvation['school_mask'], salvation['multiplier']), ('threat', 127, 0.7))
        light = self.ok(self.cast(LIGHT_3, FRIEND))['auras']
        self.assertEqual([(a['type'], a['amount'], a['linked_spell'], a['heals']) for a in light],
                         [('healing_taken', 400, 19968, 'Holy Light'), ('healing_taken', 115, 19993, 'Flash of Light')])
        sanctuary = self.ok(self.cast(SANCTUARY_4, FRIEND, talents=PROTECTION))['auras']
        self.assertEqual([(a['type'], a['amount']) for a in sanctuary], [('damage_taken', -24), ('block_damage', 35)])
        self.assertEqual((sanctuary[0]['school_mask'], sanctuary[1]['school'], sanctuary[1]['on']), (127, 1, 'block'))
        freedom = self.ok(self.cast(FREEDOM, FRIEND))
        self.assertEqual([(a['type'], a['mechanic'], a['name'], a['purges_existing'], a['duration_ms']) for a in freedom['auras']],
                         [('mechanic_immunity', 7, 'root', True, 10000), ('mechanic_immunity', 11, 'snare', True, 10000)])
        self.assertEqual(freedom['immunity'], {'mechanics': [7, 11], 'names': ['root', 'snare'], 'duration_ms': 10000,
                                               'purges_existing': True})
        self.assertEqual(freedom['cooldown_ms'], 20000)
        self.assertNotIn('stack_rule', freedom)
        sacrifice = self.ok(self.cast(SACRIFICE_2, FRIEND))
        self.assertEqual([(a['type'], a['amount'], a['duration_ms']) for a in sacrifice['auras']], [('damage_transfer', 55, 30000)])
        self.assertEqual(sacrifice['transfer'], {'per_hit': 55, 'school_mask': 127, 'to': 'caster', 'event_op': 'party_hit'})

    def test_one_blessing_of_hers_per_unit(self):
        # Another type replaces hers (group 7, personal): Kings over her Might.
        out = self.ok(self.cast(KINGS, {**FRIEND, 'auras': [MIGHT_7]}, talents=PROTECTION))
        self.assertEqual(out['replaced_auras'], [MIGHT_7])
        self.assertEqual(self.ok(self.cast(SALVATION, {**FRIEND, 'auras': [SACRIFICE_2, 9885]}))['replaced_auras'], [SACRIFICE_2])
        # A weaker or lower rank of the same bounces (Spell::CheckCast): Might 1 on Might 7, Greater Might 1 (155) on
        # Might 7 (185); an equal one replaces it (Might 7 on Greater Might 2, both 185).
        self.assertEqual(self.error(self.cast(MIGHT_1, {**FRIEND, 'auras': [MIGHT_7]})), 'SPELL_FAILED_AURA_BOUNCED')
        self.assertEqual(self.error(self.cast(G_MIGHT_1, {**FRIEND, 'auras': [MIGHT_7]}, reagents={'21177': 1})),
                         'SPELL_FAILED_AURA_BOUNCED')
        self.assertEqual(self.ok(self.cast(MIGHT_7, {**FRIEND, 'auras': [G_MIGHT_2]}))['replaced_auras'], [G_MIGHT_2])
        # On herself it does not fail; it just does not land (Unit::RemoveNoStackAurasDueToAuraHolder returns false).
        self_cast = self.ok(self.cast(MIGHT_1, auras=[MIGHT_7]))
        self.assertEqual((self_cast['bounced_by'], self_cast['auras']), (MIGHT_7, []))
        # Freedom stays beside her blessing, and Blessing of Protection replaces it (group 7).
        self.assertNotIn('replaced_auras', self.ok(self.cast(FREEDOM, {**FRIEND, 'auras': [MIGHT_7]})))
        bop = self.ok(self.cast(10278, {**FRIEND, 'auras': [MIGHT_7]}))
        self.assertEqual((bop['stack'], bop['replaced_auras']), ('Blessing', [MIGHT_7]))
        self.assertTrue(self.by_type(bop, 'school_immunity'))

    def test_blessing_targets(self):
        outsider = {**FRIEND, 'party': False}
        self.assertEqual(self.error(self.cast(SALVATION, outsider)), 'SPELL_FAILED_BAD_TARGETS')   # TARGET_UNIT_RAID
        self.assertEqual(self.error(self.cast(SACRIFICE_2)), 'SPELL_FAILED_BAD_TARGETS')          # EXCLUDE_CASTER
        self.ok(self.cast(SACRIFICE_2, FRIEND))
        self.ok(self.cast(MIGHT_7, outsider))                                                      # any friend
        self.assertEqual(self.error(self.cast(MIGHT_7, HOSTILE)), 'SPELL_FAILED_BAD_TARGETS')
        # SelectAuraRankForLevel: rank 7 (level 60) needs level 50; Kings has no ranks.
        self.assertEqual(self.error(self.cast(MIGHT_7, {**FRIEND, 'level': 49})), 'SPELL_FAILED_LOWLEVEL')
        self.ok(self.cast(MIGHT_7, {**FRIEND, 'level': 50}))
        self.ok(self.cast(KINGS, {**FRIEND, 'level': 1}, talents=PROTECTION))
        # Greater Blessings: a party member, a Symbol of Kings, 15 minutes, the class targets pending D3.
        self.assertEqual(self.error(self.cast(G_KINGS, FRIEND)), 'SPELL_FAILED_REAGENTS')
        self.assertEqual(self.error(self.cast(G_KINGS, outsider, reagents={'21177': 1})), 'SPELL_FAILED_BAD_TARGETS')
        out = self.ok(self.cast(G_KINGS, FRIEND, reagents={'21177': 1}))
        self.assertEqual(out['reagents'], [{'item': A.SYMBOL_OF_KINGS, 'count': 1}])
        self.assertEqual(out['greater_blessing'], {'applies_to': 'party', 'status': 'party_as_one_class',
                                                   'class_radius_yd': 100.0})
        self.assertEqual((out['auras'][0]['type'], out['auras'][0]['duration_ms'], out['cost']), ('stat_percent', 900000, 150))
        self.assertNotIn('area', out['auras'][0])
        self.ok(self.cast(G_KINGS, reagents={'21177': 1}))                                         # herself

    def test_her_blessings_on_her_sheet(self):
        bare = self.ok(self.ask({'op': 'stats', 'caster': PALADIN_60}))
        kings = self.ok(self.ask({'op': 'stats', 'caster': {**PALADIN_60, 'auras': [KINGS]}}))
        self.assertEqual(bare['stats'], {'str': 105, 'agi': 65, 'sta': 100, 'int': 70, 'spi': 81})
        self.assertEqual({k: kings['stats'][k] - bare['stats'][k] for k in bare['stats']}, KINGS_REFERENCE)
        might = self.ok(self.ask({'op': 'stats', 'caster': {**PALADIN_60, 'auras': [MIGHT_7]}}))
        self.assertEqual(might['attack_power'] - bare['attack_power'], 185)

    def test_blessing_of_light_on_her_holy_light(self):
        crits = set()
        for sid, linked, bonus in ((HOLY_LIGHT_9, 19968, 400), (FLASH_6, 19993, 115)):
            for seed in range(1, 13):
                plain = self.ok(self.cast(sid, FRIEND, seed=seed))['events'][0]
                for blessing in (LIGHT_3, G_LIGHT):
                    blessed = self.ok(self.cast(sid, {**FRIEND, 'auras': [blessing]}, seed=seed))['events'][0]
                    self.assertEqual((blessed['spell'], blessed['crit'], blessed['taken_spell']), (linked, plain['crit'], blessing))
                    # Taken after her healing done, before the crit (x1.5; uint32 truncation either side).
                    if plain['crit']:
                        self.assertLessEqual(abs(blessed['amount'] - (plain['amount'] + bonus * 1.5)), 1, (sid, seed))
                    else:
                        self.assertEqual(blessed['amount'], plain['amount'] + bonus, (sid, seed))
                    crits.add(plain['crit'])
            # A self cast reads her own auras; her Holy Light on a unit without it is unchanged.
            self.assertEqual(self.ok(self.cast(sid, seed=3, auras=[LIGHT_3]))['events'][0]['taken_spell'], LIGHT_3)
        self.assertEqual(crits, {True, False})
        # The oracle (custom rules off) leaves CMaNGOS's empty affect mask: no bonus.
        ch = character.build(self.cat, [], cls=2, race=1)
        ch.custom_rules = False
        self.assertEqual(A.light_bonus(self.cat, ch, {'target': {**FRIEND, 'auras': [LIGHT_3]}}, 19968), (0, None))

    def test_blessing_of_light_converts_at_the_top_ranks_rate(self):
        # Native lower ranks convert her additions (gear, talents) at the top rank's rate (spells._rank_health_bonus);
        # Blessing of Light's healing taken is one, so every rank gains what the top rank gains: no downrank gain (rank 1
        # Holy Light used to convert the +400 at its own rate, about +100 health against +22 at rank 9).
        for chain, top, bonus in ((635, HOLY_LIGHT_9, 400), (19750, FLASH_6, 115)):
            rate = self.cat.balance[top].health_factor
            ranks = sorted(s for s in self.cat.ranks if self.cat.chain(s) == chain)
            self.assertEqual(len(ranks), 9 if chain == 635 else 6)
            for sid in ranks:
                seen = 0
                for seed in range(1, 7):
                    plain = self.ok(self.cast(sid, FRIEND, seed=seed, skyrim=NATIVE))['events'][0]
                    blessed = self.ok(self.cast(sid, {**FRIEND, 'auras': [LIGHT_3]}, seed=seed, skyrim=NATIVE))['events'][0]
                    self.assertEqual((blessed['taken_bonus_wow'], blessed['crit']), (bonus, plain['crit']))
                    if not plain['crit']:
                        self.assertLessEqual(abs(blessed['amount'] - plain['amount'] - bonus * rate), 1, (sid, seed))
                        seen += 1
                self.assertTrue(seen, sid)

    def test_salvation_lowers_her_threat(self):
        undead = {**HOSTILE, 'creature_type': 'Undead', 'distance_yd': 20}
        exorcism = self.ok(self.cast(EXORCISM_6, undead, auras=[SALVATION]))
        self.assertAlmostEqual(exorcism['threat_multiplier'], 0.7)
        both = self.ok(self.cast(EXORCISM_6, undead, auras=[SALVATION, RIGHTEOUS_FURY]))
        self.assertAlmostEqual(both['threat_multiplier'], 0.7 * 1.6)
        swing = self.ok(self.ask({'op': 'melee', 'seed': 2, 'caster': {**PALADIN_60, 'items': [self.sword], 'auras': [SALVATION]},
                                  'target': {**HOSTILE, 'distance_yd': 2}}))
        self.assertAlmostEqual(swing['threat_multiplier'], 0.7)
        plain = self.ok(self.ask({'op': 'melee', 'seed': 2, 'caster': {**PALADIN_60, 'items': [self.sword]},
                                  'target': {**HOSTILE, 'distance_yd': 2}}))
        self.assertNotIn('threat_multiplier', plain)

    # --- Sanctuary and Sacrifice on a hit -------------------------------------------------------------------------------

    def hit(self, seed, caster, damage=600, **attack):
        return self.ok(self.ask({'op': 'incoming_hit', 'weapon_hit': True, 'seed': seed, 'caster': caster,
                                 'target': HOSTILE, 'attack': {'damage': damage, **attack}}))

    def test_sanctuary_on_her(self):
        caster = {**PALADIN_60, 'items': [self.shield, self.sword], 'auras': [SANCTUARY_4]}
        seen = set()
        for seed in range(80):
            d = self.hit(seed, caster)['defense']
            seen.add(d['outcome'])
            self.assertEqual(d['damage_reduced'], 24)
            if d['outcome'] == 'hit':
                self.assertEqual(d['damage'], 576)
            elif d['outcome'] == 'crit':
                self.assertEqual(d['damage'], 576 * 2)      # off the hit before the crit (CalculateMeleeDamage)
            elif d['outcome'] == 'block':
                procs = [p for p in self.hit(seed, caster)['procs'] if p['spell'] == SANCTUARY_4]
                self.assertEqual(procs[0]['events'][0]['amount'], 35)
        self.assertLessEqual({'hit', 'crit', 'block'}, seen)
        native = self.hit(0, {**caster, 'skyrim': NATIVE}, damage=50)['defense']
        self.assertEqual(native['damage_reduced'], round(24 * paladin_defense.block_health_factor(), 2))
        immune = self.hit(0, {**caster, 'auras': [SANCTUARY_4, DIVINE_SHIELD_2]})['defense']
        self.assertNotIn('damage_reduced', immune)

    def party_hit(self, auras, caster=None, ctx=None, **attack):
        return self.ok(self.ask({'op': 'party_hit', 'seed': 5, 'caster': caster or PALADIN_60, 'target': {'level': 60},
                                 'member': {'id': 9, 'auras': auras}, 'attack': {'damage': 600, **attack},
                                 **({'ctx': ctx} if ctx else {})}))

    def test_party_hit(self):
        out = self.party_hit([SANCTUARY_4], outcome='hit')
        self.assertEqual((out['member_damage'], out['events']), (576, [{'type': 'damage_reduced', 'spell': SANCTUARY_4, 'amount': 24}]))
        self.assertNotIn('procs', out)
        block = self.party_hit([SANCTUARY_4], outcome='block')
        self.assertEqual([(p['source_spell'], p['target'], p['events'][0]['amount'], p['events'][0]['school'])
                          for p in block['procs']], [(SANCTUARY_4, 'attacker', 35, 1)])
        self.assertNotIn('procs', self.party_hit([SANCTUARY_4], kind='ranged', outcome='block'))
        self.assertEqual(self.party_hit([SANCTUARY_4], school=4, kind='spell')['member_damage'], 576)   # every school
        sac = self.party_hit([SACRIFICE_2], school=2, kind='spell')
        self.assertEqual((sac['member_damage'], sac['events']),
                         (545, [{'type': 'damage_transfer', 'spell': SACRIFICE_2, 'amount': 55, 'school': 2, 'to': 'caster', 'from': 9}]))
        self.assertEqual(self.party_hit([SACRIFICE_1], damage=30)['events'][0]['amount'], 30)        # up to the hit
        self.assertEqual(self.party_hit([SACRIFICE_1])['events'][0]['amount'], 45)
        for ctx in ({'caster_alive': False}, {'self_damage': True}):
            self.assertEqual((self.party_hit([SACRIFICE_2], ctx=ctx)['member_damage']), 600)
        immune = self.party_hit([SACRIFICE_2], caster={**PALADIN_60, 'auras': [DIVINE_SHIELD_2]})
        self.assertEqual((immune['member_damage'], immune['events'][0]['amount'], immune['events'][0]['caster_immune']),
                         (600, 0, True))
        # Skyrim units: the hit's own, the flat amounts at the white-hit scale, the proc at Sanctuary's rule.
        factor = paladin_defense.block_health_factor()
        native = self.party_hit([SACRIFICE_2], caster={**PALADIN_60, 'skyrim': NATIVE}, damage=50)
        self.assertEqual((native['events'][0]['amount'], native['member_damage']),
                         (round(55 * factor, 2), round(50 - 55 * factor, 2)))
        block = self.party_hit([SANCTUARY_4], caster={**PALADIN_60, 'skyrim': NATIVE}, damage=50, outcome='block')
        self.assertEqual(block['procs'][0]['events'][0]['amount'], round(35 * PALADIN_DEFENSE[20928].health_factor))
        self.assertFalse(self.ask({'op': 'party_hit', 'caster': PALADIN_60, 'member': {'id': 1, 'auras': ['x']},
                                   'attack': {'damage': 1}})['ok'])

    # --- balance ------------------------------------------------------------------------------------------------------

    def test_balance_anchors_and_rates(self):
        trainable = {'Destruction', 'Restoration', 'Alteration'}
        ch = character.build(self.cat, [], cls=2, race=1)
        for sid, rule in PALADIN_AURAS.items():
            s = self.cat.spell(sid)
            with self.subTest(spell=rule.name):
                self.assertEqual(s['SpellName'], rule.name)
                self.assertIn(rule.skill, trainable)
                self.assertEqual(max(spells._cost(s, ch), 1), rule.wow_cost)
                self.assertEqual(max((self.cat.ranks[x]['rank'], x) for x in self.cat.ranks
                                     if self.cat.chain(x) == self.cat.chain(sid))[1], sid)
                for rank in (x for x in self.cat.ranks if self.cat.chain(x) == self.cat.chain(sid)):
                    self.assertEqual(self.cat.balance[rank].skill, rule.skill)
                    self.assertNotIn('fallback', self.cat.balance[rank].reason)
                self.assertEqual(rule.cost == (), s['ManaCost'] == 0 and s['ManaCostPercentage'] == 0)
        covered = {self.cat.chain(s) for s in PALADIN_AURAS}
        self.assertEqual(covered, {self.cat.chain(s) for s in A.auras() | A.blessings() | set(A.BLESSING_OF_FREEDOM)
                                   if s in self.cat.spells} - {self.cat.chain(10278)})   # Protection: PALADIN_DEFENSE
        # Retribution plays as Thorns, Sanctuary's block damage as Holy Shield, a blessing costs Mark's magicka per mana.
        self.assertAlmostEqual(self.cat.balance[RETRIBUTION_5].health_factor, SPELLS[9910].health_factor)
        self.assertAlmostEqual(self.cat.balance[SANCTUARY_4].health_factor, PALADIN_DEFENSE[20928].health_factor)
        self.assertAlmostEqual(self.cat.balance[MIGHT_7].base_cost, 103 * 130 / 445)
        self.assertEqual(self.cat.balance[MIGHT_1].health, ())

    def test_native_magnitudes(self):
        caster = {'skyrim': NATIVE}
        might = self.ok(self.cast(MIGHT_7, FRIEND, **caster))
        self.assertEqual(might['skill'], 'Alteration')
        self.assertEqual(might['cost'], max(1, round(103 * 130 / 445 * skyrim.cost_factor(50))))
        self.assertEqual(might['auras'][0]['skyrim'], [{'mgef': 0x7A0FF, 'magnitude': 18.5}, {'mgef': 0x7A106, 'magnitude': 18.5}])
        costs = self.ok(self.ask({'op': 'skyrim_costs', 'caster': {**PALADIN_60, **caster}}))['costs']
        self.assertEqual((costs[str(MIGHT_7)], costs[str(DEVOTION_7)]), (might['cost'], 0))
        (wisdom,) = self.ok(self.cast(WISDOM_6, FRIEND, **caster))['auras']
        rate = sum(COUNTERPARTS[n]['magnitude'] * u / r for n, u, r in MANA_AS_SPENT)
        self.assertEqual((wisdom['per_tick'], wisdom['ticks'][0]['amount']), (round(33 * rate), round(33 * rate)))
        (kings,) = self.ok(self.cast(KINGS, FRIEND, talents=PROTECTION, **caster))['auras']
        self.assertEqual({e['mgef']: round(e['magnitude'], 3) for e in kings['skyrim']},
                         {0x493AA: round(10 * 20 / 12, 3), 0x49504: round(7 * 20 / 12, 3), 0x49507: 10.0, 0x7A0F4: 10.0,
                          0x7A0FF: 5.0, 0x7A106: 5.0, 0x7A0FD: 9.0})
        (devotion,) = self.ok(self.cast(DEVOTION_7, **caster))['auras']
        self.assertEqual(devotion['skyrim'], [{'mgef': 0xE5F55, 'magnitude': 735 * 40 / 285}])
        (shadow,) = self.ok(self.cast(SHADOW_3, **caster))['auras']
        self.assertEqual(shadow['skyrim'], [{'mgef': 0xB7A35, 'magnitude': 20.0}])
        (ret,) = self.ok(self.cast(RETRIBUTION_5, **caster))['auras']
        self.assertEqual(ret['amount'], round(20 * 8 / 18))
        sanctuary = self.ok(self.cast(SANCTUARY_4, FRIEND, talents=PROTECTION, **caster))['auras']
        self.assertEqual((sanctuary[0]['native_per_hit'], sanctuary[1]['native_amount']),
                         (round(-24 * paladin_defense.block_health_factor(), 2), round(35 * 50 / 3 / 130)))
        self.assertEqual(self.ok(self.cast(DEVOTION_7, **caster))['cost'], 0)
        # Holy Light on a blessed unit converts with the heal (the bonus is WoW healing).
        wow = self.ok(self.cast(HOLY_LIGHT_9, {**FRIEND, 'auras': [LIGHT_3]}, seed=2))['events'][0]['amount']
        native = self.ok(self.cast(HOLY_LIGHT_9, {**FRIEND, 'auras': [LIGHT_3]}, seed=2, **caster))['events'][0]['amount']
        self.assertEqual(native, round(wow * self.cat.balance[HOLY_LIGHT_9].health_factor))

    # --- the Druid ------------------------------------------------------------------------------------------------------

    def test_the_druid_never_reaches_these_paths(self):
        druid_book = [sid for sid in self.cat.class_spells if self.cat.spell(sid)['SpellFamilyName'] == 7]
        self.assertFalse(any(A.handles(sid) for sid in druid_book))
        # No Druid spell uses TARGET_UNIT_RAID_AND_CLASS, the target spells.is_positive now counts positive.
        self.assertFalse(any(A.is_greater(self.cat.spell(sid)) for sid in druid_book))
        druid = {'level': 60, 'auras': [9885]}
        swing = self.ok(self.ask({'op': 'melee', 'seed': 2, 'caster': druid, 'target': {**HOSTILE, 'distance_yd': 2}}))
        self.assertNotIn('threat_multiplier', swing)
        mark = self.ok(self.ask({'op': 'resolve', 'spell': 9885, 'seed': 1, 'caster': druid, 'target': {'kind': 'self'}}))
        self.assertFalse({'stack_rule', 'stack_spells', 'replaced_auras', 'party_aura'} & set(mark))
        self.assertEqual(mark['stack'], spells.stack_group(self.cat.spell(9885)))


if __name__ == '__main__':
    unittest.main()
