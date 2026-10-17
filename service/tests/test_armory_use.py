"""Item uses retain complete effects, native conversion and item/category cooldowns."""
import unittest
from unittest.mock import patch

from service.catalog import Catalog, DEFAULT
from service.server import Service
from service import equipment, spells


@unittest.skipUnless((DEFAULT.parent / 'armory_catalog.json').exists(), 'build/armory_catalog.json required')
class ArmoryUse(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()
        cls.service = Service(cls.cat, None)

    def caster(self, *items, **kwargs):
        return {'items': list(items), 'level': 60,
                'skyrim': {'destruction': 50, 'restoration': 50, 'alteration': 50}, **kwargs}

    def use(self, entry, caster=None, target=None):
        return self.service.handle({'op': 'item_use', 'item': entry, 'seed': 1,
                                    'caster': caster if caster is not None else self.caster(entry),
                                    'target': target or {'kind': 'self'}})

    def test_diamond_flask_preserves_stat_and_heal_and_native_tooltip(self):
        reply = self.use(20130, self.caster(20130, 23558))
        use, = reply['uses']
        stat, heal = use['auras']
        self.assertEqual((stat['type'], stat['stat'], stat['amount'], stat['duration_ms']), ('stat', 0, 75, 60000))
        self.assertEqual({a['mgef']: a['magnitude'] for a in stat['skyrim']},
                         {0x7A0F4: 75, 0x7A0FF: 37.5, 0x7A106: 37.5})
        self.assertEqual((heal['type'], heal['per_tick'], len(heal['ticks'])), ('heal', 2, 12))
        self.assertEqual({t['amount'] for t in heal['ticks']}, {2})
        self.assertEqual((use['cost'], use['gcd_ms'], use['cooldown_ms']), (0, 1500, 0))
        self.assertIn('Skyrim: 37.5 One-Handed damage %', use['tooltip']['native_lines'])
        active = self.service.char(self.caster(20130, 23558, auras=[24427]))
        plain = self.service.char(self.caster(20130, 23558))
        self.assertEqual(active.stats['str'] - plain.stats['str'], 75)
        self.assertEqual(active.attack_power() - plain.attack_power(), 150)

    def test_absorb_capacity_school_duration_and_skill_conversion(self):
        for skill, amount in ((0, 112), (50, 225), (100, 338)):
            with self.subTest(skill=skill):
                use, = self.use(23558, self.caster(23558, skyrim={'alteration': skill}))['uses']
                shield, = use['auras']
                self.assertEqual((shield['type'], shield['amount'], shield['school_mask'], shield['duration_ms']),
                                 ('absorb_shield', amount, 127, 20000))
                self.assertEqual(use['tooltip']['effects'][0], [900, 900])  # client converts once
        physical, = self.use(20071)['uses'][0]['auras']
        self.assertEqual(physical['school_mask'], 1)

    def test_shared_category_blocks_another_spell_but_not_an_independent_shield(self):
        reply = self.use(20130)
        cooldown, = reply['item_cooldowns']
        self.assertEqual(cooldown, {'spell': 24427, 'item': 20130, 'cooldown_ms': 360000, 'category': 1141,
                                    'ready_category': 1141, 'category_cooldown_ms': 60000, 'gcd_ms': 1500})
        active = {str(equipment.CATEGORY_KEY | 1141): 60000}
        caster = self.caster(20130, 18820, 23558, proc_cooldowns=active)
        with self.assertRaisesRegex(spells.CastError, 'ERR_ITEM_COOLDOWN'):
            self.use(18820, caster)
        self.assertTrue(self.use(23558, caster)['uses'])
        self.assertTrue(self.use(18820, {**caster, 'proc_cooldowns': {str(equipment.CATEGORY_KEY | 1141): 0}})['uses'])
        with self.assertRaisesRegex(spells.CastError, 'ERR_ITEM_COOLDOWN'):
            self.use(20130, self.caster(20130, proc_cooldowns={str(equipment.ITEM_KEY | 20130): 1}))
        # A timer kept under the bare spell ID (a cast without the item) does not block the item.
        self.assertTrue(self.use(20130, self.caster(20130, proc_cooldowns={'24427': 1}))['uses'])
        with self.assertRaisesRegex(spells.CastError, 'ERR_ITEM_COOLDOWN'):
            self.use(20130, self.caster(20130, gcd_ms=1000))
        self.assertTrue(self.use(23558, self.caster(23558, gcd_ms=1000))['uses'])

    def test_cooldown_zero_overrides_and_minus_one_inherits(self):
        item = self.cat.item(20130)
        spell = self.cat.spell(24427)
        for own, shared in ((0, 0), (-1, -1), (1234, 5678)):
            with patch.dict(item, spellcooldown_1=own, spellcategory_1=0, spellcategorycooldown_1=shared):
                cd = equipment.use_cooldown(item, 1, spell)
                self.assertEqual(cd['cooldown_ms'], spell['RecoveryTime'] if own < 0 else own)
                self.assertEqual(cd['category_cooldown_ms'], spell['CategoryRecoveryTime'] if shared < 0 else shared)
                self.assertEqual((cd['category'], cd['ready_category']), (spell['Category'], 0))

    def test_item_category_zero_skips_the_category_check(self):
        # Player::AddCooldown starts the spell's category 59, but WorldObject::IsSpellReady takes the item's
        # category (0) for an item use and so checks none.
        item = self.cat.item(20130)
        busy = {str(equipment.CATEGORY_KEY | 59): 60000, str(equipment.CATEGORY_KEY | 1141): 60000}
        with patch.dict(item, spellcategory_1=0):
            cd, = self.use(20130)['item_cooldowns']
            self.assertEqual((cd['category'], cd['ready_category']), (59, 0))
            self.assertTrue(self.use(20130, self.caster(20130, proc_cooldowns=busy))['uses'])
        with self.assertRaisesRegex(spells.CastError, 'ERR_ITEM_COOLDOWN'):
            self.use(20130, self.caster(20130, proc_cooldowns=busy))

    def test_shared_spell_timer_blocks_only_its_own_item(self):
        # Barbaric Belt and the two Hauberks all use Rage 9174 with an hour's timer and category 1141's 15 s.
        caster = lambda **kw: self.caster(4264, 6972, 7133, **kw)
        for entry in (4264, 6972, 7133):
            cd, = self.use(entry, caster())['item_cooldowns']
            self.assertEqual((cd['spell'], cd['item'], cd['cooldown_ms'], cd['ready_category'], cd['category_cooldown_ms']),
                             (9174, entry, 3600000, 1141, 15000))
        belt = {str(equipment.ITEM_KEY | 4264): 3600000}
        with self.assertRaisesRegex(spells.CastError, 'ERR_ITEM_COOLDOWN'):
            self.use(4264, caster(proc_cooldowns=belt))
        self.assertTrue(self.use(6972, caster(proc_cooldowns=belt))['uses'])
        self.assertTrue(self.use(7133, caster(proc_cooldowns=belt))['uses'])
        with self.assertRaisesRegex(spells.CastError, 'ERR_ITEM_COOLDOWN'):
            self.use(6972, caster(proc_cooldowns={**belt, str(equipment.CATEGORY_KEY | 1141): 15000}))

    def test_cooldown_on_event_is_marked_for_the_aura_end(self):
        # No item use spell in 1.12's Spell.dbc (nor CMaNGOS spell_template) carries SPELL_ATTR_COOLDOWN_ON_EVENT;
        # Admiral's Hat 12022 is 0x09040000. A spell with it starts its timers when its aura fades.
        hat, spell = self.cat.item(10030), self.cat.spell(12022)
        self.assertFalse(spell['Attributes'] & equipment.ATTR_COOLDOWN_ON_EVENT)
        self.assertNotIn('on_event', equipment.use_cooldown(hat, 1, spell))
        self.assertFalse(any(self.cat.spell(sid)['Attributes'] & equipment.ATTR_COOLDOWN_ON_EVENT
                             for sid in self.cat.item_use_spells))
        with patch.dict(spell, Attributes=spell['Attributes'] | equipment.ATTR_COOLDOWN_ON_EVENT):
            cd = equipment.use_cooldown(hat, 1, spell)
        self.assertEqual((cd['on_event'], cd['cooldown_ms'], cd['item']), (True, 60000, 10030))

    def test_engulfing_shadows_restores_mana_once(self):
        # Blade of Eternal Darkness's proc 27860: 100 Shadow damage and one 100 mana energize (20 magicka).
        target = {'kind': 'unit', 'level': 1, 'reaction': 'hostile', 'alive': True, 'attackable': True,
                  'distance_yd': 5, 'los': True}
        procs = [p for seed in range(60) for p in self.service.handle(
            {'op': 'resolve', 'spell': self.cat.slice['starfire'], 'seed': seed, 'target': target,
             'caster': {'items': [17780], 'level': 60, 'talents': {}}}).get('procs', [])]
        self.assertTrue(procs)
        for proc in procs:
            self.assertEqual(proc['spell'], 27860)
            self.assertEqual([e for e in proc['events'] if e['type'] == 'power'],
                             [{'type': 'power', 'power': 'mana', 'amount': 100}])

    def test_trinkets_need_equipping_and_unsupported_items_fail_with_client_error(self):
        for entry in (23558, 4264):
            with self.subTest(entry=entry), self.assertRaisesRegex(spells.CastError, 'SPELL_FAILED_ITEM_NOT_READY'):
                self.use(entry, self.caster())
        for entry in (1973, 13353, 17909, 22589, 19336, 5522, 9449):
            with self.subTest(entry=entry), self.assertRaisesRegex(spells.CastError, 'SPELL_FAILED_SPELL_UNAVAILABLE'):
                self.use(entry)
        with self.assertRaisesRegex(spells.CastError, 'SPELL_FAILED_SPELL_UNAVAILABLE'):
            self.use(12640)  # no Use: spell

    def test_ranged_attack_power_does_not_multiply_melee_attack_power(self):
        plain = self.service.char(self.caster(21180))
        buffed = self.service.char(self.caster(21180, auras=[25891]))
        self.assertEqual(buffed.attack_power() - plain.attack_power(), 280)
        use, = self.use(21180)['uses']
        self.assertEqual([(a['type'], a['amount']) for a in use['auras']],
                         [('attack_power', 280), ('ranged_attack_power', 280)])
        self.assertIn('Skyrim: 28% Archery damage', use['tooltip']['native_lines'])

    def test_spell_power_and_haste_reach_later_service_casts_without_talents(self):
        caster = self.caster(18820, 19339)
        plain = self.service.char(caster)
        buffed = self.service.char({**caster, 'auras': [23271, 23723]})
        self.assertEqual(buffed.spell_damage(4) - plain.spell_damage(4), 175)
        self.assertEqual(buffed.healing_done(8) - plain.healing_done(8), 175)
        req = {'spell': self.cat.slice['starfire'], 'target': {'kind': 'unit', 'level': 1,
               'reaction': 'hostile', 'alive': True, 'distance_yd': 1}, 'seed': 1}
        normal = spells.resolve(self.cat, plain, req)
        fast = spells.resolve(self.cat, buffed, req)
        self.assertEqual(fast['cast_ms'], int(normal['cast_ms'] / 1.33))
        profiles = self.service.handle({'op': 'skyrim_costs', 'caster': {**caster, 'auras': [23723]}})['spell_tooltips']
        self.assertEqual(profiles[str(req['spell'])]['cast_ms'], fast['cast_ms'])

    def test_self_damage_and_power_restore_keep_both_effects(self):
        use, = self.use(11808)['uses']
        self.assertTrue(use['self_channel'])
        self.assertEqual(use['channel_interrupt_flags'], self.cat.spell(17447)['ChannelInterruptFlags'])
        restore, damage = use['auras']
        self.assertEqual((restore['type'], restore['power'], restore['per_tick']), ('power_restore', 'mana', 15))
        self.assertEqual((damage['type'], damage['per_tick']), ('damage', 19))
        self.assertEqual((len(restore['ticks']), len(damage['ticks'])), (10, 10))
        self.assertEqual(self.use(4264)['uses'][0]['events'], [{'type': 'power', 'power': 'rage', 'amount': 30}])
        self.assertEqual(self.use(23027)['uses'][0]['events'], [{'type': 'power', 'power': 'mana', 'amount': 100}])
        drink, = self.use(15873)['uses']
        self.assertIn('Skyrim: -5 carry weight', drink['tooltip']['native_lines'])

    def test_use_retaliation_is_not_duplicated_by_incoming_equipment_procs(self):
        ch = self.service.char(self.caster(21488, auras=[26168]))
        self.assertNotIn('procs', equipment.incoming(self.cat, ch, {'weapon_hit': True}, {}))

    def test_every_inventoried_item_resolves_or_fails_explicitly(self):
        counts = {True: 0, False: 0}
        for entry in sorted(self.cat.armory):
            item = self.cat.item(entry)
            for _, sid in equipment.use_spells(item):
                with self.subTest(entry=entry, spell=sid):
                    unsupported = bool(equipment.item_use_support(self.cat, item))
                    counts[unsupported] += 1
                    if unsupported:
                        with self.assertRaisesRegex(spells.CastError, 'SPELL_FAILED_SPELL_UNAVAILABLE'):
                            self.use(entry)
                    else:
                        positive = spells.is_positive(self.cat.spell(sid))
                        target = {'kind': 'unit', 'level': 1, 'alive': True, 'distance_yd': 1,
                                  'reaction': 'friendly' if positive else 'hostile',
                                  'healable': positive, 'attackable': not positive}
                        use, = self.use(entry, target=target)['uses']
                        self.assertTrue(use['auras'] or use['events'])
                        self.assertEqual(use['spell'], sid)
        self.assertEqual(counts, {True: 112, False: 72})

    def test_documented_inventory_matches_every_trigger_zero_row(self):
        from data.build_armory import REPO, use_inventory
        self.assertTrue((REPO / 'docs/armory.md').read_text().endswith(use_inventory(self.cat)))


if __name__ == '__main__':
    unittest.main()
