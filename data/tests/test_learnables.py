import json
import re
import unittest

from data import dbc
from data.build_learnables import build, book_name, REPO, DBC_DIR, FIELDS, LEVEL_STEP, STYLES, SYNTHETIC
from data.build_catalog import DEFAULT_DUMP
from data.sql_dump import tables


class LearnablesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = json.loads((REPO / 'data/learnables.json').read_text())
        cls.books = {b['spell']: b for b in cls.data['books']}

    def test_export_matches_classicdb_progression_and_mounts(self):
        self.assertEqual(build(), self.data)

    def test_every_druid_rank_has_exactly_one_book(self):
        druid = json.loads((REPO / 'data/progression.json').read_text())['classes']['druid']
        ranks = {r['spell'] for r in druid['spells'] + druid['excluded'] if r['source'] not in ('talent', 'starting')}
        self.assertEqual(len(self.data['books']), len(self.books))   # one book a rank
        self.assertEqual(set(self.books), ranks)
        self.assertEqual(len(ranks), 179)
        for key in ('entry', 'name'):
            self.assertEqual(len({b['item'][key] for b in self.data['books']}), 179, key)
        self.assertNotIn(5176, self.books)   # Wrath (Rank 1): she starts with it
        self.assertNotIn(16689, self.books)  # Nature's Grasp: a talent

    def test_real_and_made_up_books(self):
        real = [b for b in self.data['books'] if b['real']]
        made = [b for b in self.data['books'] if not b['real']]
        self.assertEqual((len(real), len(made)), (90, 89))
        for b in real:   # 1.12's own druid books, by name and rank
            self.assertTrue(b['item']['entry'] < SYNTHETIC)
            self.assertRegex(b['item']['name'], r'^Book(:| of) ')
            self.assertTrue(b['item']['AllowableClass'] & 1 << 10)
        for b in made:   # in their style: name, display, class mask, quality, level and trainer-cost price
            self.assertEqual(b['item']['entry'], SYNTHETIC + b['spell'])
            self.assertEqual(b['item']['name'], book_name(b))
            self.assertEqual((b['item']['displayid'], b['item']['AllowableClass'], b['item']['Quality'], b['item']['class']),
                             (1317, 32256, 1, 9))
            self.assertEqual(b['item']['RequiredLevel'], b['required_level'])
            self.assertGreater(b['item']['BuyPrice'], 0)
        names = {b['spell']: b['item']['name'] for b in self.data['books']}
        self.assertEqual(names[25297], 'Book of Healing Touch XI')   # 21294, the AQ book
        self.assertEqual(names[21849], 'Book: Gift of the Wild')
        self.assertEqual(names[8925], 'Book of Moonfire III')
        self.assertEqual(names[768], 'Book of Cat Form')
        self.assertEqual(names[9634], 'Book of Dire Bear Form')      # unranked: no numeral
        self.assertEqual(names[17401], 'Book of Hurricane II')
        self.assertEqual(self.books[25297]['item']['entry'], 21294)
        self.assertEqual(self.books[5177]['item']['entry'], 5142)     # Book of Wrath II, not "Book of Wrath" (8744)
        # Every book-only rank (Healing Touch XI etc.) is a real 1.12 book.
        self.assertTrue(all(b['real'] for b in self.data['books'] if b['source'] == 'item'))

    def test_teaching_text_is_the_clients_own(self):
        spells = dbc.by_id(dbc.read(DBC_DIR / 'Spell.dbc'))
        checked = 0
        for b in self.data['books']:
            self.assertRegex(b['text'], r'^Teaches .+?( \(Rank \d+\))?\.$')
            learn = spells.get(b['item']['spellid_1'])
            # Where 1.12's own learning spell teaches this rank and says so (some teach another rank, some say nothing;
            # Book of Regrowth's says "Teaches Rejuvenation (Rank 1).")
            if b['real'] and learn and learn['Effect'][0] == 36 and learn['EffectTriggerSpell'][0] == b['spell'] and \
                    f"Teaches {b['name']}" in learn['Description_lang']:
                self.assertEqual(b['text'], learn['Description_lang'])
                checked += 1
        self.assertGreater(checked, 30)
        self.assertEqual(self.books[2782]['text'], 'Teaches Remove Curse.')
        self.assertEqual(self.books[25297]['text'], 'Teaches Healing Touch (Rank 11).')

    def test_one_item_per_mount(self):
        mounts = json.loads((REPO / 'data/mounts.json').read_text())['mounts']
        # Reins for every mount but a class mount (a spell she learns from her librams).
        self.assertEqual([m['key'] for m in self.data['mounts']], [m['key'] for m in mounts if 'class' not in m])
        items = {m['key']: m for m in self.data['mounts']}
        self.assertEqual(sum(m['real'] for m in self.data['mounts']), 21)
        self.assertEqual(items['swift_frostsaber']['item']['name'], 'Reins of the Swift Frostsaber')
        self.assertEqual(items['swift_frostsaber']['item']['entry'], 18766)
        self.assertEqual(items['swift_brown_wolf']['item']['name'], 'Horn of the Swift Brown Wolf')
        self.assertEqual(items['great_white_kodo']['item']['name'], 'Great White Kodo')
        for m in self.data['mounts']:
            self.assertEqual(m['item']['spellid_1'], m['spell'])
            self.assertEqual(m['item']['Quality'], 4)
            self.assertTrue(m['item']['displayid'] or re.match(r'^Interface\\Icons\\', m['icon']))
        gryphon, rider = items['swift_golden_gryphon'], items['swift_green_wind_rider']
        self.assertEqual((gryphon['item']['entry'], gryphon['item']['name']), (25470, 'Swift Golden Gryphon'))
        self.assertEqual((rider['item']['entry'], rider['item']['name']), (25531, 'Swift Green Wind Rider'))
        self.assertEqual(gryphon['item']['displayid'], 33540)   # 1.12's Ability_Hunter_Pet_Owl item icon
        self.assertEqual((rider['item']['displayid'], rider['icon']), (0, 'Interface\\Icons\\Ability_Hunter_Pet_Bat'))
        self.assertTrue(rider['text'].endswith('can fly outdoors.'))
        self.assertEqual(items['swift_frostsaber']['text'],
                         'Summons and dismisses a rideable Swift Frostsaber.  This is a very fast mount.')


class PaladinBooksTest(unittest.TestCase):
    """The Paladin's books (class_books.paladin): exported for a later phase, not shipped (data/build_esp.py reads only
    'books', the Druid's, which stay as they were)."""
    @classmethod
    def setUpClass(cls):
        cls.data = json.loads((REPO / 'data/learnables.json').read_text())
        cls.books = {b['spell']: b for b in cls.data['class_books']['paladin']}
        cls.manifest = json.loads((REPO / 'data/paladin_manifest.json').read_text())
        cls.items = {r['entry']: r for r in tables(DEFAULT_DUMP, {'item_template': FIELDS},
                                                   filters={'item_template': lambda r: r['class'] == 9})['item_template']}

    def test_one_book_for_every_trained_quest_and_item_rank(self):
        ranks = {r['spell']: r for r in self.manifest['ranks'] if r['source'] not in ('talent', 'starting')}
        self.assertEqual(set(self.books), set(ranks))
        self.assertEqual(len(self.books), 142)
        self.assertEqual(len(self.data['class_books']['paladin']), 142)
        for key in ('entry', 'name'):
            self.assertEqual(len({b['item'][key] for b in self.books.values()}), 142, key)
        self.assertFalse(set(self.books) & {b['spell'] for b in self.data['books']})   # the Druid's list has none
        self.assertNotIn(635, self.books)     # Holy Light (Rank 1): she starts with it
        self.assertNotIn(20154, self.books)   # Seal of Righteousness (Rank 1): likewise
        self.assertNotIn(20473, self.books)   # Holy Shock: a talent (its trainer ranks 2 and 3 have books)
        self.assertIn(20929, self.books)
        for b in self.books.values():
            self.assertEqual({k: b[k] for k in ('chain', 'rank', 'rank_name', 'name', 'previous_spell', 'skill',
                                                'required_level', 'source')},
                             {k: ranks[b['spell']][k] for k in ('chain', 'rank', 'rank_name', 'name', 'previous_spell',
                                                                'skill', 'required_level', 'source')})

    def test_real_and_made_up_libram(self):
        real = [b for b in self.books.values() if b['real']]
        made = [b for b in self.books.values() if not b['real']]
        self.assertEqual((len(real), len(made)), (27, 115))
        taught = {i['item'] for r in self.manifest['ranks'] for i in r['provenance'].get('items', [])}
        for b in real:   # the item that teaches the rank, else 1.12's own libram of its name and rank at its level
            self.assertTrue(b['item']['AllowableClass'] & 2 and b['item']['entry'] < SYNTHETIC)
            if b['item']['entry'] not in taught:
                self.assertEqual(b['item']['name'], book_name(b, 'Libram: '))
                self.assertLessEqual(abs(b['item']['RequiredLevel'] - b['required_level']), LEVEL_STEP)
        for b in made:
            self.assertEqual(b['item']['entry'], SYNTHETIC + b['spell'])
            self.assertEqual(b['item']['name'], book_name(b, 'Libram: '))
            self.assertEqual((b['item']['displayid'], b['item']['AllowableClass'], b['item']['Quality'], b['item']['class']),
                             (1155, 31234, 1, 9))
            self.assertEqual(b['item']['RequiredLevel'], b['required_level'])
            self.assertGreater(b['item']['BuyPrice'], 0)
        # Every rank only a libram teaches (Blessing of Wisdom VI, Might VII, Holy Light IX) is its AQ libram.
        self.assertEqual({b['spell']: b['item']['entry'] for b in self.books.values() if b['source'] == 'item'},
                         {25290: 21288, 25291: 21289, 25292: 21290})
        names = {s: b['item']['name'] for s, b in self.books.items()}
        self.assertEqual(names[25292], 'Libram: Holy Light IX')
        self.assertEqual(names[639], 'Libram: Holy Light II')         # 1141, level 6 as the rank
        self.assertEqual(self.books[1152]['item']['entry'], 1150)    # Libram: Purify at 6 for a level-8 rank
        self.assertEqual(self.books[7328]['item']['entry'], 1146)    # Libram: Resurrection's spell teaches Redemption 1
        self.assertEqual(self.books[5502]['item']['entry'], 5676)    # Libram: Sense Undead
        # A pre-release libram of the rank's name far from its level is another spell's: the rank's is made up.
        for spell, old in ((21084, 5660), (20287, 5661), (20166, 6132), (20271, 4167), (4987, 1139)):
            self.assertFalse(self.books[spell]['real'], spell)
            self.assertEqual(self.books[spell]['item']['name'], self.items[old]['name'])
            self.assertGreater(abs(self.items[old]['RequiredLevel'] - self.books[spell]['required_level']), LEVEL_STEP)
        self.assertEqual(names[21084], 'Libram: Seal of Righteousness')   # its "Rank 1", the second rank
        self.assertEqual(names[13819], 'Libram: Summon Warhorse')
        self.assertEqual(self.books[13819]['text'], 'Teaches Summon Warhorse.')
        self.assertEqual(self.books[25898]['text'], 'Teaches Greater Blessing of Kings.')

    def test_made_up_style_is_each_class_books_own(self):
        for key, style in STYLES.items():
            with self.subTest(cls=key):
                own = [i for i in self.items.values() if i['subclass'] == 0 and i['Quality'] == 1 and
                       i['AllowableClass'] != -1 and i['AllowableClass'] & style['mask'] and style['pattern'].match(i['name'])]
                self.assertGreater(len(own), 40)
                self.assertEqual({(i['displayid'], i['AllowableClass']) for i in own},
                                 {(style['displayid'], style['AllowableClass'])})
                self.assertEqual(max({i['Flags'] for i in own}, key=[i['Flags'] for i in own].count), 80)

    def test_teaching_text_is_the_clients_own(self):
        spells = dbc.by_id(dbc.read(DBC_DIR / 'Spell.dbc'))
        checked = 0
        for b in self.books.values():
            self.assertRegex(b['text'], r'^Teaches .+?( \(Rank \d+\))?\.$')
            learn = spells.get(b['item']['spellid_1'])
            if b['real'] and learn and learn['Effect'][0] == 36 and learn['EffectTriggerSpell'][0] == b['spell'] and \
                    f"Teaches {b['name']}" in learn['Description_lang']:
                self.assertEqual(b['text'], learn['Description_lang'])
                checked += 1
        self.assertEqual(checked, 17)
        self.assertEqual(self.books[25292]['text'], 'Teaches Holy Light (Rank 9).')


if __name__ == '__main__':
    unittest.main()
