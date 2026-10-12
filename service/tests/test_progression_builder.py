"""Progression provenance and trainer levels cross-checked with the full dump."""
import json
import hashlib
import unittest

from data import dbc
from data.build_catalog import DBC_DIR
from data.build_progression import (CONFIG, DEFAULT_DUMP, DEFAULT_OUT, DUMP_COLUMNS,
                                    build_class, rank_chains, sha256)
from data.sql_dump import tables


class BuilderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Required reference inputs are deliberate failures if unavailable.
        cls.sql = tables(DEFAULT_DUMP, DUMP_COLUMNS)
        cls.abilities = dbc.read(DBC_DIR / 'SkillLineAbility.dbc')
        cls.lines = dbc.by_id(dbc.read(DBC_DIR / 'SkillLine.dbc'))
        cls.talents = dbc.read(DBC_DIR / 'Talent.dbc')
        cls.skill_race_info = dbc.read(DBC_DIR / 'SkillRaceClassInfo.dbc')
        cls.chains = rank_chains(cls.sql, cls.abilities, cls.talents)
        cls.talent_spells = {s for t in cls.talents for s in t['SpellRank'] if s}
        cls.data = json.loads(DEFAULT_OUT.read_text())
        cls.druid = cls.data['classes']['druid']
        cls.spells = {s['spell']: s for s in cls.druid['spells']}
        cls.excluded = {s['spell']: s for s in cls.druid['excluded']}

    def test_checked_in_data_matches_builder_and_inputs(self):
        definitions = json.loads(CONFIG.read_text())['classes']
        for key, definition in definitions.items():
            with self.subTest(class_name=key):
                self.assertEqual(self.data['classes'][key], build_class(key, definition, self.sql, self.abilities,
                                                                      self.lines, self.talent_spells, self.chains,
                                                                      self.skill_race_info))
        self.assertEqual(self.data['provenance']['dump_sha256'], sha256(DEFAULT_DUMP))
        self.assertEqual(self.data['provenance']['mapping_sha256'], sha256(CONFIG))
        for name, digest in self.data['provenance']['dbc_sha256'].items():
            self.assertEqual(digest, sha256(DBC_DIR / f'{name}.dbc'))

    def test_druid_definition_unchanged_from_original_head(self):
        # Canonical class object at e05068c1f3a51a1a5875a1730c40636d91cf4931.
        digest = hashlib.sha256(json.dumps(self.druid, sort_keys=True).encode()).hexdigest()
        self.assertEqual(digest, '151847accdfb901e8915122577a9dfa1ada986cc427e677c22b7d3c488e5e960')

    def test_reference_levels_and_healing_touch_xi_is_a_book(self):
        wrath = self.spells[5176]
        self.assertEqual((wrath['source'], wrath['required_level'], wrath['rank']), ('starting', 1, 1))
        self.assertTrue(any(r['class'] == 11 and r['Spell'] == 5176 for r in self.sql['playercreateinfo_spell']))
        hurricane = self.spells[16914]
        self.assertEqual((hurricane['required_level'], hurricane['rank']), (40, 1))
        self.assertTrue(any(r['spell'] == 24922 and r['reqlevel'] == 40 for r in self.sql['npc_trainer_template']))
        book = self.excluded[25297]
        self.assertEqual((book['source'], book['required_level'], book['rank']), ('item', 60, 11))
        self.assertIn(21294, book['items'])
        self.assertTrue(any(i['entry'] == 21294 and i['RequiredLevel'] == 60 and i['spellid_1'] == 25970
                            for i in self.sql['item_template']))
        wrappers = {s['Id'] for s in self.sql['spell_template'] if s['Id'] == 25297 or any(
            s[f'Effect{i}'] == 36 and s[f'EffectTriggerSpell{i}'] == 25297 for i in (1, 2, 3))}
        self.assertFalse(any(r['spell'] in wrappers for table in ('npc_trainer', 'npc_trainer_template') for r in self.sql[table]))

    def test_feral_dbc_chains_and_quest_dependencies(self):
        self.assertEqual((self.spells[9881]['chain'], self.spells[9881]['rank']), (6807, 7))
        self.assertEqual((self.spells[9908]['chain'], self.spells[9908]['rank']), (779, 5))
        self.assertEqual((self.spells[9634]['chain'], self.spells[9634]['previous_spell']), (5487, 5487))
        self.assertEqual((self.excluded[5487]['source'], self.excluded[5487]['required_level']), ('quest', 10))
        self.assertEqual((self.excluded[1066]['source'], self.excluded[1066]['required_level']), ('quest', 16))
        for sid in (16810, 17390, 24974):
            self.assertEqual(self.excluded[sid]['source'], 'talent')

    def test_every_trainer_option_has_dump_evidence(self):
        lookup = {table: {(r['entry'], r['spell']): r for r in self.sql[table]}
                  for table in ('npc_trainer', 'npc_trainer_template')}
        for key, definition in self.data['classes'].items():
            for row in definition['spells'] + definition['excluded']:
                for option in row.get('trainer_options', []):
                    for entry in option['entries']:
                        with self.subTest(class_name=key, spell=row['spell'], entry=entry):
                            trainer = lookup[option['table']][entry, option['trainer_spell']]
                            self.assertEqual((option['reqlevel'], option['req_skill'], option['req_skill_value']),
                                             (trainer['reqlevel'], trainer['reqskill'], trainer['reqskillvalue']))

    def test_shared_wrapper_pet_trainers_and_skill_acquisition(self):
        paladin = {r['spell']: r for r in self.data['classes']['paladin']['spells']}
        for sid in (20271, 21084):  # Wrapper 10321 teaches Judgement AND the seal.
            self.assertTrue(any(o['trainer_spell'] == 10321 and o['required_level'] == 4
                                for o in paladin[sid]['trainer_options']))
        hunter = {r['spell']: r for r in self.data['classes']['hunter']['spells']}
        pet_trainers = {r['Entry'] for r in self.sql['creature_template']
                        if r['TrainerType'] == 3 and r['TrainerClass'] == 3 and r['NpcFlags'] & 16}
        self.assertTrue(any(set(o['entries']) & pet_trainers for o in hunter[1853]['trainer_options']
                            if o['table'] == 'npc_trainer'))
        self.assertEqual(hunter[1853]['required_spell'], 5149)
        rogue = {r['spell']: r for r in self.data['classes']['rogue']['excluded']}
        instant = rogue[8681]
        self.assertEqual((instant['source'], instant['required_spell']), ('skill', 2842))
        self.assertTrue(any(a['ID'] in instant['skill_line_abilities'] and a['Spell'] == 8681 and
                            a['AcquireMethod'] == 1 for a in self.abilities))
        priest = {r['spell']: r for r in self.data['classes']['priest']['excluded']}
        self.assertEqual((priest[13908]['source'], priest[13908]['race_mask']), ('racial', 5))
        # Triggered racial damage effects must not become player book entries.
        self.assertNotIn(28377, priest)

    def test_builder_accepts_another_class_definition(self):
        warrior = {'id': 1, 'name': 'Warrior', 'default_skill': 'alteration',
                   'lines': {'26': {'skill': 'two_handed', 'tab': 1},
                             '256': {'skill': 'one_handed', 'tab': 2},
                             '257': {'skill': 'alteration', 'tab': 3}}}
        built = build_class('warrior', warrior, self.sql, self.abilities, self.lines, self.talent_spells, self.chains, self.skill_race_info)
        self.assertGreater(built['counts']['trainer_ranks'], 0)
        self.assertEqual(built['id'], 1)
        self.assertTrue(any(s['skill'] == 'two_handed' for s in built['spells']))
        self.assertFalse(any(s['spell'] == 5176 for s in built['spells']))

    def test_zero_reqlevel_uses_trainer_spell_and_class_gate(self):
        # Do not substitute the learned spell's level for the core fallback.
        definition = json.loads(CONFIG.read_text())['classes']['druid']
        sql = {**self.sql, 'npc_trainer': [], 'npc_trainer_template': [
            {**r, 'reqlevel': 0} for r in self.sql['npc_trainer_template'] if r['spell'] == 499]}
        built = build_class('druid', definition, sql, self.abilities, self.lines,
                            self.talent_spells, self.chains, self.skill_race_info)
        cat = next(s for s in built['spells'] if s['spell'] == 768)
        self.assertEqual(cat['required_level'], 10)  # Feral class gate, wrapper spellLevel=0
