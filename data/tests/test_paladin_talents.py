"""The Paladin's talent frame data (data/talents.json, data/talent_catalog.json) against the 1.12.1 client DBCs read
directly (Talent.dbc, TalentTab.dbc, Spell.dbc, SpellIcon.dbc), not through data/build_talents.py."""
import json
import re
import unittest
from pathlib import Path

from data import dbc
from data.build_catalog import DBC_DIR, REPO

PALADIN_MASK = 1 << (2 - 1)        # ChrClasses 2: TalentTab.ClassMask bit
CATALOG = REPO / 'build/catalog.json'


class PaladinTalentData(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = json.loads((REPO / 'data/talents.json').read_text())['classes']['paladin']
        cls.tabs = sorted((t for t in dbc.read(Path(DBC_DIR) / 'TalentTab.dbc') if t['ClassMask'] & PALADIN_MASK),
                          key=lambda t: t['OrderIndex'])
        cls.talents = dbc.read(Path(DBC_DIR) / 'Talent.dbc')
        cls.spells = dbc.by_id(dbc.read(Path(DBC_DIR) / 'Spell.dbc'))
        cls.icons = dbc.by_id(dbc.read(Path(DBC_DIR) / 'SpellIcon.dbc'))

    def test_three_trees_in_the_client_order(self):
        self.assertEqual([(t['Name_lang'], t['ID']) for t in self.tabs], [('Holy', 382), ('Protection', 383), ('Retribution', 381)])
        self.assertEqual([(t['name'], t['id'], t['tab']) for t in self.data['trees']],
                         [(t['Name_lang'], t['ID'], t['OrderIndex'] + 1) for t in self.tabs])
        for tree, tab in zip(self.data['trees'], self.tabs):
            self.assertEqual(tree['icon'], self.icons[tab['SpellIconID']]['TextureFilename'])
            self.assertEqual(tree['background'], tab['BackgroundFile'])

    def test_every_talent_its_place_ranks_prerequisites_and_tooltips(self):
        count = 0
        for tree, tab in zip(self.data['trees'], self.tabs):
            client = {t['ID']: t for t in self.talents if t['TabID'] == tab['ID']}
            self.assertEqual({t['id'] for t in tree['talents']}, set(client))
            for t in tree['talents']:
                row = client[t['id']]
                ranks = [sid for sid in row['SpellRank'] if sid]
                with self.subTest(talent=t['name']):
                    self.assertEqual((t['tier'], t['column'], t['max_rank'], t['rank_spells']),
                                     (row['TierID'], row['ColumnIndex'], len(ranks), ranks))
                    self.assertEqual(t['prerequisites'], [{'talent': tid, 'rank': rank + 1} for tid, rank
                                                          in zip(row['PrereqTalent'], row['PrereqRank']) if tid])
                    self.assertEqual(t['required_spell'], row['RequiredSpellID'])
                    self.assertEqual(t['name'], self.spells[ranks[0]]['Name_lang'])
                    self.assertEqual(t['icon'], self.icons[self.spells[ranks[0]]['SpellIconID']]['TextureFilename'])
                    # The tooltip text: each rank's own Spell.dbc description, its $ tokens left for the interface.
                    self.assertEqual(t['descriptions'], [self.spells[sid]['Description_lang'] for sid in ranks])
                    count += len(ranks)
        self.assertEqual(count, 140)

    def test_tooltip_references_are_in_the_catalog(self):
        """$20233s1, $25912s1, $67d...: every spell a description quotes is a catalog spell the interface can read."""
        catalog = json.loads(CATALOG.read_text())['spells']
        extra = json.loads((REPO / 'data/talent_catalog.json').read_text())['spells']
        for tree in self.data['trees']:
            for t in tree['talents']:
                for sid, text in zip(t['rank_spells'], t['descriptions']):
                    self.assertIn(str(sid), catalog)
                    for ref in re.findall(r'\$(?:[/*]\d+;)?(\d+)[a-zA-Z]', text):
                        self.assertTrue(ref in catalog or ref in extra, (t['name'], ref))

    def test_talent_taught_spells_and_their_trainer_ranks(self):
        """The spells a talent rank teaches (its own rank spell, active) and the higher ranks trainers sell after it
        (npc_trainer ReqAbility chains: data/talents.json trainer_options)."""
        taught = {t['name']: t['rank_spells'][0] for tree in self.data['trees'] for t in tree['talents']
                  if not self.spells[t['rank_spells'][0]]['Attributes'] & 0x40}
        self.assertEqual(taught, {'Consecration': 26573, 'Divine Favor': 20216, 'Holy Shock': 20473,
                                  'Blessing of Kings': 20217, 'Blessing of Sanctuary': 20911, 'Holy Shield': 20925,
                                  'Seal of Command': 20375, 'Sanctity Aura': 20218, 'Repentance': 20066})
        self.assertEqual(set(self.data['trainer_options']),
                         {str(s) for s in (20116, 20922, 20923, 20924, 20929, 20930, 20912, 20913, 20914, 20927, 20928,
                                           20915, 20918, 20919, 20920)})


if __name__ == '__main__':
    unittest.main()
