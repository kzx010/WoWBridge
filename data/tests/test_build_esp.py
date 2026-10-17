"""Read-back checks against the real Skyrim master and client spell descriptions."""
import copy
import json
import re
import struct
import tempfile
import unittest
from pathlib import Path

from data import build_esp as B, esm
from service import skyrim_effects


class BuildESPTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.catalog = json.loads((B.REPO / 'build/catalog.json').read_text())
        cls.forms = cls.root / 'forms.json'
        # Check the persisted repository IDs without changing them during tests.
        cls.forms.write_bytes((B.REPO / 'data/esp_forms.json').read_bytes())
        cls.original_forms = dict(B.Forms(cls.forms).ids)
        cls.dbc = B.REPO / 'build/dbc/DBFilesClient/Spell.dbc'
        B.build(cls.catalog, B.MASTER, cls.dbc, cls.forms, cls.root)
        cls.path = cls.root / 'esp/WoWBridge.esp'
        cls.groups = esm.read(cls.path, {'ARMO', 'WEAP', 'ENCH', 'SPEL', 'MGEF', 'COBJ', 'BOOK', 'LVLI', 'GLOB'})
        cls.records = {r.form: r for rs in cls.groups.values() for r in rs}
        cls.gear = json.loads((cls.root / 'gear.json').read_text())

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def item(self, entry):
        return self.records[0x01000000 | self.gear['items'][str(entry)]['form']]

    def effects(self, magic):
        return {struct.unpack('<I', fid)[0]: struct.unpack('<fII', fit)[0]
                for fid, fit in zip(magic.all('EFID'), magic.all('EFIT'))}

    def test_header_and_ids(self):
        raw = self.path.read_bytes()
        self.assertEqual(raw[:4], b'TES4')
        self.assertEqual(struct.unpack_from('<I', raw, 8)[0], esm.ESL)
        header = esm._subrecords(raw[24:24 + struct.unpack_from('<I', raw, 4)[0]])
        self.assertEqual([d for t, d in header if t == 'MAST'], [b'Skyrim.esm\0'])
        for form in self.records:
            self.assertEqual(form >> 24, 1)  # local records belong to the file after its master
            self.assertTrue(0x800 <= (form & 0xFFFFFF) <= 0xFFF)
        next_id = struct.unpack('<fII', dict(header)['HEDR'])[2]
        self.assertGreater(next_id, max(f & 0xFFFFFF for f in self.records))

    def test_every_item_and_strings(self):
        self.assertEqual(self.gear['plugin'], 'WoWBridge.esp')
        self.assertEqual(set(self.gear['items']), set(self.catalog['items']))
        self.assertEqual(len(self.groups['ARMO']) + len(self.groups['WEAP']), len(self.catalog['items']))
        for entry, item in self.catalog['items'].items():
            r = self.item(entry)
            self.assertEqual(r.get('FULL'), esm.zstring(item['name']))
            self.assertEqual(self.gear['items'][entry]['kind'], r.type)
            self.assertEqual(self.gear['items'][entry]['entry'], int(entry))
            self.assertEqual(self.gear['items'][entry]['name'], item['name'])
            self.assertEqual(self.gear['items'][entry]['wow'], {**item, 'RequiredLevel': 0})
            self.assertEqual(self.gear['items'][entry]['wow']['RequiredLevel'], 0)
            self.assertEqual(self.gear['items'][entry]['quality'], int(item.get('Quality', 1)))
        for r in self.records.values():
            for field in ('FULL', 'DESC'):
                if r.get(field) is not None:
                    self.assertTrue(r.get(field).endswith(b'\0'))
                    self.assertNotEqual(len(r.get(field)), 4)
            if r.type != 'COBJ':
                self.assertIsNone(r.get('CTDA'))  # includes no level gates inherited from templates

    def test_books_and_mount_items_are_plain_skyrim_books_with_client_rows(self):
        learn = json.loads(B.LEARNABLES.read_text())
        books, mounts = self.gear['books'], self.gear['mounts']
        self.assertEqual((len(books), len(mounts)),
                         (len(learn['books']) + len(learn['class_books']['paladin']), len(learn['mounts'])))
        self.assertEqual(len(self.groups['BOOK']), len(books) + len(mounts))
        for row in list(books.values()) + list(mounts.values()):
            r = self.records[0x01000000 | row['form']]
            self.assertEqual((r.type, row['kind']), ('BOOK', 'BOOK'))
            self.assertEqual(r.get('FULL'), esm.zstring(row['name']))
            flags, _, _, teaches, value, _ = struct.unpack('<BBHIIf', r.get('DATA'))
            self.assertEqual((flags, teaches), (0, 0xFFFFFFFF))   # reading teaches no Skyrim spell or skill itself
            self.assertEqual(value, row['skyrim']['value'])
            self.assertIsNone(r.get('CNAM'))
            self.assertEqual(row['wow']['RequiredLevel'], row['skyrim']['level'])
        by_spell = {b['teaches']['spell']: b for b in books.values()}
        values = {spell: b['skyrim']['value'] for spell, b in by_spell.items()}
        # Vanilla tome values by the skill a rank asks: Moonfire 1 (20), Starfire 1 (43), Moonfire 7 (72), Wrath 8 (92).
        self.assertEqual((values[8921], values[2912], values[8929], values[9912], values[25297]), (50, 300, 650, 1300, 1300))
        moonfire, touch = by_spell[8921], by_spell[25297]
        self.assertEqual((moonfire['skyrim']['level'], touch['skyrim']['level'], by_spell[9912]['skyrim']['level']), (1, 36, 31))
        self.assertEqual(touch['wow']['requiredspell'], 9889)   # Healing Touch (Rank 10) first
        self.assertEqual(moonfire['wow']['requiredspell'], 0)
        self.assertEqual(touch['teaches'], {'spell': 25297, 'previous': 9889, 'text': 'Teaches Healing Touch (Rank 11).'})
        self.assertEqual(touch['wow']['spellid_1'], 0)   # the Use: line is the teaching text
        self.assertEqual(self.records[0x01000000 | touch['form']].get('DESC'),
                         esm.zstring('Book of Healing Touch XI\n\nUse: Teaches Healing Touch (Rank 11).'))
        saber = mounts['18766']
        self.assertEqual(saber['teaches'], {'mount': 'swift_frostsaber', 'spell': 23221})
        self.assertEqual((saber['wow']['RequiredLevel'], saber['wow']['RequiredSkill'], saber['wow']['AllowableRace']), (30, 0, -1))
        self.assertEqual(saber['wow']['spellid_1'], 23221)   # its 1.12 Use: line
        self.assertEqual(saber['skyrim']['value'], 2000)
        self.assertEqual((mounts['25531']['skyrim'], mounts['25531']['icon']),
                         ({'level': 46, 'value': 6000}, 'Interface\\Icons\\Ability_Hunter_Pet_Bat'))
        self.assertNotIn('icon', saber)

    def test_leveled_lists_reach_only_treasure_and_vendors(self):
        lists = {r.edid: r for r in self.groups['LVLI']}
        self.assertEqual(set(lists), {'WoWSpellBooks', 'WoWSpellBooksDestruction', 'WoWSpellBooksRestoration',
                                      'WoWSpellBooksAlteration', 'WoWMountItemsStables', 'WoWMountItemsCaravan',
                                      'WoWPaladinBooks', 'WoWPaladinBooksRestoration'})
        entries = lambda r: [struct.unpack('<HHIHH', d) for d in r.all('LVLO')]
        paladin = lambda b: b['wow']['AllowableClass'] & 1 << 1 and not b['wow']['AllowableClass'] & 1 << 10
        books = {0x01000000 | b['form']: b for b in self.gear['books'].values() if not paladin(b)}
        librams = {0x01000000 | b['form']: b for b in self.gear['books'].values() if paladin(b)}
        self.assertEqual(len(librams), 142)
        self.assertEqual({e[2] for e in entries(lists['WoWPaladinBooks'])}, set(librams))
        # Each class's lists roll only for that class: a chance-none global the plugin sets (0 hers, 100 others').
        globals_ = {r.edid: r for r in self.groups['GLOB']}
        druid, pal = (0x01000000 | globals_[e].form & 0xFFFFFF for e in ('WoWBridgeDruidBooksNone', 'WoWBridgePaladinBooksNone'))
        self.assertEqual(self.gear['class_book_globals'], {'11': druid, '2': pal})
        self.assertEqual((struct.unpack('<f', globals_['WoWBridgeDruidBooksNone'].get('FLTV'))[0],
                          struct.unpack('<f', globals_['WoWBridgePaladinBooksNone'].get('FLTV'))[0]), (0.0, 100.0))
        for edid, gate in (('WoWSpellBooks', druid), ('WoWSpellBooksRestoration', druid), ('WoWPaladinBooks', pal),
                           ('WoWPaladinBooksRestoration', pal), ('WoWMountItemsStables', None)):
            self.assertEqual(lists[edid].get('LVLG'), None if gate is None else struct.pack('<I', gate))
        mounts = {0x01000000 | m['form']: m for m in self.gear['mounts'].values()}
        everything = entries(lists['WoWSpellBooks'])
        self.assertEqual({e[2] for e in everything}, set(books))
        for level, _, form, count, _ in everything:
            self.assertEqual((level, count), (books[form]['skyrim']['level'], 1))
        self.assertEqual(lists['WoWSpellBooks'].get('LVLF'), b'\x03')
        self.assertEqual(lists['WoWSpellBooks'].get('LLCT'), bytes([len(books)]))
        restoration = {e[2] for e in entries(lists['WoWSpellBooksRestoration'])}
        self.assertIn(0x01000000 | self.gear['books']['21294']['form'], restoration)
        self.assertEqual(lists['WoWMountItemsStables'].get('LVLF'), b'\x05')   # every mount she is old enough for
        stables = entries(lists['WoWMountItemsStables'])
        self.assertEqual({e[2] for e in stables}, set(mounts))
        self.assertEqual(sorted({e[0] for e in stables}), [30, 46])
        # Targets: lists and vendor chests that no NPC inventory, death item or outfit reaches.
        master = esm.read(B.MASTER, {'LVLI', 'CONT', 'NPC_', 'OTFT', 'FACT'})
        users = {}
        for r in master['LVLI']:
            for d in r.all('LVLO'):
                users.setdefault(struct.unpack_from('<I', d, 4)[0], set()).add(r.form)
        for r in master['NPC_']:
            for d in r.all('CNTO') + r.all('INAM'):
                users.setdefault(struct.unpack_from('<I', d)[0], set()).add('npc')
        for r in master['OTFT']:
            for d in r.all('INAM'):
                for n in range(0, len(d), 4):
                    users.setdefault(struct.unpack_from('<I', d, n)[0], set()).add('npc')
        for r in master['CONT']:
            for d in r.all('CNTO'):
                users.setdefault(struct.unpack_from('<I', d)[0], set()).add(('cont', r.form))

        def reaches_npc(form, seen=()):
            return any(u == 'npc' or isinstance(u, int) and u not in seen and reaches_npc(u, seen + (form,))
                       for u in users.get(form, ()))
        factions = {r.form: r for r in master['FACT']}
        targets = {d['name']: d for d in self.gear['distribution']}
        self.assertEqual(set(targets), set(lists))
        for d in targets.values():
            self.assertEqual(self.records[0x01000000 | d['list']].edid, d['name'])
            for target in d['lists']:
                self.assertFalse(reaches_npc(target['form']), hex(target['form']))
            for target in d['vendors']:
                self.assertIsNotNone(factions[target['form']].get('VENC'), hex(target['form']))   # a merchant chest
        self.assertTrue(reaches_npc(0x9E2B0))   # LItemSpellTomes00Alteration: mage bandits carry it
        self.assertIsNone(factions[0xA3F21].get('VENC'))   # ServicesWindhelmStables: no chest to stock
        self.assertEqual([t['form'] for t in targets['WoWSpellBooks']['lists']], [0x10FD8C, 0xA297A])
        self.assertEqual([t['form'] for t in targets['WoWMountItemsStables']['vendors']], [0xA6C01])
        self.assertEqual([t['form'] for t in targets['WoWMountItemsCaravan']['vendors']], [0x74351, 0x74352, 0x74354])

    def test_relic_world_models_keep_equipment_identity(self):
        native = next(r for r in esm.read(B.MASTER, {'MISC'})['MISC'] if r.edid == 'StatueDibellaGold')
        for entry in (22397, 22398):
            item = self.item(entry)
            self.assertEqual(item.type, 'ARMO')
            self.assertEqual(item.get('OBND'), native.get('OBND'))
            for model, textures in (('MOD2', 'MO2T'), ('MOD4', 'MO4T')):
                self.assertEqual(item.get(model), native.get('MODL'))
                self.assertEqual(item.get(textures), native.get('MODT'))
            self.assertEqual(struct.unpack('<II', item.get('BOD2')), (1 << 31, 0))
            self.assertIsNone(item.get('MODL'))  # no worn cuirass or statue armor addon
            self.assertEqual(self.gear['items'][str(entry)]['form'], self.original_forms[f'item:{entry}'])

    def test_worn_looks_fill_only_the_items_own_slots(self):
        # As the Armory's: her armour has a Skyrim look (data/build_esp.py look), its world model and, for an NPC (a
        # follower) wearing it, its addons over the item's own slots only. Helms, chests and robes, gloves, boots and the
        # shield are seen; shoulders, belts, legs and bracers (whose look is a cuirass's or gauntlets') and relics are not.
        templates, _ = B.read_master(B.MASTER, {'ARMO', 'WEAP', 'MISC'})
        arma = {r.form: r for r in esm.read(B.MASTER, {'ARMA'})['ARMA']}
        biped = lambda r: struct.unpack_from('<I', r.get('BOD2') or r.get('BODT'))[0]
        curated = B.reviewed_looks()
        seen = {}
        for entry, item in self.catalog['items'].items():
            r = self.item(entry)
            if r.type != 'ARMO':
                continue
            look = B.look(item, self.gear['items'][entry]['skyrim'], templates, curated)
            for d in r.all('MODL'):
                form = struct.unpack('<I', d)[0]
                self.assertEqual(form >> 24, 0, entry)   # Skyrim.esm's
                self.assertIn(form, arma, (entry, hex(form)))
                self.assertTrue(biped(arma[form]) & biped(r) or (biped(r) & B.HEAD_SLOTS and biped(arma[form]) & B.CIRCLET),
                                (entry, hex(form)))   # a helm with a circlet look wears the circlet
                self.assertIn(d, look.all('MODL'), entry)
            if look is not None:
                self.assertEqual((r.get('MOD2'), r.get('OBND')), (look.get('MOD2'), look.get('OBND')), entry)
            seen.setdefault(item['InventoryType'], set()).add(bool(r.all('MODL')))
        self.assertEqual(seen, {**{slot: {True} for slot in (1, 5, 20, 10, 8, 14)}, **{slot: {False} for slot in (3, 6, 7, 9, 28)}})
        # Pauldrons of Might look like a cuirass on the ground, and wear none of it over the Breastplate of Might.
        pauldrons = B.look(self.catalog['items']['16868'], self.gear['items']['16868']['skyrim'], templates, curated)
        self.assertTrue(pauldrons.edid.endswith(('Cuirass', 'CuirassA')) and pauldrons.all('MODL'), pauldrons.edid)
        self.assertEqual(self.item(16868).all('MODL'), [])
        self.assertTrue(self.item(16865).all('MODL'))

    def test_stormrage(self):
        r = self.item(16897)
        self.assertEqual(r.type, 'ARMO')
        self.assertEqual(struct.unpack('<II', r.get('BOD2')), (1 << 2, 0))
        # ItemLevel 76 is Skyrim level 48: a Dragonscale Cuirass (41), epic (x1.1).
        self.assertEqual({k: self.gear['items']['16897']['skyrim'][k] for k in ('level', 'tier', 'armor')},
                         {'level': 48, 'tier': 'Dragonscale', 'armor': round(41 * 1.1)})
        self.assertEqual(struct.unpack('<I', r.get('DNAM'))[0], round(41 * 1.1) * 100)
        self.assertEqual(struct.unpack('<If', r.get('DATA'))[1], 6)
        keywords = struct.unpack('<' + 'I' * (len(r.get('KWDA')) // 4), r.get('KWDA'))
        self.assertIn(0x6BBD3, keywords)
        self.assertIn(0x6C0EC, keywords)
        self.assertNotIn(0xC27BD, keywords)
        self.assertIsNone(r.get('EITM'))
        ench = self.records[0x01000000 | self.gear['items']['16897']['ability']]
        self.assertEqual(ench.type, 'SPEL')
        effects = self.effects(ench)
        self.assertEqual(effects[0x49504], 25 * 1.5)
        self.assertEqual(effects[0x493AA], 20 * 1.5)
        self.assertEqual(effects[0x7A0FD], 17)
        self.assertAlmostEqual(effects[0x48C8B], 10 / 3, places=5)
        self.assertAlmostEqual(effects[0x49295], 10 / 3, places=5)

    def test_staff(self):
        r = self.item(873)
        self.assertEqual(r.type, 'WEAP')
        # ItemLevel 40 is Skyrim level 21: an Elven Warhammer's 23 damage at speed .6, swung at 2.6/3.7, epic.
        damage = round(23 * .6 / (2.6 / 3.7) * 1.1)
        self.assertEqual(self.gear['items']['873']['skyrim'], {'level': 21, 'tier': 'Elven', 'damage': damage,
                                                               'value': round(565 * 1.1)})
        self.assertEqual(struct.unpack('<IfH', r.get('DATA')), (round(565 * 1.1), 24.0, damage))
        self.assertAlmostEqual(struct.unpack_from('<f', r.get('DNAM'), 4)[0], 2.6 / 3.7, places=6)
        self.assertEqual(r.get('MODL'), esm.zstring(r'Weapons\Iron\WarHammer.nif'))
        self.assertIsNone(r.get('EITM'))
        self.assertNotIn(struct.pack('<I', 0xC27BD), r.get('KWDA'))
        ability = self.records[0x01000000 | self.gear['items']['873']['ability']]
        self.assertEqual(ability.type, 'SPEL')
        spit = struct.unpack('<3If2If2I', ability.get('SPIT'))
        self.assertEqual(spit, (0, 0, 4, 0, 0, 0, 0, 0, 0))
        self.assertEqual(self.effects(ability)[0x49504], 16.5)
        self.assertEqual(self.effects(ability)[0x7A0FD], 11)

    def test_descriptive_effects(self):
        self.assertTrue(self.groups['MGEF'])
        for r in self.groups['MGEF']:
            self.assertNotIn(b'$', r.get('DNAM'))  # includes references to a triggered spell's magnitude
            self.assertEqual(struct.unpack_from('<i', r.get('DATA'), 12)[0], -1)
            self.assertEqual(struct.unpack_from('<I', r.get('DATA'), 72)[0], 77)
        jordan = self.records[0x01000000 | self.gear['items']['873']['ability']]
        descriptions = [(self.records[f].get('DNAM'), v) for f, v in self.effects(jordan).items() if f in self.records]
        self.assertIn((esm.zstring('Increases damage and healing done by magical spells and effects by up to <mag>.'), 26), descriptions)
        shield = self.records[0x01000000 | self.gear['items']['1168']['ability']]
        self.assertIn(esm.zstring('When struck in combat has a 3% chance of stealing 35 life from target enemy.'),
                      [self.records[f].get('DNAM') for f in self.effects(shield) if f in self.records])

    def test_stable_ids_and_bytes(self):
        current = B.Forms(self.forms).ids
        for key, value in self.original_forms.items():
            self.assertEqual(current[key], value)
        before = (self.path.read_bytes(), self.forms.read_bytes(), (self.root / 'gear.json').read_bytes())
        B.build(self.catalog, B.MASTER, self.dbc, self.forms, self.root)
        self.assertEqual(before, (self.path.read_bytes(), self.forms.read_bytes(), (self.root / 'gear.json').read_bytes()))
        forms = B.Forms(self.forms)
        old = dict(forms.ids)
        forms.get('item:999999')
        self.assertTrue(all(forms.ids[k] == v for k, v in old.items()))

    def test_physical_and_other_slots(self):
        templates = {r.edid: r for rs in esm.read(B.MASTER, {'ARMO', 'WEAP', 'KYWD', 'MISC'}).values() for r in rs}
        base = copy.deepcopy(self.catalog['items']['16897'])
        # Exercise physical conversion with an equip attack-power aura and mana regeneration.
        base.update({'stat_type1': 4, 'stat_value1': 20, 'stat_type2': 3, 'stat_value2': 10,
                     'stat_type3': 0, 'stat_value3': 100, 'stat_type4': 1, 'stat_value4': 100,
                     'spellid_1': 900000, 'spellid_2': 900001})
        cat = {'spells': {'900000': {'Effect1': 6, 'EffectApplyAuraName1': 99, 'EffectBasePoints1': 99},
                          '900001': {'Effect1': 6, 'EffectApplyAuraName1': 85, 'EffectBasePoints1': 4, 'EffectMiscValue1': 0}}}
        effects, desc = B.effects_for(base, cat, {})
        e = dict(effects)
        self.assertEqual(desc, [])
        for fid, value in {0x7A0F4: 20, 0x7A0FF: 20, 0x7A106: 20, 0x49507: 15,
                           0x49504: 10, 0x493AA: 15, 0x7A0FD: 10}.items():
            self.assertEqual(e[fid], value)
        ratings = {}
        for subclass, kind in ((1, 0), (2, 0), (3, 1), (4, 1), (6, 1), (8, 0)):
            base['subclass'] = subclass
            stats = B.skyrim_stats(base, templates)
            r = B.armor(base, templates, 0xFFF, stats)
            self.assertEqual(struct.unpack('<II', r.get('BOD2'))[1], kind)
            self.assertEqual(struct.unpack('<I', r.get('DNAM'))[0], stats['armor'] * 100)
            ratings[subclass] = stats['armor']
        self.assertEqual(ratings[8], 0)   # an idol is no armour
        self.assertGreaterEqual(ratings[4], ratings[3])
        self.assertGreaterEqual(ratings[3], ratings[2])
        self.assertGreaterEqual(ratings[2], ratings[1])
        for inv, slot in B.SLOTS.items():
            base['InventoryType'] = inv
            r = B.armor(base, templates, 0xFFF, B.skyrim_stats(base, templates))
            self.assertEqual(struct.unpack('<II', r.get('BOD2'))[0], 0b11 if slot == 30 else 1 << (slot - 30))   # a helm: head and hair
        base.update({'class': 2, 'delay': 2600, 'dmg_min1': 40, 'dmg_max1': 80})
        for subclass in B.WEAPONS:
            base['subclass'] = subclass
            stats = B.skyrim_stats(base, templates)
            r = B.weapon(base, templates, 0xFFF, stats)
            self.assertEqual(struct.unpack('<IfH', r.get('DATA'))[2], stats['damage'])
            self.assertEqual(struct.unpack_from('<f', r.get('DNAM'), 4)[0], 1)
            self.assertEqual(struct.unpack_from('<H', r.get('CRDT'))[0], round(stats['damage'] / 2))

    def test_every_armor_trains_a_skill_and_has_matching_material(self):
        for entry, item in self.catalog['items'].items():
            r = self.item(entry)
            if r.type != 'ARMO':
                continue
            heavy = item['class'] == 4 and item['subclass'] in (3, 4, 6)
            self.assertEqual(struct.unpack('<II', r.get('BOD2'))[1], int(heavy), entry)
            kw = set(struct.unpack('<' + 'I' * (len(r.get('KWDA')) // 4), r.get('KWDA')))
            self.assertIn(0x6BBD2 if heavy else 0x6BBD3, kw, entry)
            templates = {r.edid: r for r in esm.read(B.MASTER, {'KYWD'})['KYWD']}
            self.assertIn(templates['ArmorMaterial' + B.crafting_material(item)[0]].form, kw, entry)
            self.assertNotIn(0x6BBD1, kw, entry)  # ArmorClothing

    def test_sets_and_iconic_weapon_conversion(self):
        for key, group in self.catalog['equipment_sets'].items():
            self.assertEqual({k: self.gear['sets'][key][k] for k in group}, group)
            for bonus in self.gear['sets'][key]['bonuses']:
                if 'ability' in bonus:
                    self.assertTrue(self.effects(self.records[0x01000000 | bonus['ability']]))
        for group in self.gear['sets'].values():
            for entry in group['entries']:
                row = self.gear['items'][str(entry)]
                self.assertEqual(row['set']['id'], group['id'])
                self.assertEqual([r['entry'] for r in row['set']['items']], group['entries'])
                for field in ('displayid', 'InventoryType', 'class', 'subclass', 'sheath', 'Material', 'delay'):
                    self.assertEqual(row[field], self.catalog['items'][str(entry)][field])
        for entry in (19019, 17182, 19364, 18842, 18713, 18715, 18608, 18609, 18816, 18348):
            r, item = self.item(entry), self.catalog['items'][str(entry)]
            self.assertEqual(r.type, 'WEAP')
            self.assertEqual(struct.unpack('<IfH', r.get('DATA'))[2], self.gear['items'][str(entry)]['skyrim']['damage'])
            self.assertAlmostEqual(struct.unpack_from('<f', r.get('DNAM'), 4)[0], 2600 / item['delay'], places=6)

    def test_material_policy_boundaries_and_enchanter_slot_restrictions(self):
        # A metal weapon tempers with its tier's alloy: ItemLevel 59 is Skyrim level 35 (Glass), 60 is 36 (Ebony).
        item = {'class': 2, 'subclass': 7, 'Material': 1, 'ItemLevel': 5}
        self.assertEqual(B.crafting_material(item), ('Iron', 'IngotIron', None))
        item['ItemLevel'] = 59
        self.assertEqual(B.crafting_material(item), ('Glass', 'IngotMalachite', 'GlassSmithing'))
        item['ItemLevel'] = 60
        self.assertEqual(B.crafting_material(item), ('Ebony', 'IngotEbony', 'EbonySmithing'))
        item['ItemLevel'] = 76
        self.assertEqual(B.crafting_material(item), ('Daedric', 'IngotEbony', 'DaedricSmithing'))
        item['Material'] = 2
        self.assertEqual(B.crafting_material(item), ('Wood', 'LeatherStrips', None))
        item.update({'class': 4, 'subclass': 4, 'ItemLevel': 60})
        self.assertEqual(B.crafting_material(item), ('SteelPlate', 'IngotCorundum', 'AdvancedArmors'))
        item.update({'subclass': 3, 'ItemLevel': 70})
        self.assertEqual(B.crafting_material(item), ('Orcish', 'IngotOrichalcum', 'OrcishSmithing'))
        item['subclass'] = 2
        self.assertEqual(B.crafting_material(item), ('Leather', 'Leather01', None))
        # Every actual armour item matches at least one vanilla base enchantment
        # restriction, including nonstandard WoW biped slots.
        master = esm.read(B.MASTER, {'ENCH', 'FLST'})
        lists = {r.form: {struct.unpack('<I', d)[0] for d in r.all('LNAM')} for r in master['FLST']}
        restrictions = [lists[struct.unpack_from('<I', r.get('ENIT'), 32)[0]]
                        for r in master['ENCH'] if r.edid.startswith('EnchArmor') and
                        len(r.get('ENIT')) >= 36 and struct.unpack_from('<I', r.get('ENIT'), 32)[0] in lists]
        self.assertTrue(restrictions)
        for entry in self.gear['items']:
            r = self.item(entry)
            if r.type == 'ARMO':
                kw = set(struct.unpack('<' + 'I' * (len(r.get('KWDA')) // 4), r.get('KWDA')))
                self.assertTrue(any(kw & restriction for restriction in restrictions), entry)

    def test_crafting_recipes_and_enchantability(self):
        master = {r.edid: r for rs in esm.read(B.MASTER, {'COBJ', 'PERK', 'MISC', 'KYWD'}).values() for r in rs}
        recipes = {r.edid: r for r in self.groups['COBJ']}
        self.assertEqual(len(recipes), len(self.gear['items']))
        for entry, item in self.catalog['items'].items():
            r = recipes['WoWTemper' + entry]
            material, ingredient, perk = B.crafting_material(item)
            self.assertEqual(r.get('CNAM'), B.u32(self.item(entry).form))
            self.assertEqual(r.get('CNTO'), struct.pack('<II', master[ingredient].form, 1))
            station = 'CraftingSmithingSharpeningWheel' if item['class'] == 2 else 'CraftingSmithingArmorTable'
            self.assertEqual(r.get('BNAM'), B.u32(master[station].form))
            self.assertEqual(r.get('NAM1'), struct.pack('<H', 1))
            vanilla = master['TemperWeaponIronSword' if item['class'] == 2 else 'TemperArmorLeatherCuirass']
            self.assertEqual(r.all('CTDA')[:2], vanilla.all('CTDA'))
            self.assertEqual(len(r.all('CTDA')), 3 if perk else 2)
            if perk:
                gate = r.all('CTDA')[2]
                self.assertEqual(len(gate), 32)
                self.assertEqual(struct.unpack_from('<H', gate, 8)[0], 448)
                self.assertEqual(struct.unpack_from('<I', gate, 12)[0], master[perk].form)
                self.assertEqual(gate[0], 0)
            self.assertIsNone(self.item(entry).get('EITM'))
            self.assertNotIn(B.u32(0xC27BD), self.item(entry).get('KWDA'))
            if item['class'] == 2:
                self.assertIn(B.u32(master['WeapMaterial' + material].form), self.item(entry).get('KWDA'))

    def test_every_item_is_as_good_or_a_little_better_than_its_tier(self):
        # The best vanilla piece of its slot by its Skyrim level, read from the records here; shoulders, waist, legs,
        # wrists and back share 12.5% of the four-piece total by WoW's ratios (Tier 1 plate: 562/749 shoulders ...
        # 328/749 wrists; a cloak .6), which comes to 3.225 chests.
        master = {r.edid: r for rs in esm.read(B.MASTER, {'ARMO', 'WEAP'}).values() for r in rs}
        shares = {slot: share / 16 / 3.225 * .125 for slot, share in ((3, 12), (6, 9), (7, 14), (9, 7))}
        parts = {1: 'Helmet', 5: 'Cuirass', 20: 'Cuirass', 10: 'Gauntlets', 8: 'Boots', 14: 'Shield'}
        classes = {1: .6, 2: 1, 3: .9, 4: 1, 6: 1}

        def rating(level, tiers, part):
            return max(struct.unpack('<I', r.get('DNAM'))[0] / 100 for at, m in tiers if at <= level
                       for r in [master.get(f'Armor{m}{part}') or master.get(f'Armor{m}{part}A')] if r)
        checked = 0
        for entry, item in self.catalog['items'].items():
            r, skyrim = self.item(entry), self.gear['items'][entry]['skyrim']
            level = skyrim['level']
            self.assertEqual(level, B.skyrim_level(item['ItemLevel']))
            if item['class'] == 2:
                kind = B.WEAPONS[item['subclass']]
                rate = max(struct.unpack('<IfH', w.get('DATA'))[2] * struct.unpack_from('<f', w.get('DNAM'), 4)[0]
                           for at, m in B.ARMS if at <= level
                           for w in [master.get(m + kind.removeprefix('Iron').removeprefix('Hunting')) or
                                     (master[kind] if m == 'Iron' else None)] if w)
                expected, actual = rate * item['delay'] / 2600, struct.unpack('<IfH', r.get('DATA'))[2]
                self.assertEqual(actual, skyrim['damage'], entry)
            elif item['armor']:
                tiers = B.HEAVY if item['subclass'] in (3, 4, 6) else B.LIGHT
                slot = item['InventoryType']
                base = rating(level, tiers, parts[slot]) if slot in parts else \
                    sum(rating(level, tiers, part) for part in ('Cuirass', 'Helmet', 'Gauntlets', 'Boots')) * shares[slot]
                expected, actual = base * classes[item['subclass']], struct.unpack('<I', r.get('DNAM'))[0] / 100
                self.assertEqual(actual, skyrim['armor'], entry)
            else:
                self.assertEqual(skyrim['armor'], 0)   # idols
                continue
            self.assertGreaterEqual(actual, expected - .5, entry)          # whole-number ratings and damage
            self.assertLessEqual(actual, expected * 1.11 + .5, entry)
            checked += 1
        self.assertEqual(checked, 93)
        # A whole WoW set is a little strong, not overwhelmingly so: 1.10-1.25x the Skyrim set of its tier (class
        # factor applied), these epic sets near the top.
        for key, group in self.gear['sets'].items():
            item = self.catalog['items'][str(group['entries'][0])]
            tiers = B.HEAVY if item['subclass'] in (3, 4) else B.LIGHT
            level = self.gear['items'][str(group['entries'][0])]['skyrim']['level']
            skyrim = sum(rating(level, tiers, part) for part in ('Cuirass', 'Helmet', 'Gauntlets', 'Boots')) * classes[item['subclass']]
            ratio = sum(self.gear['items'][str(e)]['skyrim']['armor'] for e in group['entries']) / skyrim
            self.assertTrue(1.18 <= ratio <= 1.25, (key, ratio))

    def test_tiers_are_vanilla_leveled_list_levels(self):
        master = esm.read(B.MASTER, {'ARMO', 'WEAP', 'LVLI'})
        names = {r.form: r.edid for rs in master.values() for r in rs}
        lists = {r.edid.lower(): r for r in master['LVLI']}

        def entries(*edids):   # (level, item), a sublist by its first item
            out = set()
            for edid in edids:
                for d in lists[edid.lower()].all('LVLO'):
                    level, _, form = struct.unpack_from('<HHI', d)
                    while names[form].lower() in lists:
                        form = struct.unpack_from('<HHI', lists[names[form].lower()].get('LVLO'))[2]
                    out.add((level, names[form]))
            return out
        known = {n.lower() for n in names.values()}
        for tiers, weight in ((B.HEAVY, 'Heavy'), (B.LIGHT, 'Light')):
            for part in ('Cuirass', 'Helmet', 'Gauntlets', 'Boots', 'Shield'):
                given = entries(f'LItemArmor{part}{weight}', f'LItemArmor{part}{weight}Special')
                for at, m in tiers[1:]:   # level 1 mixes Iron, Banded Iron and Steel; Hide and Studded
                    piece = f'armor{m}{part}'.lower()
                    if {piece, piece + 'a'} & known:   # Steel's are A/B variants
                        self.assertTrue(any(level == at and item.lower().startswith(piece) for level, item in given), (m, part))
        for kind in ('WarAxe', 'BattleAxe', 'Mace', 'Warhammer', 'GreatSword', 'Sword', 'Dagger', 'Bow'):
            given = {(level, item.lower()) for level, item in entries(f'LItemWeapon{kind}', f'LItemWeapon{kind}Special')}
            for at, m in B.ARMS[1:]:   # Steel at 2 from the plain lists, the rest as the Special ones give them
                if (m + kind).lower() in known:
                    self.assertIn((at, (m + kind).lower()), given, (m, kind))
        tiers = {}
        for d in lists['litemencharmorheavycuirass'].all('LVLO'):
            level, _, form = struct.unpack_from('<HHI', d)
            tiers.setdefault(int(re.search(r'Cuirass0(\d)', names[form], re.I)[1]), level)
        self.assertEqual(tiers, {n: at for at, n in B.ENCHANTMENTS})

    def test_native_bonuses_stay_within_vanilla_enchantments(self):
        caps = B.enchantment_caps({r.edid: r for r in esm.read(B.MASTER, {'ENCH'})['ENCH']})
        self.assertEqual(sorted(caps[0x49504].items()), [(1, 20), (2, 30), (3, 40), (4, 50), (5, 60), (6, 70)])
        checked = 0
        for entry, row in self.gear['items'].items():
            tier = max(n for at, n in B.ENCHANTMENTS if at <= row['skyrim']['level'])
            for effect, magnitude in self.effects(self.records[0x01000000 | row['ability']]).items() if 'ability' in row else ():
                if effect in caps:
                    allowed = caps[effect][max((n for n in caps[effect] if n <= tier), default=min(caps[effect]))]
                    self.assertLessEqual(magnitude, allowed + 1e-4, (entry, hex(effect)))
                    checked += 1
        self.assertGreater(checked, 200)

    def test_id_budget(self):
        path = self.root / 'full.json'
        path.write_text(json.dumps({'reserved': 0xFFF}))
        with self.assertRaisesRegex(ValueError, 'ESL budget'):
            B.Forms(path).get('new')

    def test_shared_conversion_tables_and_vanilla_armor_effects(self):
        self.assertIs(B.STAT, skyrim_effects.STAT)
        self.assertIs(B.RESISTS, skyrim_effects.RESISTS)
        master = {r.form: r for r in esm.read(B.MASTER, {'MGEF'})['MGEF']}
        for form, edid, detrimental in (
                (skyrim_effects.ARMOR_BUFF, 'doomLordDamageEffect', False),
                (skyrim_effects.ARMOR_DEBUFF, 'VoiceMarkedForDeathArmorEffect01', True)):
            record = master[form]
            self.assertEqual(record.edid, edid)
            data = record.get('DATA')
            self.assertEqual(struct.unpack_from('<I', data, 64)[0], 0)  # Value Modifier
            self.assertEqual(struct.unpack_from('<i', data, 68)[0], 39)  # DamageResist
            self.assertEqual(bool(struct.unpack_from('<I', data)[0] & 4), detrimental)


class TierConversionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.templates = {r.edid: r for rs in esm.read(B.MASTER, {'ARMO', 'WEAP', 'ENCH'}).values() for r in rs}

    def test_item_levels_map_to_skyrim_levels(self):
        for item_level, level in ((0, 1), (5, 1), (20, 8), (35, 18), (40, 21), (50, 27), (59, 35), (60, 36),
                                  (66, 40), (71, 44), (76, 48), (80, 52), (92, 52)):
            self.assertEqual(B.skyrim_level(item_level), level, item_level)

    def test_classes_keep_their_order_and_everything_rises_with_tier(self):
        for slot in (1, 3, 5, 6, 7, 8, 9, 10, 14, 16, 20):
            last = None
            for item_level in range(1, 90, 2):
                stats = {sub: B.skyrim_stats({'class': 4, 'subclass': sub, 'InventoryType': slot, 'armor': 100,
                                              'Quality': 2, 'ItemLevel': item_level, 'SellPrice': 100}, self.templates)
                         for sub in (1, 2, 3, 4)}
                ratings = [stats[sub]['armor'] for sub in (4, 3, 2, 1)]   # plate >= mail >= leather >= cloth
                self.assertEqual(ratings, sorted(ratings, reverse=True), (slot, item_level))
                for sub, now in stats.items() if last else ():
                    self.assertGreaterEqual(now['armor'], last[sub]['armor'], (slot, sub, item_level))
                    self.assertGreaterEqual(now['value'], last[sub]['value'], (slot, sub, item_level))
                last = stats
        for subclass in B.WEAPONS:
            last = None
            for item_level in range(1, 90, 2):
                now = B.skyrim_stats({'class': 2, 'subclass': subclass, 'InventoryType': 17, 'delay': 2600, 'Quality': 2,
                                      'ItemLevel': item_level, 'SellPrice': 100}, self.templates)
                if last:
                    self.assertGreaterEqual(now['damage'], last['damage'], (subclass, item_level))
                    self.assertGreaterEqual(now['value'], last['value'], (subclass, item_level))
                last = now
        # Quality and the item's strength in its band: grey .85 .. legendary 1.11, never above 1.11.
        chest = {'class': 4, 'subclass': 4, 'InventoryType': 5, 'armor': 100, 'ItemLevel': 80, 'SellPrice': 1}
        self.assertEqual([B.skyrim_stats({**chest, 'Quality': q}, self.templates)['armor'] for q in range(6)],
                         [round(49 * f) for f in (.85, .925, 1, 1.05, 1.1, 1.11)])
        self.assertEqual(B.skyrim_stats({**chest, 'Quality': 4}, self.templates, 1.1)['armor'], round(49 * 1.11))
        self.assertEqual(B.skyrim_stats({**chest, 'SellPrice': 0}, self.templates)['value'], 1)   # unsellable in WoW

    def test_a_full_outfit_is_a_little_strong_not_overwhelmingly(self):
        # Eight WoW pieces, and with a cloak: green near 1.10x the Skyrim set of its tier, epic/legendary near 1.25x.
        def outfit(sub, quality, item_level, slots):
            return sum(B.skyrim_stats({'class': 4, 'subclass': sub, 'InventoryType': slot, 'armor': 100, 'Quality': quality,
                                       'ItemLevel': item_level, 'SellPrice': 1}, self.templates)['armor'] for slot in slots)
        eight = (1, 3, 5, 6, 7, 8, 9, 10)
        for sub, item_level in ((4, 80), (4, 66), (3, 76), (2, 76), (2, 66)):
            skyrim = outfit(sub, 2, item_level, (1, 5, 8, 10)) / B.QUALITY_FACTORS[2]
            for quality, low, high in ((2, 1.08, 1.15), (3, 1.13, 1.2), (4, 1.18, 1.25), (5, 1.19, 1.26)):
                for slots in (eight, eight + (16,)):
                    ratio = outfit(sub, quality, item_level, slots) / skyrim
                    self.assertTrue(low <= ratio <= high, (sub, item_level, quality, len(slots), ratio))

    def test_variation_within_a_band_is_capped(self):
        # Per chest share: 200, 200 (a 162.5 head), 274 (a 120 wrist), 170, 190; another item level is its own band.
        band = {'class': 4, 'subclass': 2, 'Quality': 4, 'ItemLevel': 66}
        items = {'1': {**band, 'InventoryType': 5, 'armor': 200}, '2': {**band, 'InventoryType': 1, 'armor': 162.5},
                 '3': {**band, 'InventoryType': 9, 'armor': 120}, '4': {**band, 'InventoryType': 20, 'armor': 170},
                 '5': {**band, 'InventoryType': 5, 'armor': 190}, '6': {**band, 'ItemLevel': 70, 'InventoryType': 5, 'armor': 1}}
        v = B.variations(items)
        self.assertEqual([v[k] for k in '12346'], [1, 1, 1.1, .9, 1])
        self.assertAlmostEqual(v['5'], .95)

    def test_the_services_weapon_skill_scaling_is_skyrims(self):
        from service.skyrim_balance import DAMAGE_PC_SKILL_MIN, DAMAGE_PC_SKILL_MAX
        gmst = {r.edid: struct.unpack('<f', r.get('DATA')[:4])[0] for r in esm.read(B.MASTER, {'GMST'})['GMST']
                if r.edid.startswith('fDamagePCSkill')}
        self.assertEqual(gmst, {'fDamagePCSkillMax': DAMAGE_PC_SKILL_MAX})   # Min keeps the engine's default
        self.assertEqual(DAMAGE_PC_SKILL_MIN, 1.0)

    def test_native_bonuses_are_capped_at_vanilla_enchantments(self):
        caps = B.enchantment_caps(self.templates)
        # Level 8 has vanilla's 03 enchantments (Fortify Magicka 40; Magicka Rate starts at 03, 40%); 52 has 06.
        self.assertEqual(B.capped([(0x49504, 150), (0x7A0FD, 100), (0x49504, 12), (0xE5F55, 500)], 8, caps),
                         [(0x49504, 40), (0x7A0FD, 40), (0x49504, 12), (0xE5F55, 500)])
        self.assertEqual(B.capped([(0x49504, 150), (0x7A0FD, 100)], 1, caps), [(0x49504, 20), (0x7A0FD, 40)])
        self.assertEqual(B.capped([(0x49504, 150), (0x7A0FD, 150)], 52, caps), [(0x49504, 70), (0x7A0FD, 100)])


class GearAdaptationTests(unittest.TestCase):
    def test_all_native_adaptation_ids_have_the_intended_actor_values(self):
        master = {r.form: r for r in esm.read(B.MASTER, {'MGEF'})['MGEF']}
        names = {0x49504: 'EnchFortifyMagickaConstantSelf', 0x493AA: 'EnchFortifyHealthConstantSelf',
                 0x7A0FD: 'EnchFortifyMagickaRateConstantSelf', 0x7A0F8: 'EnchFortifyHealRateConstantSelf',
                 0x7A105: 'EnchFortifyStaminaRateConstantSelf', 0x7A0F3: 'EnchFortifyBlockConstantSelf',
                 0x7A0FF: 'EnchFortifyOneHandedConstantSelf', 0x7A106: 'EnchFortifyTwoHandedConstantSelf',
                 0x7A0FE: 'EnchFortifyArcheryConstantSelf', 0x7A101: 'EnchFortifyRestorationConstantSelf',
                 0x7A0F6: 'EnchFortifyDestructionConstantSelf', 0x7A103: 'EnchFortifySneakConstantSelf',
                 0x49507: 'EnchFortifyStaminaConstantSelf', 0xB7A35: 'EnchResistMagicConstantSelf'}
        for adaptation in B.SET_ADAPTATIONS.values():
            self.assertTrue(adaptation['reason'])
            for form, value in adaptation['effects']:
                self.assertGreater(value, 0)
                self.assertEqual(master[form].edid, names[form])

    def test_client_item_use_export_is_real_and_has_shared_category_cooldown(self):
        gear = json.loads((B.REPO / 'build/gear.json').read_text())['items']
        # The staves share category 1031's 30 minutes (CMaNGOS Player::AddCooldown); cooldown_ms stays the longest timer.
        for item, spell, other in (('18608', 23041, 18609), ('18609', 23042, 18608)):
            use = gear[item]['use']
            self.assertEqual((use['spell'], use['entry'], use['cooldown_ms']), (spell, other, 1800000))
            self.assertEqual((use['category'], use['category_cooldown_ms']), (1031, 1800000))


if __name__ == '__main__':
    unittest.main()
