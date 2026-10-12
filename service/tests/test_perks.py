"""Every mapped vanilla perk, live service effects, class isolation and carrier contract."""
from pathlib import Path
import random
import unittest
from unittest.mock import patch

from data.build_perks import DEFAULT_ESM, extract, registry
from service import formulas as F, melee, spells, skyrim
from service.catalog import Catalog
from service.perks import Perks, WHITE
from service.server import Service
from service.tests.test_skyrim_balance import request_for

ROOT = Path(__file__).resolve().parents[2]
COST_SPELLS = {
    'Destruction': (7, [403, 529, 943, 10392, 29228]),
    'Restoration': (11, [8946, 25299, 25297, 9863, 20748]),
    'Alteration': (5, [17, 10797, 19289, 19292, 19293]),
    'Illusion': (9, [172, 6222, 7648, 11671, 25311]),
    'Conjuration': (3, [13163, 136, 3662, 13542, 13544]),
}
TIERS = ('Novice00', 'Apprentice25', 'Adept50', 'Expert75', 'Master100')


class SkyrimPerks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()
        cls.perks = Perks(cls.cat)

    def setUp(self):
        self.svc = Service(self.cat, None)

    def ch(self, key=None, cls=11, **state):
        return self.svc.char({'class': cls, 'perks': {key: True} if key else {}, **state})

    def cast(self, sid, keys=(), **state):
        req = request_for(self.cat, sid, seed=1)
        req['caster'].update({'class': 11, 'perks': dict.fromkeys(keys, True), **state})
        return self.svc.handle(req)

    def test_each_mapped_perk_has_an_independent_expected_modifier(self):
        """86 named perks: no generated expectation from the implementation table."""
        covered = set()
        for school, (cls, ids) in COST_SPELLS.items():
            for suffix, sid in zip(TIERS, ids):
                key = school + suffix
                covered.add(key)
                plain, perk = self.ch(cls=cls), self.ch(key, cls)
                s = self.cat.spell(sid)
                self.assertGreater(spells._cost(s, plain), 0, (key, sid))
                with self.subTest(perk=key, spell=sid):
                    self.assertEqual(spells._cost(s, perk), spells._cost(s, plain) // 2)
                    cost = self.svc.handle({'op': 'skyrim_costs', 'caster': {'class': cls, 'perks': {key: True}, 'skyrim': {}}})['costs'][str(sid)]
                    expected = skyrim.convert({'spell': sid, 'cost': spells._cost(s, plain) // 2}, {'skyrim': {}},
                                               skyrim.spell_skill(s, self.cat.balance), self.cat.balance)['cost']
                    self.assertEqual(cost, expected)

        groups = [
            ({'AugmentedFlames':25, 'AugmentedFlames60':50}, 8, 133, 0),
            ({'AugmentedFrost':25, 'AugmentedFrost60':50}, 8, 116, 0),
            ({'AugmentedShock':25, 'AugmentedShock60':50}, 11, 9912, 0),
            ({'Regeneration':50}, 11, 25297, 0),
            ({'Stability':50}, 11, 22812, 1),
            ({'MageArmor30':100, 'MageArmor50':150, 'MageArmor70':200}, 11, 9885, 8),
            ({'Armsman'+s:20*(i+1) for i,s in enumerate(('00','20','40','60','80'))}, 11, 9850, 0),
            ({'Barbarian'+s:20*(i+1) for i,s in enumerate(('00','20','40','60','80'))}, 1, 78, 0),
            ({'Overdraw'+s:20*(i+1) for i,s in enumerate(('00','20','40','60','80'))}, 3, 3044, 0),
            ({'FightingStance':-25}, 11, 9850, 14),
            ({'ChampionsStance':-25}, 1, 78, 14),
            ({'SavageStrike':25}, 11, 9830, 0),
            ({'DevastatingBlow':25}, 1, 12294, 0),
            ({'HackAndSlash'+str(s):25*(i+1) for i,s in enumerate((30,60,90))}, 11, 9896, 22),
            ({'Limbsplitter'+str(s):25*(i+1) for i,s in enumerate((30,60,90))}, 1, 772, 22),
            ({'Backstab':100, 'AssassinsBlade':400}, 11, 9867, 0),
            ({'DeadlyAim':50}, 3, 3044, 0),
            ({'ShieldWall'+s:10*(i+1) for i,s in enumerate(('00','20','40','60','80'))}, 2, 465, 8),
            ({'DeadlyBash':400}, 1, 72, 0),
            ({'Alchemist'+s:20*(i+1) for i,s in enumerate(('00','20','40','60','80'))}, 4, 8680, 0),
            ({'Poisoner':25}, 4, 2818, 22),
        ]
        for keys, cls, sid, op in groups:
            for key, pct in keys.items():
                covered.add(key)
                with self.subTest(perk=key, spell=sid):
                    ch = self.ch(key, cls, wearing_armor=False, stealthed=True)
                    self.assertEqual(F.apply_spell_mod(self.cat.spell(sid), op, 100, ch.mods, eff=1), 100 + pct)
        for prefix, cls, sid in (('Bladesman',11,9850), ('DeepWounds',1,78), ('CriticalShot',3,2643)):
            for rank, (suffix, chance, bonus) in enumerate(((30,10,0), (60,15,25), (90,20,50))):
                key = prefix + str(suffix)
                covered.add(key)
                with self.subTest(perk=key):
                    ch = self.ch(key, cls)
                    s = self.cat.spell(sid)
                    self.assertEqual(F.apply_spell_mod(s, 7, 5, ch.mods), 5 + chance)
                    self.assertEqual(F.apply_spell_mod(s, 15, 1, ch.mods), 1 + bonus / 100)
        covered.add('Respite')
        self.assertEqual({k for r in self.perks.data['rules'] for k in r['perks']}, covered)
        self.assertEqual(len(covered), 86)

    def test_live_damage_healing_and_rank_replacement(self):
        plain = self.cast(9912)
        first = self.cast(9912, ['AugmentedShock'])
        highest = self.cast(9912, ['AugmentedShock', 'AugmentedShock60'])
        single = self.cast(9912, ['AugmentedShock60'])
        self.assertEqual(single, highest)
        self.assertGreater(first['events'][0]['amount'], plain['events'][0]['amount'])
        self.assertGreater(highest['events'][0]['amount'], first['events'][0]['amount'])
        self.assertEqual(self.cast(9835, ['AugmentedShock']), self.cast(9835))  # Moonfire is Arcane
        self.assertGreater(self.cast(25297, ['Regeneration'])['events'][0]['amount'], self.cast(25297)['events'][0]['amount'])
        self.assertGreater(self.cast(25299, ['Regeneration'])['auras'][0]['per_tick'], self.cast(25299)['auras'][0]['per_tick'])
        def area(keys, sid):
            return self.svc.handle({'op':'tick_area', 'spell':sid, 'seed':1, 'targets':[{'id':1,'level':60,'friendly':sid==9863}],
                                    'caster':{'perks':dict.fromkeys(keys,True), 'skyrim':{}}})['results'][0]['amount']
        self.assertGreater(area(['Regeneration'],9863), area([],9863))
        self.assertGreater(area(['AugmentedShock'],17402), area([],17402))

    def test_respite_all_healing_paths_and_frenzied_regeneration(self):
        direct = self.cast(25297, ['Respite'], skyrim={})
        e = direct['events'][0]
        self.assertEqual(e['stamina'], e['amount'])
        self.assertTrue(self.cast(25299, ['Respite'], skyrim={})['auras'][0]['respite'])
        area = self.svc.handle({'op':'tick_area', 'spell':9863, 'targets':[{'id':7,'friendly':True}],
                               'caster':{'perks':{'Respite':True}, 'skyrim':{}}})['results'][0]
        self.assertEqual(area['stamina'], area['amount'])
        self.assertNotIn('stamina', self.cast(9912, ['Respite'], skyrim={})['events'][0])
        plain = self.cast(22896, skyrim={})['auras'][0]
        stronger = self.cast(22896, ['Regeneration','Respite'], skyrim={})['auras'][0]
        self.assertAlmostEqual(stronger['health_per_rage'], plain['health_per_rage'] * 1.5)
        self.assertTrue(stronger['respite'])

    def test_mage_armor_changes_only_armor_and_requires_real_equipment_state(self):
        plain = self.cast(9885, skyrim={})
        self.assertEqual(plain, self.cast(9885, ['MageArmor70'], skyrim={}))
        self.assertEqual(plain, self.cast(9885, ['MageArmor70'], skyrim={}, wearing_armor=True))
        boosted = self.cast(9885, ['MageArmor30','MageArmor50','MageArmor70'], skyrim={}, wearing_armor=False)
        for a,b in zip(plain['auras'], boosted['auras']):
            if a['type']=='armor':
                self.assertEqual(b['amount'], a['amount']*3)
                self.assertEqual(b['skyrim'][0]['magnitude'], a['skyrim'][0]['magnitude']*3)
            else:
                self.assertEqual(a,b)
        # Neither Thorns nor Barkskin has an armor effect in CMaNGOS.
        for sid in (9910,22812,9907):
            self.assertEqual(self.cast(sid,skyrim={}), self.cast(sid,['MageArmor70'],skyrim={},wearing_armor=False))
        self.assertEqual(self.cast(22812,['Stability'])['auras'][0]['duration_ms'],22500)
        self.assertEqual(self.cast(768,['Stability'])['auras'][0]['duration_ms'],-1)

    def test_stealth_openers_and_pounce_trigger_damage(self):
        for sid in (9867,9830,9827):
            def result(keys, prowling=True):
                return self.cast(sid,keys,prowling=prowling)
            plain = result([])
            stab = result(['Backstab'])
            blade = result(['Backstab','AssassinsBlade'])
            self.assertEqual(blade,result(['AssassinsBlade']))
            def amount(out):
                return sum(e.get('amount',0) for e in out['events'] if e['type']=='damage') + sum(a.get('per_tick',0) for a in out['auras'] if a['type']=='damage')
            self.assertGreater(amount(stab),amount(plain))
            self.assertGreater(amount(blade),amount(stab))
        self.assertEqual(self.cast(9830),self.cast(9830,['AssassinsBlade'],prowling=False))
        self.assertEqual(self.cast(9850,prowling=True),self.cast(9850,['AssassinsBlade'],prowling=True))

    def test_weapon_damage_bleeds_costs_and_crit_not_applied_twice(self):
        self.assertEqual(self.cast(9850,['FightingStance'])['cost'],33)
        self.assertEqual(self.cast(9881,['FightingStance'])['cost'],11.2)  # integer core rage units /10
        self.assertGreater(self.cast(9896,['HackAndSlash30'])['auras'][0]['per_tick'],self.cast(9896)['auras'][0]['per_tick'])
        self.assertGreater(self.cast(9850,['Armsman00'])['events'][0]['amount'],self.cast(9850)['events'][0]['amount'])
        self.assertEqual(self.cast(9912),self.cast(9912,['Armsman80','Barbarian80']))
        self.assertEqual(self.cast(9850),self.cast(9850,['Barbarian80']))
        req={'op':'melee','seed':1,'target':{'level':60,'distance_yd':2},'caster':{'form':'cat','perks':{}}}
        plain=self.svc.handle(req)
        req['caster']['perks']={'Armsman00':True}
        strong=self.svc.handle(req)
        self.assertGreater(strong['damage'],plain['damage'])
        ch=self.ch('Bladesman30',form='cat')
        self.assertEqual(F.apply_spell_mod(WHITE,7,0,ch.mods),10)
        with patch('service.formulas.roll_chance', return_value=False) as roll:
            melee.ability_damage(self.cat,ch,self.cat.spell(9850),{'level':60},100,random.Random(1))
            self.assertEqual(roll.call_args_list[0].args[1], ch.melee_crit()+10)
        # A yellow crit uses its spell selector once, not the white-hit selector as well.
        seen=[]
        for seed in range(60):
            req=request_for(self.cat,9850,seed=seed)
            req['caster']['perks']={'Bladesman30':True}
            out=self.svc.handle(req)
            seen.append(out['events'][0].get('crit',False))
        self.assertTrue(any(seen))

    def test_swiftmend_inherits_regeneration_once_from_consumed_hot(self):
        from service.tests.test_talents import RESTORATION
        def heal(keys):
            hot = self.cast(25299, keys, talents=RESTORATION)['auras'][0]
            req = {'op':'resolve', 'spell':18562, 'seed':1,
                   'caster':{'class':11,'talents':RESTORATION,'perks':dict.fromkeys(keys,True)},
                   'target':{'kind':'self','hots':[{'spell':25299,'per_tick':hot['per_tick'],'remaining_ms':1000}]}}
            return self.svc.handle(req)['events'][0]['amount']
        plain, strong = heal([]), heal(['Regeneration'])
        self.assertLess(abs(strong - plain * 1.5), 10)

    def test_poison_strength_does_not_change_recipes_or_physical_finishers(self):
        def poison(sid,keys):
            return self.svc.handle({'op':'resolve','spell':sid,'seed':1,'target':{'kind':'unit','level':60,'distance_yd':2},
                                    'caster':{'class':4,'perks':dict.fromkeys(keys,True)}})
        self.assertGreater(poison(8680,['Alchemist80','Poisoner'])['events'][0]['amount'],poison(8680,[])['events'][0]['amount'])
        self.assertGreater(poison(2818,['Poisoner'])['auras'][0]['per_tick'],poison(2818,[])['auras'][0]['per_tick'])
        ch=self.ch('Alchemist80',4)
        self.assertEqual(F.apply_spell_mod(self.cat.spell(2835),8,1,ch.mods),1)
        self.assertEqual(F.apply_spell_mod(self.cat.spell(2098),0,100,ch.mods),100)
        self.assertEqual(F.apply_spell_mod(self.cat.spell(8680),0,100,self.ch('Alchemist80',11).mods),100)

    def test_additive_talents_class_isolation_and_cache(self):
        ch=self.ch('AugmentedShock')
        base=self.ch()
        self.assertIsNot(ch,base)
        self.assertFalse(any('perk' in m for m in base.mods))
        # Moonfury's +10% and Augmented Shock's +25% share CMaNGOS's pct pool.
        s=self.cat.spell(9912)
        talent={'op':0,'type':108,'value':10,'family':7,'mask':int(s['SpellFamilyFlags']),'class_driven':True}
        self.assertEqual(F.apply_spell_mod(s,0,100,ch.mods+[talent]),135)
        cost_talent={**talent,'op':14,'value':-10}
        ch=self.ch('DestructionApprentice25')
        self.assertEqual(F.apply_spell_mod(s,14,100,ch.mods+[cost_talent]),40)
        # Even shared generic family flags cannot spill perks into another class's spells.
        ch=self.ch('DestructionNovice00',8)
        self.assertEqual(F.apply_spell_mod(self.cat.spell(403),14,100,ch.mods),100)
        self.assertEqual(F.apply_spell_mod(self.cat.spell(133),14,100,ch.mods),50)

    def test_invalid_payload_and_native_only_perks(self):
        for value in ([],{'MadeUp':True},{'Regeneration':1},{'Regeneration':None}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.svc.char({'perks':value})
        for state in ({'wearing_armor':1},{'stealthed':'yes'}):
            with self.assertRaises(ValueError): self.ch('MageArmor30',**state)
        # These are native abilities/crafting/dual-casting, not fake spell modifiers.
        for key in ('Recovery30','Recovery50','AvoidDeath','Necromage','DestructionDualCasting',
                    'Enchanter80','AgileDefender80','Juggernaut80','Snakeblood'):
            self.assertEqual(self.cast(9912,[key]),self.cast(9912),key)
        self.assertEqual(self.cast(9912),self.cast(9912,['AugmentedFlames']))

    def test_pinned_registry_matches_actual_vanilla_tree_records(self):
        source=extract(DEFAULT_ESM)
        self.assertEqual(source,self.perks.data['perks'])
        self.assertEqual(len(source),251)
        self.assertEqual(source['RestorationAdept50']['form'],0xC44C8)
        self.assertEqual(source['Armsman40']['form'],0x79342)
        self.assertEqual((ROOT/'skse/src/Perks.inc').read_text(),registry(source))
        self.assertEqual(len({r['tree'] for r in source.values()}),18)
        mapped={k for r in self.perks.data['rules'] for k in r['perks']}
        decided={k for r in self.perks.data['dispositions'] for k in r['perks']}
        self.assertFalse(mapped & decided)
        self.assertEqual(mapped | decided, set(source))
        docs=(ROOT/'docs/perks.md').read_text()
        self.assertTrue(all(k in docs for k in source))
