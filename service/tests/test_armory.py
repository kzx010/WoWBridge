"""WoWBridgeArmory.esp's items reach the service as WoWBridge.esp's do: the same caster.items, their item_template
rows, sets and spells from build/armory_catalog.json (data/build_armory.py)."""
import unittest
from unittest.mock import patch

from service.catalog import Catalog, DEFAULT
from service.server import Service
from service import equipment

ARMORY = DEFAULT.parent / 'armory_catalog.json'


@unittest.skipUnless(ARMORY.exists(), 'build/armory_catalog.json: run data/build_armory.py')
class Armory(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()
        cls.service = Service(cls.cat, None)

    def test_rows_are_whole_and_wowbridge_esp_keeps_its_own(self):
        self.assertGreater(len(self.cat.armory), 8000)
        halo = self.cat.item(16921)   # Halo of Transcendence
        self.assertEqual(set(halo), set(self.cat.item(16897)))   # every column, as Stormrage Cover has them
        self.assertEqual((halo['name'], halo['RequiredLevel'], halo['AllowableRace']), ('Halo of Transcendence', 0, -1))
        self.assertNotIn(16897, self.cat.armory)
        self.assertIn('211', self.cat.raw['item_sets'])
        armory = Catalog(armory=ARMORY, raw={**self.cat.raw, 'items': {}})   # an explicit supplement path
        self.assertEqual(armory.item(16921), halo)

    def test_stats_equip_spells_and_set_bonuses(self):
        naked = self.service.char({'items': [], 'level': 60})
        halo = self.service.char({'items': [16921], 'level': 60})
        for stat, value in (('int', 27), ('spi', 22), ('sta', 17)):
            self.assertEqual(halo.stats[stat] - naked.stats[stat], value)
        self.assertIn(18034, {a['spell'] for a in halo.auras})   # its Equip: line
        reply = self.service.handle({'op': 'stats', 'caster': {'items': [16921, 16897], 'level': 60}})
        self.assertEqual(set(reply['items']), {16921, 16897})
        entries = [e for e in self.cat.raw['item_sets']['211']['ItemID'] if e]
        auras = lambda items: {a['spell'] for a in self.service.char({'items': items, 'level': 60}).auras}
        self.assertNotIn(21894, auras(entries[:2]))
        self.assertIn(21894, auras(entries[:3]))   # (3) Set: Meditation

    def test_a_chance_on_hit_weapon_procs(self):
        entry, sid = next((e, self.cat.item(e)['spellid_1']) for e in sorted(self.cat.armory)
                          if self.cat.item(e)['class'] == 2 and self.cat.item(e)['spelltrigger_1'] == 2 and
                          self.cat.spell(self.cat.item(e)['spellid_1'])['Effect1'] == 2)   # direct damage
        caster = {'items': [entry], 'level': 60, 'form': 'none', 'skyrim': {'destruction': 50, 'one_handed': 100}}
        procs = []
        for seed in range(300):
            reply = self.service.handle({'op': 'melee', 'caster': caster, 'seed': seed,
                                         'target': {'level': 1, 'kind': 'unit', 'reaction': 'hostile', 'alive': True,
                                                    'attackable': True, 'facing': True, 'distance_yd': 1}})
            procs += reply.get('procs', [])
        self.assertTrue(procs, entry)
        self.assertEqual({p['spell'] for p in procs}, {sid})

    def test_incoming_equip_procs_follow_their_chance_and_trigger(self):
        for entry, source, trigger, chance in ((942, 18799, 18798, 1), (7375, 9160, 9159, 5)):
            with self.subTest(entry=entry):
                req = {'op': 'incoming_hit', 'weapon_hit': True, 'caster': {'items': [entry], 'level': 60},
                       'target': {'kind': 'unit', 'level': 1, 'reaction': 'hostile', 'alive': True,
                                  'attackable': True, 'distance_yd': 1}}
                procs = [p for seed in range(1000) for p in self.service.handle({**req, 'seed': seed}).get('procs', [])]
                self.assertTrue(procs)
                self.assertEqual({p['spell'] for p in procs}, {trigger})
                self.assertEqual({p['source_spell'] for p in procs}, {source})
                self.assertTrue(any(p['auras'] for p in procs))   # freeze or sleep actually resolves
                self.assertNotIn('procs', self.service.handle({**req, 'weapon_hit': False}))
                ch = self.service.char(req['caster'])
                with patch('service.equipment.F.roll_chance', return_value=False) as roll:
                    self.assertNotIn('procs', equipment.incoming(self.cat, ch, req, {}))
                    roll.assert_called_once()
                    self.assertEqual(roll.call_args.args[1], chance)
                for changes in ({'ProcFlags': 0x4}, {'ProcChance': 0}, {'EffectTriggerSpell1': 0}):
                    with patch.dict(self.cat.spell(source), changes):
                        self.assertNotIn('procs', equipment.incoming(self.cat, ch, req, {}))
                with patch.dict(self.cat.spell(source), {'ProcFlags': 0x4, 'ProcChance': 0}), \
                        patch.dict(self.cat.raw, {'spell_proc_event': [
                            {'entry': source, 'procFlags': 0x28, 'CustomChance': 100}]}):
                    proc = equipment.incoming(self.cat, ch, req, {})['procs'][0]
                    self.assertEqual((proc['source_spell'], proc['spell']), (source, trigger))

    def test_equipment_incoming_does_not_repeat_natures_grasp(self):
        req = {'weapon_hit': True, 'target': {'kind': 'unit'}}
        ch = self.service.char({'items': [], 'level': 60, 'auras': [16689]})
        self.assertTrue(any(a['aura'] == 42 for a in ch.auras))
        with patch('service.equipment.F.roll_chance', return_value=True):
            self.assertNotIn('procs', equipment.incoming(self.cat, ch, req, {}))

    def test_incoming_set_proc_requires_its_earned_threshold(self):
        group = self.cat.raw['item_sets']['181']   # Magister's Regalia: (6) Set: Freeze
        entries = [e for e in group['ItemID'] if e]
        req = {'weapon_hit': True, 'target': {'kind': 'unit'}}
        for items, expected in ((entries[:6], 1), (entries[:5], 0)):
            ch = self.service.char({'items': items, 'level': 60})
            with patch('service.equipment.F.roll_chance', return_value=True), \
                    patch('service.equipment._trigger', return_value={'spell': 27868}) as trigger:
                equipment.incoming(self.cat, ch, req, {})
                self.assertEqual(trigger.call_count, expected)
                if expected:
                    self.assertEqual(trigger.call_args.args[-2:], (27868, 27867))

    def test_shared_set_spell_stacks_and_removes_per_set(self):
        cadaverous = [14637, 14638]   # chest and boots
        necropile = [14629, 14633]   # wrists and hands: both sets can be worn together
        def count(items):
            ch = self.service.char({'items': items, 'level': 60})
            return sum(a['spell'] == 7516 for a in ch.auras)
        self.assertEqual(count(cadaverous), 1)
        self.assertEqual(count(necropile), 1)
        self.assertEqual(count(cadaverous + necropile), 2)
        self.assertEqual(count(cadaverous + necropile[:1]), 1)
        self.assertEqual(count(cadaverous[:1] + necropile[:1]), 0)
        self.assertEqual(count(cadaverous[:1] * 2), 0)   # two copies are one distinct set member

    def test_new_carried_weapon_tooltip_uses_converted_damage_with_unchanged_caster(self):
        caster = {'items': [], 'level': 60, 'skyrim': {'one_handed': 50}}
        req = {'op': 'skyrim_costs', 'caster': caster}
        self.assertNotIn('10571', self.service.handle(req)['weapon_tooltips'])
        reply = self.service.handle({**req, 'weapons': {'10571': 12}})
        tip = reply['weapon_tooltips']['10571']
        self.assertAlmostEqual((tip['min'] + tip['max']) / 2, 15)   # base 12 at skill 50: 1.25x
        self.assertNotIn('10571', self.service.handle(req)['weapon_tooltips'])


if __name__ == '__main__':
    unittest.main()
