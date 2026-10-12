"""Read-back checks against the real Skyrim master and client spell descriptions."""
import copy
import json
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
        cls.groups = esm.read(cls.path, {'ARMO', 'WEAP', 'ENCH', 'SPEL', 'MGEF', 'COBJ'})
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
            if r.type == 'ARMO':
                self.assertIsNone(r.get('MODL'))

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

    def test_stormrage(self):
        r = self.item(16897)
        self.assertEqual(r.type, 'ARMO')
        self.assertEqual(struct.unpack('<II', r.get('BOD2')), (1 << 2, 0))
        self.assertEqual(struct.unpack('<I', r.get('DNAM'))[0], round(225 / 8) * 100)
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
        self.assertEqual(struct.unpack('<IfH', r.get('DATA'))[2], round(149.5 * .25))
        self.assertAlmostEqual(struct.unpack_from('<f', r.get('DNAM'), 4)[0], 2.6 / 3.7, places=6)
        self.assertEqual(r.get('MODL'), esm.zstring(r'Weapons\Staff01\Staff01.nif'))
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
        for subclass, kind, divisor in ((1, 0, 15), (2, 0, 8), (3, 1, 12), (4, 1, 30), (6, 1, 30), (8, 0, 15)):
            base['subclass'] = subclass
            r = B.armor(base, templates, 0xFFF)
            self.assertEqual(struct.unpack('<II', r.get('BOD2'))[1], kind)
            self.assertEqual(struct.unpack('<I', r.get('DNAM'))[0], round(225 / divisor) * 100)
        for inv, slot in B.SLOTS.items():
            base['InventoryType'] = inv
            r = B.armor(base, templates, 0xFFF)
            self.assertEqual(struct.unpack('<II', r.get('BOD2'))[0], 0b11 if slot == 30 else 1 << (slot - 30))   # a helm: head and hair
        base.update({'class': 2, 'delay': 2600, 'dmg_min1': 40, 'dmg_max1': 80})
        for subclass in B.WEAPONS:
            base['subclass'] = subclass
            r = B.weapon(base, templates, 0xFFF)
            self.assertEqual(struct.unpack('<IfH', r.get('DATA'))[2], 15)
            self.assertEqual(struct.unpack_from('<f', r.get('DNAM'), 4)[0], 1)

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
            self.assertEqual(struct.unpack('<IfH', r.get('DATA'))[2], round((item['dmg_min1'] + item['dmg_max1']) / 8))
            self.assertAlmostEqual(struct.unpack_from('<f', r.get('DNAM'), 4)[0], 2600 / item['delay'], places=6)

    def test_material_policy_boundaries_and_enchanter_slot_restrictions(self):
        item = {'class': 2, 'subclass': 7, 'Material': 1, 'ItemLevel': 59}
        self.assertEqual(B.crafting_material(item), ('Steel', 'IngotSteel', 'SteelSmithing'))
        item['ItemLevel'] = 60
        self.assertEqual(B.crafting_material(item), ('Dwarven', 'IngotDwarven', 'DwarvenSmithing'))
        item['ItemLevel'] = 70
        self.assertEqual(B.crafting_material(item), ('Ebony', 'IngotEbony', 'EbonySmithing'))
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

    def test_id_budget(self):
        path = self.root / 'full.json'
        path.write_text(json.dumps({'reserved': 0xFFF}))
        with self.assertRaisesRegex(ValueError, 'ESL budget'):
            B.Forms(path).get('new')

    def test_shared_conversion_tables_and_vanilla_armor_effects(self):
        self.assertIs(B.STAT, skyrim_effects.STAT)
        self.assertIs(B.RESISTS, skyrim_effects.RESISTS)
        self.assertIs(B.ARMOR_DIVISORS, skyrim_effects.ARMOR_DIVISORS)
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
        self.assertEqual(gear['18608']['use'], {'spell': 23041, 'entry': 18609, 'cooldown_ms': 1800000})
        self.assertEqual(gear['18609']['use'], {'spell': 23042, 'entry': 18608, 'cooldown_ms': 1800000})


if __name__ == '__main__':
    unittest.main()
