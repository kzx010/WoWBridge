"""New item entries travel through the same caster.items request as the reference kit."""
import unittest

from service.catalog import Catalog
from service.server import Service


class Equipment(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()

    def test_every_set_and_weapon_reaches_stats(self):
        service = Service(self.cat, None)
        base = service.handle({'op': 'stats', 'caster': {'items': [], 'level': 60}})
        for key, group in self.cat.raw['equipment_sets'].items():
            with self.subTest(set=key):
                entries = group['entries']
                reply = service.handle({'op': 'stats', 'caster': {'items': entries, 'level': 60}})
                self.assertEqual(set(reply['items']), set(entries))
                ch = service.char({'items': entries, 'level': 60})
                for stat, code in (('str', 4), ('agi', 3), ('sta', 7), ('int', 5), ('spi', 6)):
                    bonus = sum(row[f'stat_value{n}'] for row in (self.cat.item(e) for e in entries)
                                for n in range(1, 11) if row[f'stat_type{n}'] == code)
                    self.assertGreaterEqual(reply['stats'][stat], base['stats'][stat] + bonus)
                equipped_auras = {a['spell'] for a in ch.auras}
                source = self.cat.raw['item_sets'][str(group['id'])]
                for sid in source['SetSpellID']:
                    if sid and any(self.cat.spell(sid)[f'Effect{n}'] == 6 for n in (1, 2, 3)):
                        self.assertIn(sid, equipped_auras)
        for entry in (19019, 17182, 19364, 18842, 18713, 18715, 18608, 18609, 18816, 18348):
            with self.subTest(weapon=entry):
                reply = service.handle({'op': 'stats', 'caster': {'items': [entry], 'level': 60}})
                self.assertEqual(reply['items'], [entry])

    def test_cenarion_bonus_threshold_and_removal(self):
        service = Service(self.cat, None)
        entries = self.cat.raw['equipment_sets']['cenarion']['entries']
        def aura_ids(items):
            return {a['spell'] for a in service.char({'items': items, 'level': 60}).auras}
        self.assertNotIn(23557, aura_ids(entries[:2]))
        self.assertIn(23557, aura_ids(entries[:3]))
        self.assertNotIn(18382, aura_ids(entries[:4]))
        self.assertIn(18382, aura_ids(entries[:5]))
        self.assertNotIn(23556, aura_ids(entries[:7]))
        self.assertIn(23556, aura_ids(entries))
        self.assertNotIn(23556, aura_ids(entries[:7]))
        self.assertNotIn(23557, aura_ids([]))

    def test_all_imported_equipment_has_full_stats_and_effects_at_level_two(self):
        service = Service(self.cat, None)
        naked = service.char({'items': [], 'level': 2})
        imported = {entry: item for entry, item in self.cat.items.items() if entry not in self.cat.armory}
        self.assertEqual(len(imported), 95)   # WoWBridge.esp's; test_armory covers the Armory's
        for entry, item in imported.items():
            with self.subTest(entry=entry):
                self.assertEqual(item['RequiredLevel'], 0)
                ch = service.char({'items': [entry], 'level': 2})
                self.assertEqual(ch.items, [entry])
                for stat, code in (('str', 4), ('agi', 3), ('sta', 7), ('int', 5), ('spi', 6)):
                    bonus = sum(item[f'stat_value{n}'] for n in range(1, 11) if item[f'stat_type{n}'] == code)
                    self.assertGreaterEqual(ch.stats[stat], naked.stats[stat] + bonus)
                high = service.char({'items': [entry], 'level': 60})
                self.assertEqual(ch.weapon, high.weapon)
                self.assertEqual(ch.auras, high.auras)
        for group in self.cat.raw['equipment_sets'].values():
            low = service.char({'items': group['entries'], 'level': 2})
            high = service.char({'items': group['entries'], 'level': 60})
            self.assertEqual(low.items, high.items)
            self.assertEqual(low.auras, high.auras)

    def test_stale_equipment_level_gate_is_discarded_without_changing_spell_ranks(self):
        raw = self.cat.raw
        original = raw['items']['873']['RequiredLevel']
        raw['items']['873']['RequiredLevel'] = 35
        try:
            stale = Catalog(raw=raw)
            self.assertEqual(stale.item(873)['RequiredLevel'], 0)
            self.assertEqual(stale.ranks, self.cat.ranks)
            self.assertEqual(stale.spells, self.cat.spells)
        finally:
            raw['items']['873']['RequiredLevel'] = original

    def test_dominance_benediction_and_anathema_equip_bonuses(self):
        service = Service(self.cat, None)
        naked = service.char({'items': [], 'level': 60})
        dominance = service.char({'items': [18842], 'level': 60})
        self.assertEqual(dominance.spell_damage(0x7E) - naked.spell_damage(0x7E), 40)
        self.assertEqual(dominance.stats['int'] - naked.stats['int'], 37)
        self.assertEqual(dominance.total(71, 0x7E), 1)
        benediction = service.char({'items': [18608], 'level': 60})
        self.assertEqual(benediction.healing_done(0x7E) - naked.healing_done(0x7E), 106)
        anathema = service.char({'items': [18609], 'level': 60})
        self.assertEqual(anathema.spell_damage(0x20) - naked.spell_damage(0x20), 69)


class EquipmentHitEffects(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()
        cls.service = Service(cls.cat, None)

    def request(self, entry, seed=0, form='none', native=True, level=60):
        caster = {'items': [entry], 'level': level, 'form': form}
        if native:
            caster['skyrim'] = {'destruction': 50, 'alteration': 50, 'one_handed': 100, 'two_handed': 100}
        return {'op': 'melee', 'caster': caster, 'seed': seed,
                'target': {'level': 1, 'kind': 'unit', 'reaction': 'hostile', 'alive': True,
                           'attackable': True, 'facing': True, 'distance_yd': 1}}

    def test_each_weapon_proc_reaches_native_damage_or_armor(self):
        for entry, sid in ((17182, 21162), (18348, 22850), (18816, 23267), (19019, 21992)):
            found = []
            for seed in range(300):
                req = self.request(entry, seed)
                reply = self.service.handle(req)
                if reply['outcome'] in ('miss', 'dodge', 'parry') or reply['damage'] <= 0:
                    self.assertNotIn('procs', reply)
                if reply.get('procs'):
                    with self.subTest(entry=entry):
                        proc = reply['procs'][0]
                        self.assertEqual(proc['spell'], sid)
                        self.assertEqual(proc['cost'], 0)
                        self.assertEqual(proc['cast_ms'], 0)
                        self.assertEqual(proc['gcd_ms'], 0)
                        self.assertTrue(proc['events'] or proc['auras'])
                        if sid == 22850:
                            self.assertEqual(proc['target'], 'caster')
                            self.assertTrue(all(a.get('skyrim') for a in proc['auras']))
                        else:
                            self.assertEqual(proc['target'], 'target')
                            if any(e['type'] in ('miss', 'resist') for e in proc['events']):
                                self.assertFalse(proc['auras'])
                                continue
                            self.assertTrue(any(e['type'] == 'damage' and e['amount'] > 0 for e in proc['events']))
                            wow = self.service.handle(self.request(entry, seed, native=False))['procs'][0]
                            # Proc uses Destruction 50, not the weapon skill 100.
                            self.assertEqual(proc['events'][0]['amount'], round(wow['events'][0]['amount'] * .25))
                        if sid == 21162:
                            self.assertEqual(len(proc['auras'][0]['ticks']), 5)
                        if sid == 21992:
                            self.assertTrue(any(a['type'] == 'attack_speed' and a['amount'] == 20 for a in proc['auras']))
                    found.append(proc)
            self.assertTrue(found, entry)
            self.assertLess(len(found), 100, '101 is a database sentinel, never a 101% proc probability')
            for seed in range(30):
                self.assertNotIn('procs', self.service.handle(self.request(entry, seed, form='cat')))

    def test_incoming_retaliation_and_drain(self):
        def incoming(entry, seed, weapon=True):
            req = self.request(entry, seed)
            return self.service.handle({**req, 'op': 'incoming_hit', 'weapon_hit': weapon})
        proc = incoming(17182, 0)['procs'][0]
        self.assertEqual(proc['events'][0]['amount'], 1)  # 5 WoW fire damage, converted
        self.assertNotIn('procs', incoming(17182, 0, False))
        spells = set()
        for seed in range(500):
            for proc in incoming(1168, seed).get('procs', []):
                spells.add(proc['spell'])
                self.assertTrue(proc['events'])
                if proc['spell'] == 18817:
                    self.assertEqual(proc['events'][0]['amount'], proc['events'][1]['amount'])
                    self.assertEqual(proc['events'][1]['target'], 'caster')
        self.assertEqual(spells, {18817, 18818})
        self.assertNotIn('procs', incoming(1168, 0, False))

    def test_weapon_and_incoming_effects_work_at_level_two(self):
        for entry, sid in ((17182, 21162), (18348, 22850), (18816, 23267), (19019, 21992)):
            with self.subTest(entry=entry):
                procs = [p for seed in range(300)
                         for p in self.service.handle(self.request(entry, seed, level=2)).get('procs', [])]
                self.assertTrue(any(p['spell'] == sid and (p['events'] or p['auras']) for p in procs))
                self.assertTrue(all(p['cost'] == 0 and p['gcd_ms'] == 0 for p in procs))
        req = {**self.request(17182, level=2), 'op': 'incoming_hit', 'weapon_hit': True}
        self.assertTrue(self.service.handle(req)['procs'][0]['events'])
        spells = {p['spell'] for seed in range(500)
                  for p in self.service.handle({**self.request(1168, seed, level=2),
                      'op': 'incoming_hit', 'weapon_hit': True}).get('procs', [])}
        self.assertEqual(spells, {18817, 18818})


if __name__ == '__main__':
    unittest.main()
