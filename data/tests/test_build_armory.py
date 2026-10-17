"""Read-back checks of WoWBridgeArmory.esp against the ClassicDB dump, the Skyrim master and WoWBridge.esp's gear."""
import collections
import json
import shutil
import struct
import tempfile
import time
import unittest
from pathlib import Path

from data import build_armory as A, build_esp as B, esm
from data.build_catalog import CORE_SPELLS, DBC_DIR, DEFAULT_DUMP

SPELL_DBC = B.REPO / 'build/dbc/DBFilesClient/Spell.dbc'
GEAR, LOOKS = B.REPO / 'build/gear.json', B.REPO / 'data/skyrim_gear_looks.json'


def entries(r):
    return [struct.unpack('<HHIHH', d) for d in r.all('LVLO')]


def biped(r):
    """An ARMO's or ARMA's biped slots, read here as Skyrim writes them (Skyrim.esm's addons have legacy BODT)."""
    return struct.unpack_from('<I', r.get('BOD2') or r.get('BODT'))[0]


class ArmoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.forms = cls.root / 'forms.json'
        # The persisted IDs, never changed by a test build.
        shutil.copyfile(B.REPO / 'data/armory_forms.json', cls.forms)
        cls.original = dict(B.Forms(cls.forms, last=0xFFFFFF).ids)
        cls.sql, cls.templates, cls.addons = A.read_inputs(DEFAULT_DUMP, B.MASTER)
        cls.arma = {r.form: r for r in esm.read(B.MASTER, {'ARMA'})['ARMA']}
        cls.build()
        cls.path = cls.root / 'esp' / A.PLUGIN
        cls.groups = esm.read(cls.path, {'ARMO', 'WEAP', 'SPEL', 'MGEF', 'COBJ', 'LVLI'})
        cls.records = {r.form: r for rs in cls.groups.values() for r in rs}
        cls.armory = json.loads((cls.root / 'armory.json').read_text())
        cls.service = json.loads((cls.root / 'armory_catalog.json').read_text())
        cls.rows = {r['entry']: r for r in cls.sql['item_template']}
        cls.light = json.loads(GEAR.read_text())
        cls.lists = {r.edid: r for r in cls.groups['LVLI']}

    @classmethod
    def build(cls):
        return A.build(cls.sql, cls.templates, cls.addons, SPELL_DBC, DBC_DIR, CORE_SPELLS, GEAR, LOOKS, cls.forms, cls.root)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def record(self, entry):
        return self.records[A.FILE << 24 | self.armory['items'][str(entry)]['form']]

    def effects(self, magic):
        return [(struct.unpack('<I', fid)[0], struct.unpack('<fII', fit)[0]) for fid, fit in zip(magic.all('EFID'), magic.all('EFIT'))]

    def test_selection_rules(self):
        items = self.armory['items']
        # In: what a vendor sells, a recipe makes, a pocket or a fishing pool holds, a quest rewards, a new character wears,
        # a raid drops, and the class legendaries.
        for entry in (85, 2300, 1786, 6360, 2059, 6123, 3661, 19375, 22589, 22630):
            self.assertIn(str(entry), items, entry)
        # Out: monster weapons, test and deprecated rows, an expiring raid weapon, profession tools, a quest's own
        # implement, WoWBridge.esp's items, rows nothing gives, and an armour row in a weapon's slot.
        for entry in (143, 2189, 3895, 7809, 22736, 6256, 2901, 2944, 19019, 16897, 913, 10515):
            self.assertNotIn(str(entry), items, entry)
        equippable = [r for r in self.sql['item_template'] if r['class'] in (2, 4) and r['InventoryType']]
        selection = self.armory['selection']
        self.assertEqual(len(items), 8521)
        self.assertEqual(len(items) + sum(selection['excluded'].values()), len(equippable))
        self.assertEqual(selection['excluded']['already in WoWBridge.esp'], len(self.light['items']))
        for kind in ('slot', 'class', 'quality'):
            self.assertEqual(sum(selection['counts'][kind].values()), len(items))
        self.assertEqual(selection['counts']['quality']['5'], 4)   # the four Atiesh
        self.assertFalse({int(e) for e in items} & {int(e) for e in self.light['items']})

    def test_header_masters_and_ids(self):
        raw = self.path.read_bytes()
        self.assertEqual(raw[:4], b'TES4')
        self.assertEqual(struct.unpack_from('<I', raw, 8)[0], 0)   # a regular plugin, not light
        header = esm._subrecords(raw[24:24 + struct.unpack_from('<I', raw, 4)[0]])
        self.assertEqual([d for t, d in header if t == 'MAST'], [b'Skyrim.esm\0', b'WoWBridge.esp\0'])
        _, count, next_id = struct.unpack('<fII', dict(header)['HEDR'])
        records = [r for rs in self.groups.values() for r in rs]
        self.assertEqual(count, len(records))
        self.assertEqual(len(self.records), len(records))   # no two share an ID
        for r in records:
            self.assertEqual(r.form >> 24, A.FILE, r.edid)
            self.assertTrue(0x800 <= r.form & 0xFFFFFF < next_id)
        ids = B.Forms(self.forms, last=0xFFFFFF).ids
        self.assertEqual(sorted(ids.values()), sorted(r.form & 0xFFFFFF for r in records))
        light = {0x01000000 | row['form'] for row in self.light['items'].values()}
        for r in self.groups['LVLI']:
            for level, _, form, count, _ in entries(r):
                self.assertTrue(form in self.records or form in light, (r.edid, hex(form)))
                self.assertEqual(count, 1)

    def test_items_carry_their_identity_and_stats(self):
        for entry, row in self.armory['items'].items():
            r, item = self.record(entry), self.rows[int(entry)]
            self.assertEqual((r.type, r.edid), (row['kind'], f'WoWItem{entry}'))
            self.assertEqual(r.get('FULL'), esm.zstring(item['name']))
            self.assertEqual({**A.column_defaults(item), **row['wow']}, {**item, 'RequiredLevel': 0})
            self.assertEqual(row['skyrim']['level'], B.skyrim_level(item['ItemLevel']))
            self.assertIsNone(r.get('CTDA'))
            if r.type == 'ARMO':
                self.assertEqual(struct.unpack('<I', r.get('DNAM'))[0], row['skyrim']['armor'] * 100)
                self.assertEqual(struct.unpack('<If', r.get('DATA'))[0], row['skyrim']['value'])
            else:
                value, _, damage = struct.unpack('<IfH', r.get('DATA'))
                self.assertEqual((value, damage), (row['skyrim']['value'], row['skyrim']['damage']))
                self.assertAlmostEqual(struct.unpack_from('<f', r.get('DNAM'), 4)[0], 2600 / item['delay'], places=5)

    def test_every_item_is_its_tier_by_quality(self):
        # The best vanilla piece of its slot by its Skyrim level, as test_build_esp measures WoWBridge.esp: shoulders,
        # waist, legs, wrists and back share 12.5% of the four-piece total. Quality and its band's spread (+-10%) move it
        # from .85 (grey) to never more than 1.11.
        master = {r.edid: r for rs in esm.read(B.MASTER, {'ARMO', 'WEAP'}).values() for r in rs}
        shares = {slot: share / 16 / 3.225 * .125 for slot, share in ((3, 12), (6, 9), (7, 14), (9, 7), (16, 9.6))}
        parts = {1: 'Helmet', 5: 'Cuirass', 20: 'Cuirass', 10: 'Gauntlets', 8: 'Boots', 14: 'Shield'}

        def rating(level, tiers, part):
            return max(struct.unpack('<I', r.get('DNAM'))[0] / 100 for at, m in tiers if at <= level
                       for r in [master.get(f'Armor{m}{part}') or master.get(f'Armor{m}{part}A')] if r)
        checked = collections.Counter()
        for entry, row in self.armory['items'].items():
            item, skyrim = self.rows[int(entry)], row['skyrim']
            level, quality = skyrim['level'], min(5, item['Quality'])
            if item['class'] == 2:
                kind = B.WEAPONS[item['subclass']].removeprefix('Iron').removeprefix('Hunting')
                rate = max(struct.unpack('<IfH', w.get('DATA'))[2] * struct.unpack_from('<f', w.get('DNAM'), 4)[0]
                           for at, m in B.ARMS if at <= level
                           for w in [master.get(m + kind) or (master[B.WEAPONS[item['subclass']]] if m == 'Iron' else None)] if w)
                expected, actual = rate * item['delay'] / 2600, skyrim['damage']
            elif skyrim['tier']:
                tiers = B.HEAVY if item['subclass'] in (3, 4, 6) else B.LIGHT
                slot = item['InventoryType']
                base = rating(level, tiers, parts[slot]) if slot in parts else \
                    sum(rating(level, tiers, part) for part in ('Cuirass', 'Helmet', 'Gauntlets', 'Boots')) * shares[slot]
                expected, actual = base * B.CLASS_FACTORS[item['subclass']], skyrim['armor']
            else:
                self.assertEqual(skyrim['armor'], 0)   # jewelry, trinkets, shirts, off-hand frills, relics
                continue
            low, high = B.QUALITY_FACTORS[quality] * .9, min(1.11, B.QUALITY_FACTORS[quality] * 1.1)
            self.assertGreaterEqual(actual, max(1, expected * low - .5), entry)
            self.assertLessEqual(actual, max(1, expected * high + .5), entry)
            checked[quality] += 1
        self.assertGreater(sum(checked.values()), 7000)
        self.assertTrue(all(checked[q] > 0 for q in range(6)), checked)
        # Samples: Lionheart Helm (plate, ItemLevel 61: level 37, an Ebony Helmet's 21) and Judgement Crown (76: 48,
        # Daedric 23), epic; Edgemaster's Handguards (mail 49: 26, Orcish Gauntlets 10 x .9 mail).
        skyrim = lambda e: self.armory['items'][str(e)]['skyrim']
        self.assertEqual((skyrim(12640)['tier'], skyrim(12640)['armor']), ('Ebony', round(21 * 1.1)))
        self.assertEqual((skyrim(16955)['tier'], skyrim(16955)['armor']), ('Daedric', round(23 * 1.1)))
        self.assertEqual(skyrim(14551)['tier'], 'Orcish')

    def test_tier_two_sets_are_a_little_strong(self):
        # As WoWBridge.esp's Tier 1: a whole epic set 1.18-1.25x the Skyrim set of its tier (class factor applied).
        master = {r.edid: r for r in esm.read(B.MASTER, {'ARMO'})['ARMO']}

        def rating(level, tiers, part):
            return max(struct.unpack('<I', r.get('DNAM'))[0] / 100 for at, m in tiers if at <= level
                       for r in [master.get(f'Armor{m}{part}') or master.get(f'Armor{m}{part}A')] if r)
        for set_id in (210, 211, 212, 213, 215, 216, 217, 218):   # Netherwind .. Wrath (Stormrage is WoWBridge.esp's)
            group = self.armory['sets'][str(set_id)]
            first = self.rows[group['items'][0]['entry']]
            tiers = B.HEAVY if first['subclass'] in (3, 4) else B.LIGHT
            level = group['level']
            skyrim = sum(rating(level, tiers, part) for part in ('Cuirass', 'Helmet', 'Gauntlets', 'Boots')) * B.CLASS_FACTORS[first['subclass']]
            worn = sum(self.armory['items'][str(i['entry'])]['skyrim']['armor'] for i in group['items'])
            self.assertTrue(1.18 <= worn / skyrim <= 1.25, (group['name'], worn / skyrim))

    def test_shared_abilities_add_up_and_stay_within_vanilla_enchantments(self):
        caps = B.enchantment_caps(self.templates)
        copies, users = self.armory['abilities'], collections.defaultdict(list)
        for entry, row in self.armory['items'].items():
            if 'ability' in row:
                users[row['ability']].append(entry)
        self.assertEqual(set(users), set(range(len(copies))))
        self.assertLess(len(copies), len([r for r in self.armory['items'].values() if 'ability' in r]))   # shared
        for index, forms in enumerate(copies):
            first = self.records[A.FILE << 24 | forms[0]]
            self.assertEqual(first.type, 'SPEL')
            for form in forms:   # every copy is the same bonus
                self.assertEqual(self.effects(self.records[A.FILE << 24 | form]), self.effects(first))
            groups = collections.Counter()
            for e in users[index]:
                item = self.rows[int(e)]
                groups[A.slot_group(item)] += 2 if item['class'] == 2 and item['InventoryType'] in (13, 21, 22) else 1
            self.assertEqual(len(forms), sum(min(n, 2 if g == 'hands' else 1) for g, n in groups.items()))
            level = min(self.armory['items'][e]['skyrim']['level'] for e in users[index])
            tier = max(n for at, n in B.ENCHANTMENTS if at <= level)
            for effect, magnitude in self.effects(first):
                if effect in caps:
                    allowed = caps[effect][max((n for n in caps[effect] if n <= tier), default=min(caps[effect]))]
                    self.assertLessEqual(magnitude, allowed + 1e-4, (index, hex(effect)))
                else:   # its own descriptive effect, or a vanilla one without an armour enchantment
                    self.assertTrue(effect >> 24 in (0, A.FILE), hex(effect))
        # Halo of Transcendence: its stats as Skyrim effects, its +spell power described, the service's to apply.
        halo = self.records[A.FILE << 24 | copies[self.armory['items']['16921']['ability']][0]]
        described = [self.records[f] for f, _ in self.effects(halo) if f in self.records]
        self.assertTrue(described and all(r.type == 'MGEF' and b'$' not in r.get('DNAM') for r in described))

    def test_dual_wielding_one_entry_has_two_equal_ability_copies(self):
        club = self.armory['items']['10571']   # Ebony Boneclub: a unique bonus, worn once in each hand
        self.assertEqual(self.rows[10571]['InventoryType'], 13)
        copies = self.armory['abilities'][club['ability']]
        self.assertEqual(len(copies), 2)
        self.assertNotEqual(copies[0], copies[1])
        first, second = [self.records[A.FILE << 24 | local] for local in copies]
        self.assertTrue(self.effects(first))
        self.assertEqual(self.effects(first), self.effects(second))

    def test_sets_and_their_bonuses(self):
        abilities = []
        for set_id, group in self.armory['sets'].items():
            for item in group['items']:
                self.assertEqual(item['name'], self.rows[item['entry']]['name'])
            members = [i['entry'] for i in group['items'] if str(i['entry']) in self.armory['items']]
            self.assertTrue(members)
            for entry in members:
                self.assertEqual(self.armory['items'][str(entry)]['set'], int(set_id))
            for bonus in group['bonuses']:
                if 'ability' in bonus:
                    abilities.append(bonus['ability'])
                    self.assertTrue(self.effects(self.records[A.FILE << 24 | bonus['ability']]))
        transcendence = self.armory['sets']['211']
        self.assertEqual((transcendence['name'], len(transcendence['items'])), ('Vestments of Transcendence', 8))
        meditation = next(b for b in transcendence['bonuses'] if b['spell'] == 21894)
        self.assertIn('ability', meditation)   # the set adaptation from data/equipment.json
        self.assertEqual(len(abilities), len(set(abilities)))   # one ability per set and bonus, never per spell
        shared = [next(b['ability'] for b in self.armory['sets'][sid]['bonuses'] if b['spell'] == 7516)
                  for sid in ('121', '122')]
        self.assertNotEqual(*shared)
        self.assertEqual(*(self.effects(self.records[A.FILE << 24 | local]) for local in shared))

    def test_populated_use_slots_keep_zero_triggers_for_lua(self):
        hat = self.armory['items']['10030']['wow']   # Admiral's Hat: Use: Admiral's Hat
        self.assertGreater(hat['spellid_1'], 0)
        self.assertEqual(hat['spelltrigger_1'], 0)
        self.assertEqual(self.service['items']['10030']['spelltrigger_1'], 0)
        for row in (r['wow'] for r in self.armory['items'].values()):
            for n in range(1, 6):
                if row.get(f'spellid_{n}'):
                    self.assertIn(f'spelltrigger_{n}', row)

    def test_looks_by_review_or_kind_slot_and_tier(self):
        models = lambda r: [(t, d) for t, d in r.subs if t in ('MOD2', 'MOD4', 'MODL')]
        curated = collections.defaultdict(list)
        for row in sorted(json.loads(LOOKS.read_text())['items'].values(), key=lambda r: r['local_form_id']):
            if row['plugin'] == 'Skyrim.esm':
                curated[row['display_id']].append(row)
        reviewed = 0
        for entry, row in self.armory['items'].items():
            item = self.rows[int(entry)]
            look = B.look(item, row['skyrim'], self.templates, curated)
            r = self.record(entry)
            if look is not None and look.type != 'MISC':
                # An armour's worn look (an NPC's): its look's addons that fill a slot both claim.
                shared = biped(r) & biped(look) if r.type == 'ARMO' else 0
                if r.type == 'ARMO' and biped(r) & B.HEAD_SLOTS and biped(look) & B.CIRCLET:
                    shared |= B.CIRCLET   # a helm whose look is a circlet wears it
                if r.type == 'ARMO' and biped(r) & B.RING:
                    shared = 0   # 1.12 draws no ring: rings wear no addon, so two are worn at once
                self.assertEqual(models(r), [s for s in models(look) if s[0] != 'MODL' or r.type == 'WEAP' or
                                             biped(self.arma[struct.unpack('<I', s[1])[0]]) & shared], entry)
                self.assertEqual(r.get('OBND'), look.get('OBND'))
            elif r.type == 'ARMO':
                self.assertEqual(r.all('MODL'), [], entry)   # a relic's statue, a trinket's gem: nothing worn
            reviewed += any(look is self.templates.get(c['edid']) for c in curated.get(item['displayid'], ()))
        self.assertGreater(reviewed, 200)
        model = lambda edid, sub='MOD2': self.templates[edid].get(sub)
        self.assertEqual(self.record(12640).get('MOD2'), model('ArmorEbonyHelmet'))         # plate, level 37
        self.assertEqual(self.record(2059).get('MOD2'), model('ClothesMonkRobes'))          # a cloak of level 11
        self.assertEqual(self.record(16921).get('MOD2'), model('ClothesPsiijicHood'))       # a cloth crown of 48
        staff = next(e for e, row in self.armory['items'].items() if self.rows[int(e)]['class'] == 2 and
                     self.rows[int(e)]['subclass'] == 10 and B.look(self.rows[int(e)], row['skyrim'], self.templates, curated) is None)
        self.assertEqual(self.record(staff).get('MODL'), esm.zstring(r'Weapons\Iron\WarHammer.nif'))   # melee staff placeholder
        self.assertEqual(self.record(21126).get('MODL'), model('DaedricDagger', 'MODL'))    # a dagger of level 52

    def test_worn_looks_fill_only_the_items_own_slots(self):
        # A follower wears her gear in its look's addons (her own Skyrim body is never drawn): each a Skyrim.esm addon
        # over the item's own slots, so none fights another item for its slot (a cuirass's over the chest).
        seen, armour = collections.Counter(), collections.Counter()
        for entry in self.armory['items']:
            r, slot = self.record(entry), self.rows[int(entry)]['InventoryType']
            if r.type != 'ARMO':
                continue
            for d in r.all('MODL'):
                form = struct.unpack('<I', d)[0]
                self.assertEqual(form >> 24, 0, entry)   # Skyrim.esm's, the master of both plugins
                self.assertIn(form, self.arma, (entry, hex(form)))
                self.assertTrue(biped(self.arma[form]) & biped(r) or (biped(r) & B.HEAD_SLOTS and biped(self.arma[form]) & B.CIRCLET),
                                (entry, hex(form)))   # a helm with a circlet look wears the circlet
            armour[slot] += 1
            seen[slot] += bool(r.all('MODL'))
        # Seen: chests and robes, hands, feet, shields, necks and helms (the five whose reviewed look is a circlet or
        # wreath wear it in slot 42); unseen: rings (1.12 draws none), shoulders, waist, legs, wrists, back, shirts, tabards, trinkets, frills held
        # in the off hand and relics.
        for slot in (5, 20, 10, 8, 14, 2, 1):
            self.assertEqual(seen[slot], armour[slot], A.SLOT_NAMES[slot])
        for slot in (3, 6, 7, 9, 16, 4, 19, 11, 12, 23, 28):
            self.assertEqual(seen[slot], 0, A.SLOT_NAMES[slot])
        self.assertGreater(sum(seen.values()), 3200)   # 3,215 since rings (328) wear none
        # Lionheart Helm wears the Ebony Helmet; Wyrmtongue Shoulders look like a cuirass on the ground, and wear none of it.
        self.assertEqual(self.record(12640).all('MODL'), self.templates['ArmorEbonyHelmet'].all('MODL'))
        shoulders = B.look(self.rows[13358], self.armory['items']['13358']['skyrim'], self.templates, B.reviewed_looks(LOOKS))
        self.assertTrue(shoulders.edid.endswith(('Cuirass', 'CuirassA')) and shoulders.all('MODL'), shoulders.edid)
        self.assertEqual(self.record(13358).all('MODL'), [])

    def test_tempering_by_tier(self):
        recipes = {r.edid: r for r in self.groups['COBJ']}
        for entry, row in self.armory['items'].items():
            item, r = self.rows[int(entry)], self.record(entry)
            recipe = recipes.get(f'WoWTemper{entry}')
            if r.type == 'ARMO' and not row['skyrim']['armor']:
                self.assertIsNone(recipe, entry)   # nothing to improve
                continue
            material, ingredient, perk = A.material(item, row['skyrim'])
            self.assertEqual(recipe.get('CNAM'), B.u32(r.form))
            self.assertEqual(recipe.get('CNTO'), struct.pack('<II', self.templates[ingredient].form, 1))
            self.assertEqual(len(recipe.all('CTDA')), 3 if perk else 2)
            if perk:
                self.assertEqual(struct.unpack_from('<I', recipe.all('CTDA')[2], 12)[0], self.templates[perk].form)
            prefix = 'WeapMaterial' if r.type == 'WEAP' else 'ArmorMaterial'
            self.assertIn(B.u32(self.templates[prefix + material].form), r.get('KWDA'), entry)
        crown = A.material(self.rows[16955], self.armory['items']['16955']['skyrim'])   # Judgement Crown, Daedric tier
        self.assertEqual(crown, ('Daedric', 'IngotEbony', 'DaedricSmithing'))
        halo = A.material(self.rows[16921], self.armory['items']['16921']['skyrim'])    # cloth, Dragonscale tier
        self.assertEqual(halo, ('Dragonscale', 'DragonScales', 'DragonArmor'))

    def test_leveled_lists_by_level_quality_and_vendor(self):
        light = {0x01000000 | row['form']: row for row in self.light['items'].values()}
        mine = {A.FILE << 24 | row['form']: (row, self.rows[int(e)]) for e, row in self.armory['items'].items()}

        def item(form):   # (level, quality, row)
            if form in light:
                return light[form]['skyrim']['level'], light[form]['quality'], light[form]['wow']
            row, wow = mine[form]
            return row['skyrim']['level'], wow['Quality'], wow

        def contents(form, seen=()):
            r = self.records.get(form)
            if r is None or r.type != 'LVLI':   # an item, its own or WoWBridge.esp's
                yield form
            else:
                for _, _, inner, _, _ in entries(r):
                    yield from contents(inner, seen + (form,))
        for r in self.groups['LVLI']:
            self.assertLessEqual(len(entries(r)), 255, r.edid)
        legendary = self.lists['WoWArmoryLegendary']
        self.assertEqual(legendary.get('LVLD'), bytes([A.LEGENDARY_CHANCE_NONE]))
        self.assertEqual({f for _, _, f, _, _ in entries(legendary)},
                         {f for f in list(light) + list(mine) if item(f)[1] == 5})
        self.assertIn(0x01000000 | self.light['items']['19019']['form'], {f for _, _, f, _, _ in entries(legendary)})
        for name, pool in A.POOLS.items():
            top = self.lists[name]
            self.assertEqual((top.get('LVLF'), top.get('LVLD')), (b'\x02', bytes([pool.get('chance_none', 0)])))
            for at, _, sub, _, _ in entries(top):
                if sub == legendary.form:
                    self.assertGreaterEqual(at, pool['legendary'], name)
                    continue
                for form in contents(sub):
                    level, quality, wow = item(form)
                    self.assertLessEqual(level, at, (name, hex(form)))   # nothing above her level
                    self.assertIn(quality, pool['mix'], (name, hex(form)))
                    if 'kind' in pool:
                        self.assertEqual(A.vendor_kind(wow), pool['kind'], (name, hex(form)))
            self.assertEqual(any(sub == legendary.form for _, _, sub, _, _ in entries(top)), 'legendary' in pool, name)
        # Tier 1 (WoWBridge.esp's Arcanist Crown, level 40) is boss and dragon loot.
        crown = 0x01000000 | self.light['items']['16795']['form']
        self.assertIn(crown, set(contents(self.lists['WoWArmoryBoss'].form)))
        self.assertIn(crown, set(contents(self.lists['WoWArmoryDragon'].form)))
        self.assertNotIn(crown, set(contents(self.lists['WoWArmoryLoot'].form)))

    def test_boss_legendaries_can_exceed_the_rolled_level_from_40(self):
        legendary = self.lists['WoWArmoryLegendary']
        boss = self.lists['WoWArmoryBoss']
        self.assertIn((40, 0, legendary.form, 1, 0), entries(boss))
        atiesh = self.armory['items']['22589']
        self.assertGreater(atiesh['skyrim']['level'], 45)
        self.assertEqual(atiesh['wow']['Quality'], 5)
        self.assertIn(A.FILE << 24 | atiesh['form'], {f for _, _, f, _, _ in entries(legendary)})

    def test_lists_join_only_where_no_npc_wears_them(self):
        master = esm.read(B.MASTER, {'LVLI', 'NPC_', 'OTFT', 'FACT'})
        names = {r.form: r.edid for r in master['LVLI']}
        children = {r.form: [struct.unpack_from('<HHI', d)[2] for d in r.all('LVLO')] for r in master['LVLI']}
        outfits = {r.form: [struct.unpack_from('<I', d, n)[0] for d in r.all('INAM') for n in range(0, len(d), 4)] for r in master['OTFT']}
        worn, dropped = set(), set()
        for r in master['NPC_']:
            worn.update(struct.unpack_from('<I', d)[0] for d in r.all('CNTO'))
            worn.update(struct.unpack('<I', r.get(t))[0] for t in ('DOFT', 'SOFT') if r.get(t))
            dropped.update(struct.unpack_from('<I', d)[0] for d in r.all('INAM'))

        def closure(roots):
            seen, todo = set(), list(roots)
            while todo:
                form = todo.pop()
                if form not in seen:
                    seen.add(form)
                    todo += children.get(form, []) + outfits.get(form, [])
            return seen
        worn, dropped = closure(worn), closure(dropped)
        factions = {r.form: r for r in master['FACT']}
        rows = {d['name']: d for d in self.armory['distribution']}
        self.assertEqual(set(rows), set(A.POOLS))
        for name, row in rows.items():
            self.assertEqual(self.records[A.FILE << 24 | row['list']].edid, name)
            for target in row['lists']:
                self.assertIn(target['form'], names, hex(target['form']))
                self.assertNotIn(target['form'], worn, (name, names[target['form']]))
                if target['form'] in dropped:   # a corpse's: the humanoids' and dragons' death items only
                    self.assertIn(name, ('WoWArmoryDeath', 'WoWArmoryDragon'), names[target['form']])
            for target in row['vendors']:
                self.assertIsNotNone(factions[target['form']].get('VENC'), hex(target['form']))   # a merchant chest
        self.assertEqual({names[t['form']] for t in rows['WoWArmoryLoot']['lists']}, {'LItemSpecialLoot10', 'LItemSpecialLoot100'})
        self.assertTrue(all(names[t['form']].startswith('DeathItem') for t in rows['WoWArmoryDeath']['lists']))
        self.assertTrue(all('ChestBoss' in names[t['form']] for t in rows['WoWArmoryBoss']['lists']))
        self.assertEqual(factions[rows['WoWArmoryOutfitter']['vendors'][0]['form']].edid, 'ServicesSolitudeRadiantRaiments')
        self.assertEqual(factions[rows['WoWArmoryFletcher']['vendors'][0]['form']].edid, 'ServicesSolitudeFletcher')

    def test_service_catalog_has_every_row_and_spell(self):
        defaults = self.service['item_defaults']
        self.assertEqual(set(self.service['items']), set(self.armory['items']))
        for entry in ('16921', '21126', '85', '22589'):
            self.assertEqual({**defaults, **self.service['items'][entry]}, self.rows[int(entry)])
        spells = self.service['spells']
        for row in self.service['items'].values():
            for n in range(1, 6):
                if row.get(f'spellid_{n}'):
                    self.assertIn(str(row[f'spellid_{n}']), spells)
        for row in self.service['item_sets'].values():
            for sid in row['SetSpellID']:
                if sid:
                    self.assertIn(str(sid), spells)
        for s in spells.values():
            for e in (1, 2, 3):
                self.assertIn(f'EffectBonusCoefficient{e}', s)
                if s[f'EffectTriggerSpell{e}']:
                    self.assertIn(str(s[f'EffectTriggerSpell{e}']), spells)
        self.assertEqual(set(self.service['item_sets']), set(self.armory['sets']))

    def test_sizes_and_load(self):
        # What Skyrim and the plugin load: a regular plugin of about 21,500 small records, and armory.json parsed once.
        self.assertLess(self.path.stat().st_size, 8_000_000)
        self.assertLess(len(self.records), 25_000)
        self.assertLess((self.root / 'armory.json').stat().st_size, 6_000_000)
        start = time.perf_counter()
        json.loads((self.root / 'armory.json').read_text())
        self.assertLess(time.perf_counter() - start, 1.0)

    def test_stable_ids_and_bytes(self):
        current = B.Forms(self.forms, last=0xFFFFFF).ids
        for key, value in self.original.items():
            self.assertEqual(current[key], value)
        outputs = (self.path, self.forms, self.root / 'armory.json', self.root / 'armory_catalog.json')
        before = [p.read_bytes() for p in outputs]
        self.build()
        self.assertEqual(before, [p.read_bytes() for p in outputs])
        forms = B.Forms(self.forms, last=0xFFFFFF)
        old = dict(forms.ids)
        forms.get('item:999999')
        self.assertTrue(all(forms.ids[k] == v for k, v in old.items()))
        self.assertEqual(forms.ids['item:999999'], max(old.values()) + 1)


if __name__ == '__main__':
    unittest.main()
