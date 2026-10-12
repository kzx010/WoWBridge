"""Acquisition snapshots, rank replacement, independent lines, and class reuse."""
import copy
import json
import unittest

from service.catalog import Catalog
from service.progression import Progression, wow_level, skill_value
from service.server import Service
from service.skyrim import SKILLS

MAX_SKILLS = dict.fromkeys(SKILLS.values(), 100)

# Spell IDs and counts are reference expectations from the 1.12 dump, rather
# than a second implementation of the eligibility algorithm.
CLASS_BOOKS = {
    'warrior': (1, {78, 2457, 6673}, 93, {'26': 11567, '256': 11556, '257': 11597}),
    'paladin': (2, {635, 20154, 465}, 122, {'594': 10329, '267': 10293, '184': 19838}),
    'hunter': (3, {75, 2973, 1494}, 161, {'50': 13544, '163': 14325, '51': 14266, '261': 14927}),
    'rogue': (4, {2098, 1752, 21184, 1784}, 107,
              {'253': 11300, '38': 11294, '39': 1787, '40': 11343, '633': 1804}),
    'priest': (5, {585, 2050, 1243}, 125, {'613': 10901, '56': 10965, '78': 10947}),
    'shaman': (7, {403, 331, 8017}, 160, {'375': 15208, '373': 16316, '374': 10396}),
    'mage': (8, {133, 168, 1459}, 136, {'237': 10157, '8': 10151, '6': 10181}),
    'warlock': (9, {686, 687, 348, 688}, 142, {'355': 11672, '354': 11735, '593': 11661}),
    'druid': (11, {5176, 5185, 1126}, 175, {'574': 9912, '134': 9850, '573': 9889}),
}


class ProgressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.progression = Progression()
        cls.service = Service(Catalog(), None, cls.progression)

    def ask(self, skills=None, **fields):
        return self.service.handle({'op': 'known_spells', 'class': 'druid',
                                    'caster': {'skyrim': skills or {}}, **fields})

    def test_new_character(self):
        reply = self.ask()
        self.assertEqual(set(reply['known_spells']), {5176, 5185, 1126})
        self.assertEqual(reply['newly_learned'], reply['known_spells'])
        self.assertEqual(reply['highest_ranks'], {'5176': 5176, '5185': 5185, '1126': 1126})
        self.assertEqual(set(reply['wow_levels'].values()), {1})

    def test_mid_character_and_delta(self):
        new = self.ask()['known_spells']
        reply = self.ask(dict.fromkeys(MAX_SKILLS, 50), since=new)
        self.assertEqual(set(reply['wow_levels'].values()), {25})
        self.assertEqual(reply['highest_ranks']['5176'], 5179)  # Wrath 4, level 22
        self.assertEqual(reply['highest_ranks']['5185'], 5188)  # HT 4, level 20
        self.assertEqual(reply['highest_ranks']['8921'], 8926)  # Moonfire 4, level 22
        self.assertNotIn(16914, reply['known_spells'])  # Hurricane, level 40
        self.assertEqual(set(reply['newly_learned']), set(reply['known_spells']) - set(new))
        self.assertIn({'chain': 5176, 'old_spell': 5176, 'new_spell': 5179}, reply['replacements'])
        repeat = self.ask(dict.fromkeys(MAX_SKILLS, 50), since=reply['known_spells'])
        self.assertEqual((repeat['newly_learned'], repeat['replacements']), ([], []))

    def test_unlearned_ranks_and_inverse_curve(self):
        reply = self.ask()
        pending = {s['spell']: s for s in reply['unlearned_spells']}
        druid = self.progression.data['classes']['druid']
        self.assertEqual(set(pending) | set(reply['known_spells']),   # trainer ranks and the class quests' (auto_sources)
                         {s['spell'] for s in druid['spells'] + [r for r in druid['excluded'] if r['source'] == 'quest']})
        self.assertFalse(set(pending) & set(reply['known_spells']))
        hurricane = pending[16914]
        self.assertEqual(hurricane['learn_skill'], 'Destruction')
        self.assertEqual(hurricane['learn_skill_value'], 72)   # 15 + 39 * 85 / 59, whole
        self.assertEqual(pending[768]['learn_skill'], 'Alteration')
        self.assertEqual(pending[6808]['learn_skill'], 'One-handed')
        self.assertIn('Requires Maul (Rank 1) (quest)', pending[6808]['learn_requirements'])
        self.assertNotIn(25297, pending)  # item/talent-only ranks are omitted until earned
        self.assertEqual(pending[5487]['learn_skill'], 'Alteration')   # a class quest's: learned at its level
        self.assertNotIn(16857, pending)
        for row in pending.values():
            self.assertGreaterEqual(wow_level(row['learn_skill_value'], self.progression.curve), row['required_level'])
        at_unlock = self.ask({'destruction': hurricane['learn_skill_value']})
        self.assertIn(16914, at_unlock['known_spells'])
        self.assertNotIn(16914, {s['spell'] for s in at_unlock['unlearned_spells']})
        acquired = self.ask(acquired_spells=[5487])
        self.assertIn(5487, acquired['known_spells'])
        curve = dict(skill_min=10, skill_max=90, level_min=2, level_max=42)
        self.assertEqual(skill_value(22, curve), 50)
        self.assertIsNone(skill_value(43, curve))

    def test_independent_lines_and_form_override(self):
        reply = self.ask({'destruction': 100})
        self.assertEqual(reply['highest_ranks']['5176'], 9912)
        self.assertEqual(reply['highest_ranks']['5185'], 5185)
        self.assertNotIn(768, reply['known_spells'])
        self.assertEqual(reply['lines']['574']['wow_level'], 60)
        self.assertEqual(reply['lines']['573']['wow_level'], 1)
        forms = self.ask({'alteration': 100})
        self.assertIn(768, forms['known_spells'])
        self.assertNotIn(1082, forms['known_spells'])  # Claw uses One-handed
        self.assertEqual(forms['highest_ranks']['1126'], 9885)  # utility in Restoration tab
        cat = next(s for s in forms['spells'] if s['spell'] == 768)
        self.assertEqual((cat['skill'], cat['skill_line'], cat['tab'], cat['wow_level']), ('alteration', 134, 2, 60))

    def test_master_character_and_external_acquisition(self):
        reply = self.ask(MAX_SKILLS)
        self.assertEqual(len(reply['known_spells']), 175)   # 163 trainer ranks + 12 class-quest ranks
        self.assertEqual(reply['highest_ranks']['16914'], 17402)
        self.assertEqual(reply['highest_ranks']['5185'], 9889)  # trainer max, not book XI
        for sid in (5487, 1066, 8946, 6808, 9634):   # the class quests' and what follows them
            self.assertIn(sid, reply['known_spells'])
        for sid in (25297, 16857, 17390):   # a book's, the talents'
            self.assertNotIn(sid, reply['known_spells'])
        acquired = self.ask(MAX_SKILLS, acquired_spells=[5487, 6795, 6807, 25297])
        self.assertEqual(acquired['highest_ranks']['6807'], 9881)
        self.assertEqual(acquired['highest_ranks']['5487'], 9634)
        self.assertEqual(acquired['highest_ranks']['5185'], 25297)

    def test_levels_curve_clamp_and_exact_unlock(self):
        curve = self.progression.curve
        self.assertEqual([wow_level(s, curve) for s in (-1, 15, 25, 50, 75, 100, 150)], [1, 1, 7, 25, 42, 60, 60])
        threshold = 15 + 39 * 85 / 59
        self.assertNotIn(16914, self.ask({'destruction': threshold - .000001})['known_spells'])
        self.assertIn(16914, self.ask({'destruction': threshold + .000001})['known_spells'])
        wow = self.service.handle({'op': 'known_spells', 'class': 11, 'caster': {'level': 40}})
        self.assertIn(16914, wow['known_spells'])
        self.assertEqual(set(wow['wow_levels'].values()), {40})
        skills_win = self.ask({}, caster={'level': 60, 'skyrim': {}})
        self.assertEqual(set(skills_win['known_spells']), {5176, 5185, 1126})

    def test_second_class_and_trainer_gates(self):
        # A hypothetical class uses a different skill, tab and arbitrary IDs.
        data = copy.deepcopy(self.progression.data)
        def row(sid, rank, previous, required=0):
            return {'spell': sid, 'name': 'Sentinel strike', 'chain': 101, 'rank': rank,
                    'previous_spell': previous, 'required_spell': required, 'tab': 7, 'skill_line': 999,
                    'skill': 'two_handed', 'source': 'starting' if rank == 1 else 'trainer',
                    'required_level': 1 if rank == 1 else 20, 'trainer_options': [] if rank == 1 else [{
                        'required_level': 20, 'req_skill': 44, 'req_skill_value': 50,
                        'required_abilities': [900], 'condition_id': 5}]}
        data['classes']['sentinel'] = {'id': 99, 'name': 'Sentinel', 'default_skill': 'two_handed',
            'lines': {'999': {'name': 'Arms', 'skill': 'two_handed', 'tab': 7}},
            'spells': [row(102, 2, 101, 900), row(101, 1, 0)],
            'excluded': [{**row(900, 1, 0), 'chain': 900, 'source': 'item'}]}
        service = Service(self.service.cat, None, Progression(data=data))
        request = {'op': 'known_spells', 'class': 99, 'caster': {'skyrim': {'two_handed': 100}}}
        self.assertEqual(service.handle(request)['known_spells'], [101])
        pending = service.handle(request)['unlearned_spells'][0]
        self.assertEqual((pending['spell'], pending['tab'], pending['learn_skill']), (102, 7, 'Two-handed'))
        self.assertEqual(pending['learn_skill_value'], 43)   # 15 + 19 * 85 / 59, whole
        self.assertIn('Requires Sentinel strike (item)', pending['learn_requirements'])
        self.assertIn('Also requires condition 5, WoW skill 44 50, spell 900', pending['learn_requirements'])
        request.update(acquired_spells=[900], satisfied_conditions=[5], since=[101])
        self.assertNotIn(102, service.handle(request)['known_spells'])  # skill gate still unmet
        request['caster']['wow_skills'] = {'44': 50}
        reply = service.handle(request)
        self.assertEqual(reply['class'], 'sentinel')
        self.assertEqual(set(reply['known_spells']), {101, 102, 900})
        self.assertEqual(reply['highest_ranks']['101'], 102)
        self.assertEqual(reply['lines']['999']['wow_level'], 60)
        self.assertEqual(reply['replacements'], [{'chain': 101, 'old_spell': 101, 'new_spell': 102}])
        del request['satisfied_conditions']
        self.assertNotIn(102, service.handle(request)['known_spells'])

    def test_all_classes_novice_starting_books(self):
        self.assertEqual(set(self.progression.data['classes']), set(CLASS_BOOKS))
        for key, (class_id, novice, _, _) in CLASS_BOOKS.items():
            with self.subTest(class_name=key):
                reply = self.ask({}, **{'class': key.upper()})
                self.assertEqual((reply['class'], reply['class_id']), (key, class_id))
                self.assertEqual(set(reply['known_spells']), novice)
                self.assertEqual(reply['newly_learned'], reply['known_spells'])
                for row in self.progression.data['classes'][key]['spells']:
                    if row['source'] == 'starting':
                        self.assertIn(row['spell'], novice)
                self.assertEqual(self.ask({}, **{'class': class_id})['known_spells'], reply['known_spells'])

    def test_all_classes_master_trainer_books(self):
        paladin_dependencies = {25918: 25290, 25898: 20217, 25899: 20914, 25916: 25291}
        for key, (_, _, count, _) in CLASS_BOOKS.items():
            with self.subTest(class_name=key):
                definition = self.progression.data['classes'][key]
                reply = self.ask(MAX_SKILLS, **{'class': key})
                known = set(reply['known_spells'])
                self.assertEqual(len(known), count)
                missing = {row['spell'] for row in definition['spells']} - known
                self.assertEqual(missing, set(paladin_dependencies) if key == 'paladin' else set())
                automatic = definition.get('auto_sources', ['quest'])
                for row in definition['excluded']:
                    if row['source'] in automatic:
                        self.assertIn(row['spell'], known)
                    else:
                        self.assertNotIn(row['spell'], known)
                repeat = self.ask(MAX_SKILLS, since=reply['known_spells'], **{'class': key})
                self.assertEqual((repeat['newly_learned'], repeat['replacements']), ([], []))
        # Greater blessings have real book/talent prerequisites; mastery alone
        # must not bypass them. Once earned, every paladin trainer rank opens.
        paladin = self.ask(MAX_SKILLS, acquired_spells=list(paladin_dependencies.values()), **{'class': 'paladin'})
        self.assertLessEqual({r['spell'] for r in self.progression.data['classes']['paladin']['spells']},
                             set(paladin['known_spells']))

    def test_all_classes_independent_lines(self):
        for key, (_, novice, _, examples) in CLASS_BOOKS.items():
            definition = self.progression.data['classes'][key]
            for line_id, example in examples.items():
                with self.subTest(class_name=key, line=line_id):
                    skill = definition['lines'][line_id]['skill']
                    reply = self.ask({skill: 100}, **{'class': key})
                    self.assertNotIn(example, novice)
                    self.assertIn(example, reply['lines'][line_id]['spells'])
                    added = set(reply['known_spells']) - novice
                    self.assertTrue(added)
                    self.assertTrue(all(row['skill'] == skill for row in reply['spells'] if row['spell'] in added))
                    for other_id, line in definition['lines'].items():
                        self.assertEqual(reply['lines'][other_id]['wow_level'], 60 if line['skill'] == skill else 1)
                    # Shared skills intentionally move both mage elemental
                    # lines and a class's auxiliary line; other skills stay put.
                    for other_id, other_spell in examples.items():
                        if definition['lines'][other_id]['skill'] != skill:
                            self.assertNotIn(other_spell, reply['known_spells'])

    def test_data_driven_quest_skill_pet_and_racial_routes(self):
        warrior = self.ask({'block': 100}, **{'class': 'warrior'})
        self.assertTrue({71, 355, 7386, 11597} <= set(warrior['known_spells']))
        self.assertNotIn(2458, warrior['known_spells'])  # Berserker Stance uses Fury.
        shaman = self.ask({'one_handed': 100}, **{'class': 'shaman'})
        self.assertIn(8071, shaman['known_spells'])
        self.assertNotIn(3599, shaman['known_spells'])
        hunter = self.ask({'conjuration': 100}, **{'class': 'hunter'})
        self.assertTrue({883, 982, 1515, 2641, 5149, 6991, 1853, 14927} <= set(hunter['known_spells']))
        self.assertNotIn(17254, hunter['known_spells'])  # Bite learned from taming, not a trainer.
        acquired = self.ask({'conjuration': 100}, acquired_spells=[17254], **{'class': 'hunter'})
        self.assertIn(17254, acquired['known_spells'])
        rogue = self.ask({'alchemy': 100}, **{'class': 'rogue'})
        self.assertTrue({2842, 8681, 8687, 11343} <= set(rogue['known_spells']))
        explicit_low = self.ask({'alchemy': 100}, caster={'skyrim': {'alchemy': 100}, 'wow_skills': {'40': 1}},
                                **{'class': 'rogue'})
        self.assertIn(3420, explicit_low['known_spells'])
        self.assertNotIn(11343, explicit_low['known_spells'])
        for key, spell in [('priest', 13908), ('priest', 6346), ('mage', 3561)]:
            with self.subTest(class_name=key, spell=spell):
                reply = self.ask(MAX_SKILLS, **{'class': key})
                self.assertNotIn(spell, reply['known_spells'])
                self.assertNotIn(spell, {r['spell'] for r in reply['unlearned_spells']})
                self.assertIn(spell, self.ask(MAX_SKILLS, acquired_spells=[spell], **{'class': key})['known_spells'])

    def test_cast_resources_are_metadata_not_acquisition_gates(self):
        shaman = self.ask(MAX_SKILLS, **{'class': 'shaman'})
        stoneskin = next(r for r in shaman['spells'] if r['spell'] == 8071)
        self.assertEqual(stoneskin['totem_items'], [5175])
        warlock = self.ask(MAX_SKILLS, **{'class': 'warlock'})
        voidwalker = next(r for r in warlock['spells'] if r['spell'] == 697)
        self.assertEqual(voidwalker['reagents'], [{'item': 6265, 'count': 1}])
        doom = next(r for r in warlock['spells'] if r['spell'] == 18540)
        self.assertNotIn('requires_pet', doom)  # Target 47 is a caster-front location, not a pet.
        hunter = self.ask(MAX_SKILLS, **{'class': 'hunter'})
        growl = next(r for r in hunter['spells'] if r['spell'] == 1853)
        self.assertTrue(growl['requires_pet'])
        self.assertEqual(growl['teaches_pet_spells'], [2649])
        marksman = next(r for r in self.ask({}, **{'class': 'hunter'})['unlearned_spells'] if r['spell'] == 3044)
        self.assertEqual(marksman['learn_skill'], 'Archery')

    def test_bad_requests_and_since_is_only_delta(self):
        self.assertNotIn(5487, self.ask(since=[5487])['known_spells'])
        cases = [{'class': 'death_knight'}, {'class': True}, {'caster': {}}, {'since': [True]}, {'since': '5176'},
                 {'acquired_spells': [999999]}, {'caster': {'skyrim': {'destruction': '100'}}},
                 {'caster': {'skyrim': {'destruction': float('nan')}}},
                 {'caster': {'skyrim': {'destruction': True}}}, {'caster': {'level': 4.5}},
                 {'caster': {'skyrim': {'unknown': 100}}}]
        for fields in cases:
            with self.subTest(fields=fields):
                reply = self.service.respond(json.dumps({'op': 'known_spells', 'class': 'druid',
                    'caster': {'skyrim': {}}, **fields}).encode())
                self.assertFalse(reply['ok'])
                self.assertTrue(reply['error'].startswith('BAD_REQUEST:'))
