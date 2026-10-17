"""Every 1.12.1 Paladin talent rank, through the public service protocol (service/paladin_talents.py and the generic
talent machinery), as the Druid's talent probes do it.

Each probe checks every rank of its talent against an untalented control with the same legal allocation (the lower
tiers filled in the talent's own tree, prerequisites included). Expected values come from the 1.12.1 Spell.dbc rows of
the catalog and the cited CMaNGOS Classic routines, not from the module under test.
"""
import json
import unittest

from service import skyrim, character, formulas as F, melee, paladin, paladin_defense, paladin_talents as PT
from service.catalog import Catalog
from service.server import Service
from service.talents import Talents

HOSTILE = {'kind': 'unit', 'level': 60, 'distance_yd': 5, 'reaction': 'hostile', 'alive': True, 'id': 7}
FRIEND = {'kind': 'unit', 'level': 60, 'distance_yd': 5, 'reaction': 'friendly', 'alive': True, 'id': 8}
SELF = {'kind': 'self'}
SWING = {'level': 60, 'distance_yd': 2, 'reaction': 'hostile', 'alive': True, 'id': 7}   # a melee op's target: no kind
UNDEAD = {**HOSTILE, 'creature_type': 'Undead'}
NATIVE = {'restoration': 50, 'alteration': 50, 'one_handed': 50, 'two_handed': 50, 'destruction': 50}


class LowRoll:
    """A die that always rolls its lowest side (talent_effects._LowRoll): the bottom of a rank's range."""
    def randint(self, lo, hi):
        return lo


class PaladinTalents(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = Catalog()
        definition = Talents().data['classes']['paladin']
        cls.trees = definition['trees']
        cls.talents = {t['id']: (tree, t) for tree in definition['trees'] for t in tree['talents']}
        cls.ids = {t['name']: t['id'] for _, t in cls.talents.values()}
        items = sorted(cls.cat.items.items())
        cls.sword = next(e for e, it in items if it['class'] == 2 and it['InventoryType'] in (13, 21) and it['subclass'] == 7)
        cls.two_hander = next(e for e, it in items if it['class'] == 2 and it['InventoryType'] == 17 and it['subclass'] == 8)
        cls.shield = next(e for e, it in items if it['InventoryType'] == character.INVTYPE_SHIELD)

    def setUp(self):
        self.svc = Service(self.cat, None)

    # --- helpers -----------------------------------------------------------------------------------------------------

    def talent(self, name):
        return self.talents[self.ids[name]][1]

    def each(self, name):
        return range(1, self.talent(name)['max_rank'] + 1)

    def rank_spell(self, name, rank):
        return self.talent(name)['rank_spells'][rank - 1]

    def ranks(self, name, rank, *also):
        """The legal allocation: the talent's prerequisites (recursively), its tree's lower tiers filled to 5 x tier
        points, the talent at this rank (0: the control), and any other talents (name, rank) given."""
        out = {}
        for wanted, wanted_rank in ((name, rank),) + also:
            tree, talent = self.talents[self.ids[wanted]]
            r = {}

            def prereq(t):
                for p in t['prerequisites']:
                    r[p['talent']] = max(r.get(p['talent'], 0), p['rank'])
                    prereq(self.talents[p['talent']][1])
            prereq(talent)
            for t in sorted(tree['talents'], key=lambda t: (t['tier'], t['id'])):
                while (t['tier'] < talent['tier'] and t['id'] != talent['id'] and sum(r.values()) < 5 * talent['tier']
                       and r.get(t['id'], 0) < t['max_rank']):
                    r[t['id']] = r.get(t['id'], 0) + 1
            if wanted_rank:
                r[talent['id']] = wanted_rank
            for tid, value in r.items():
                out[tid] = max(out.get(tid, 0), value)
        return {str(tid): value for tid, value in out.items()}

    def caster(self, name, rank, *also, **extra):
        return {'class': 'paladin', 'race': 1, 'level': 60, 'talents': self.ranks(name, rank, *also), **extra}

    def ask(self, req):
        return self.svc.respond(json.dumps(req).encode())

    def ok(self, req):
        out = self.ask(req)
        self.assertTrue(out['ok'], out)
        return out

    def cast(self, name, rank, spell, target=SELF, seed=1, *also, **extra):
        return self.ok({'op': 'resolve', 'spell': spell, 'seed': seed, 'target': target,
                        'caster': self.caster(name, rank, *also, **extra)})

    def refused(self, name, rank, spell, target=SELF, **extra):
        out = self.ask({'op': 'resolve', 'spell': spell, 'seed': 1, 'target': target, 'caster': self.caster(name, rank, **extra)})
        self.assertFalse(out['ok'])
        return out['error']

    def stats(self, name, rank, *also, **extra):
        return self.ok({'op': 'stats', 'caster': self.caster(name, rank, *also, **extra)})

    def char(self, name, rank, *also, **extra):
        return self.svc.char(self.caster(name, rank, *also, **extra))

    def value(self, sid, eff=1):
        """The rank's own value (Spell.dbc base points + base dice, its maximum roll at level 60)."""
        return int(F.effect_value(self.cat.spell(sid), eff, 60, None, maximum=True))

    def tooltip(self, name, rank, spell, eff=1, target=SELF, **extra):
        return self.cast(name, rank, spell, target, **extra)['tooltip']['effects'][eff - 1]

    def roll(self, spell, eff=1):
        """The rank's unmodified roll range at level 60 (WorldObject::CalculateSpellEffectValue keeps the per-level
        fraction until the spell mods are applied, then truncates)."""
        s = self.cat.spell(spell)
        return [F.effect_value(s, eff, 60, LowRoll()), F.effect_value(s, eff, 60, None, maximum=True)]

    def percent_of(self, name, spell, eff=1, target=SELF, flat=False, **extra):
        """A spell mod on the cast's effect: SPELLMOD_ALL_EFFECTS percent (or flat) of the rank's own roll, every rank."""
        raw = self.roll(spell, eff)
        base = self.tooltip(name, 0, spell, eff, target, **extra)
        self.assertEqual(base, [int(v) for v in raw])
        for rank in self.each(name):
            mod = self.value(self.rank_spell(name, rank))
            actual = self.tooltip(name, rank, spell, eff, target, **extra)
            expected = [int(v + mod) for v in raw] if flat else [int(v * (100 + mod) / 100) for v in raw]
            self.assertEqual(actual, expected, (name, rank))
            self.assertGreater(actual, base)

    def incoming(self, name, rank, seed, attack, *also, **extra):
        return self.ok({'op': 'incoming_hit', 'seed': seed, 'target': HOSTILE, 'attack': attack,
                        'caster': self.caster(name, rank, *also, **extra)})

    def gated(self, name, spells, target=SELF, requires=(), **extra):
        """A talent-taught spell (and its trainer ranks): SPELL_FAILED_NOT_KNOWN without the talent, cast with it; the
        talents op teaches the rank and the book (every Skyrim skill at 100) holds the ranks and the trainer spells that
        require it (requires: e.g. Greater Blessing of Kings, npc_trainer ReqAbility / progression required_spell)."""
        for spell in spells:
            self.assertEqual(self.refused(name, 0, spell, target, **extra), 'SPELL_FAILED_NOT_KNOWN', spell)
            self.cast(name, 1, spell, target, **extra)
        talents = self.ok({'op': 'talents', 'class': 'paladin', 'caster': {'level': 60, 'talents': self.ranks(name, 1)}})
        self.assertIn(self.rank_spell(name, 1), talents['learned_spells'])
        skills = {k: 100 for k in ('restoration', 'block', 'two_handed', 'one_handed', 'alteration', 'destruction')}
        for rank, present in ((1, True), (0, False)):
            book = self.ok({'op': 'known_spells', 'class': 'paladin',
                            'caster': {'level': 60, 'skyrim': skills, 'talents': self.ranks(name, rank)}})['known_spells']
            for spell in tuple(spells) + tuple(requires):
                self.assertEqual(spell in book, present, (name, rank, spell))

    # --- every talent has a probe ----------------------------------------------------------------------------------

    def test_every_talent_has_a_rank_probe(self):
        names = {t['name'] for _, t in self.talents.values()}
        self.assertEqual(len(names), 44)
        for name in names:
            method = 'test_' + name.lower().replace("'", '').replace('-', '_').replace(' ', '_')
            self.assertTrue(callable(getattr(self, method, None)), method)

    def test_rank_ids_are_the_talent_data_and_the_manifest_links(self):
        for name, ranks in (('Illumination', PT.ILLUMINATION), ('Improved Lay on Hands', PT.IMPROVED_LAY_ON_HANDS),
                            ('Vindication', PT.VINDICATION), ('Vengeance', PT.VENGEANCE), ('Eye for an Eye', PT.EYE_FOR_AN_EYE),
                            ('Improved Seal of Righteousness', PT.IMPROVED_SEAL_OF_RIGHTEOUSNESS),
                            ('Improved Seal of the Crusader', PT.IMPROVED_SEAL_OF_THE_CRUSADER),
                            ('Divine Favor', (PT.DIVINE_FAVOR,))):
            self.assertEqual(tuple(self.talent(name)['rank_spells']), ranks, name)
        self.assertEqual(sorted(PT.HOLY_SHOCK), sorted(s for s in self.cat.ranks if self.cat.chain(s) == 20473))
        links = paladin.manifest()['links']
        for rank, (hurt, heal) in PT.HOLY_SHOCK.items():
            dummy = {l['note']: l['to'] for l in links if l['kind'] == 'dummy' and l['from'] == rank}
            self.assertEqual(dummy, {'enemy target': hurt, 'friendly target (CanAssistSpell)': heal})
        self.assertEqual({l['to'] for l in links if l['kind'] == 'proc_override' and l['from'] in PT.ILLUMINATION},
                         {PT.ILLUMINATION_MANA})
        self.assertEqual({l['to'] for l in links if l['kind'] == 'dummy_proc' and l['from'] in PT.EYE_FOR_AN_EYE},
                         {PT.EYE_FOR_AN_EYE_DAMAGE})

    # --- Holy --------------------------------------------------------------------------------------------------------

    def test_divine_strength(self):
        base = self.stats('Divine Strength', 0)['stats']
        for rank in self.each('Divine Strength'):
            out = self.stats('Divine Strength', rank)['stats']
            # SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE: Unit::GetTotalStatValue's TOTAL_PCT (formulas.stat_total).
            self.assertEqual(out['str'], F.stat_total(base['str'], 0, (), (2 * rank,)))
            self.assertEqual({k: v for k, v in out.items() if k != 'str'}, {k: v for k, v in base.items() if k != 'str'})

    def test_divine_intellect(self):
        base = self.stats('Divine Intellect', 0)
        for rank in self.each('Divine Intellect'):
            out = self.stats('Divine Intellect', rank)
            intellect = F.stat_total(base['stats']['int'], 0, (), (2 * rank,))
            self.assertEqual(out['stats']['int'], intellect)
            self.assertEqual(out['max_mana'], base['max_mana'] - round(F.mana_from_intellect(base['stats']['int']))
                             + round(F.mana_from_intellect(intellect)))

    def test_spiritual_focus(self):
        # SPELLMOD_NOT_LOSE_CASTING_TIME (Spell::Delayed, Spell.cpp:6817): 14/28/42/56/70% on Holy Light and Flash of Light.
        for rank, chance in zip(self.each('Spiritual Focus'), (14, 28, 42, 56, 70)):
            for spell in (25292, 19943, 635):
                self.assertEqual(self.cast('Spiritual Focus', rank, spell)['pushback_resist_pct'], chance)
            self.assertEqual(self.cast('Spiritual Focus', rank, 10314, UNDEAD)['pushback_resist_pct'], 0)
        self.assertEqual(self.cast('Spiritual Focus', 0, 25292)['pushback_resist_pct'], 0)

    def test_improved_seal_of_righteousness(self):
        # CMaNGOS: the talent's mask (0x400) is Judgement of Righteousness; its damage is the rank's ALL_EFFECTS share
        # (the seal's tooltip quotes its judgement, 20286; Judgement casts it with the same numbers).
        self.assertEqual(self.cat.affect_mask(20224, 0), 0x400)
        lo, hi = self.roll(20286)
        quoted = self.cast('Improved Seal of Righteousness', 0, 20293)['tooltip']['linked_spells']['20286']['effects'][0]
        self.assertEqual(quoted, [int(lo), int(hi)])
        for rank in self.each('Improved Seal of Righteousness'):
            pct = self.value(self.rank_spell('Improved Seal of Righteousness', rank))
            self.assertEqual(pct, 3 * rank)
            k = (100 + pct) / 100
            tip = self.cast('Improved Seal of Righteousness', rank, 20293)['tooltip']['linked_spells']['20286']
            self.assertEqual(tip['effects'][0], [int(lo * k), int(hi * k)])
            for seed in range(10):
                event = self.cast('Improved Seal of Righteousness', rank, 20271, HOSTILE, seed, auras=[20293])['procs'][0]['events'][0]
                if not event['crit']:
                    self.assertTrue(int(lo * k) <= event['amount'] <= int(hi * k), event)
        # Labelled custom rule: the same share of the seal's own amount (its proc damage), only with custom rules on.
        seal = self.cat.spell(20293)
        amount = int(F.effect_value(seal, 1, 60, None))
        for rank in self.each('Improved Seal of Righteousness'):
            caster = self.caster('Improved Seal of Righteousness', rank, items=[self.sword], auras=[20293])
            ch = self.svc.char(caster)
            self.assertEqual(PT.righteousness_amount(ch, seal, amount), int(amount * (100 + 3 * rank) / 100))
            control = self.svc.char({**caster, 'talents': self.ranks('Improved Seal of Righteousness', 0)})
            self.assertGreaterEqual(paladin.righteousness_damage(self.cat, ch, 20293, SWING),
                                    paladin.righteousness_damage(self.cat, control, 20293, SWING))
            ch.custom_rules = False          # as tools/oracle.py turns WoWBridge's rules off: CMaNGOS's seal
            self.assertEqual(PT.righteousness_amount(ch, seal, amount), amount)
            self.assertEqual(paladin.righteousness_damage(self.cat, ch, 20293, SWING),
                             paladin.righteousness_damage(self.cat, control, 20293, SWING))
            ch.custom_rules = True
        rank5 = self.svc.char(self.caster('Improved Seal of Righteousness', 5, items=[self.sword], auras=[20293]))
        control = self.svc.char(self.caster('Improved Seal of Righteousness', 0, items=[self.sword], auras=[20293]))
        self.assertGreater(paladin.righteousness_damage(self.cat, rank5, 20293, SWING),
                           paladin.righteousness_damage(self.cat, control, 20293, SWING))

    def test_healing_light(self):
        # ALL_EFFECTS on the rank (mask 0xC0000000): the custom basepoint of 19968 / 19993, so the heal itself.
        for spell in (25292, 19943):
            self.percent_of('Healing Light', spell)
        self.assertEqual(self.cat.affect_mask(20237, 0), 0xC0000000)

    def test_consecration(self):
        self.gated('Consecration', (26573, 20116, 20922, 20923, 20924))
        aura = self.cast('Consecration', 1, 26573)['auras'][0]
        self.assertEqual((aura['type'], aura['per_tick'], aura['area']['channel']), ('damage', 8, False))
        caster = {'level': 60, 'skyrim': {'restoration': 100}}
        with_talent = self.ok({'op': 'known_spells', 'class': 'paladin',
                               'caster': {**caster, 'talents': self.ranks('Consecration', 1)}})['known_spells']
        without = self.ok({'op': 'known_spells', 'class': 'paladin',
                           'caster': {**caster, 'talents': self.ranks('Consecration', 0)}})['known_spells']
        self.assertTrue({26573, 20116, 20922, 20923, 20924} <= set(with_talent))
        self.assertFalse({26573, 20116, 20922, 20923, 20924} & set(without))

    def test_improved_lay_on_hands(self):
        control = self.cast('Improved Lay on Hands', 0, 10310, FRIEND)
        self.assertEqual(control['cooldown_ms'], 3600000)
        self.assertNotIn('procs', control)
        for rank in self.each('Improved Lay on Hands'):
            sid = self.rank_spell('Improved Lay on Hands', rank)
            trigger = self.cat.spell(sid)['EffectTriggerSpell1']
            for target, side in ((FRIEND, 'target'), (SELF, 'caster')):
                out = self.cast('Improved Lay on Hands', rank, 10310, target)
                # SPELLMOD_COOLDOWN flat -10/-20 minutes (effect 2).
                self.assertEqual(out['cooldown_ms'], 3600000 + self.value(sid, 2))
                proc, = out['procs']
                aura, = proc['auras']
                self.assertEqual((proc['source_spell'], proc['spell'], proc['target']), (sid, trigger, side))
                self.assertEqual((aura['type'], aura['amount'], aura['school_mask'], aura['duration_ms']),
                                 ('armor_from_items_percent', 15 * rank, 1, 120000))
            for spell in (633, 2800):
                self.assertEqual(len(self.cast('Improved Lay on Hands', rank, spell, FRIEND)['procs']), 1)
            self.assertNotIn('procs', self.cast('Improved Lay on Hands', rank, 25292, FRIEND))

    def test_unyielding_faith(self):
        self.assertEqual(self.stats('Unyielding Faith', 0)['paladin']['mechanic_resist_percent'], {})
        for rank in self.each('Unyielding Faith'):
            self.assertEqual(self.stats('Unyielding Faith', rank)['paladin']['mechanic_resist_percent'],
                             {'fear': 5 * rank, 'disoriented': 5 * rank})

    def test_illumination(self):
        # spell_proc_event 20210: family 10, mask 0xC0206000 (the Holy Light / Flash of Light ranks and their linked heals
        # 19968 / 19993, Holy Shock), procEx CRITICAL_HIT; its 18350 trigger is replaced by 20272 (UnitAuraProcHandler.cpp:
        # 1410-1443); the class script 2689 of effect 2 has no handler.
        row = next(r for r in self.cat.raw['spell_proc_event'] if r['entry'] == 20210)
        self.assertEqual((row['SpellFamilyName'], row['SpellFamilyMask0'], row['procEx']), (10, 0xC0206000, 2))
        self.assertEqual((self.cat.spell(19968)['SpellFamilyFlags'], self.cat.spell(19993)['SpellFamilyFlags']), (0x4000, 0x2000))
        self.assertEqual((self.cat.spell(20215)['EffectTriggerSpell1'], self.cat.spell(20215)['EffectMiscValue2']), (18350, 2689))
        # Every rank's chance (ProcChance, SPELLMOD_CHANCE_OF_SUCCESS) on Flash of Light's crits: the rank's manaCost back.
        for rank in self.each('Illumination'):
            sid = self.rank_spell('Illumination', rank)
            self.assertEqual(self.cat.spell(sid)['ProcChance'], 20 * rank)
            procs, crits = 0, 0
            for seed in range(400):
                out = self.cast('Illumination', rank, 19943, FRIEND, seed)
                crits += out['events'][0]['crit']
                for p in out.get('procs', []):
                    self.assertTrue(out['events'][0]['crit'])
                    self.assertFalse(p['spell'] == 18350)           # the overridden 'Dummy Trigger'
                    self.assertEqual((p['spell'], p['source_spell'], p['target'], p['proc_chance'], p['original_spell'],
                                      p['from_spell'], p['events']),
                                     (PT.ILLUMINATION_MANA, sid, 'caster', 20 * rank, 19943, 19993,
                                      [{'type': 'power', 'power': 'mana', 'amount': 140}]))
                    procs += 1
            self.assertTrue(crits)
            p = 0.2 * rank
            self.assertLessEqual(abs(procs - crits * p), 4 * (crits * p * (1 - p)) ** .5 + 1, (rank, procs, crits))
        # No talent, no proc.
        self.assertFalse(any(self.cast('Illumination', 0, 19943, FRIEND, s).get('procs') for s in range(100)))

    def test_illumination_returns_the_rank_base_cost(self):
        # Rank 5 (100%): Divine Favor (which needs it) makes each heal crit. Holy Light and Flash of Light refund their
        # rank's Spell.dbc manaCost (basepoint 1 of 19968 / 19993 names the rank, SpellEffects.cpp:4160), Holy Shock
        # its rank's (25914 -> 20473 ..., UnitAuraProcHandler.cpp:1425-1427), whatever was paid (Benediction is not one).
        also = (('Divine Favor', 1), ('Holy Shock', 1))
        for spell, cost in ((25292, 660), (19943, 140), (635, 35), (20473, 225), (20929, 275), (20930, 325)):
            out = self.cast('Illumination', 5, spell, FRIEND, 1, *also, auras=[PT.DIVINE_FAVOR])
            proc = out['procs'][-1]
            self.assertEqual((proc['spell'], proc['original_spell'], proc['events'][0]['amount']),
                             (PT.ILLUMINATION_MANA, spell, cost), spell)
            self.assertNotIn('custom_rule', proc)
            # It is CMaNGOS's own behaviour: unchanged with WoWBridge's custom rules off.
            caster = self.caster('Illumination', 5, *also, auras=[PT.DIVINE_FAVOR])
            self.svc.char(caster).custom_rules = False
            again = self.ok({'op': 'resolve', 'spell': spell, 'seed': 1, 'target': FRIEND, 'caster': caster})
            self.assertEqual(again['procs'][-1]['events'], proc['events'])
            self.svc.char(caster).custom_rules = True
        # A damaging Holy Shock's crit returns nothing; nor does a heal that does not crit.
        strike = next(o for s in range(60) for o in [self.cast('Illumination', 5, 20473, HOSTILE, s, *also,
                                                               auras=[PT.DIVINE_FAVOR])]
                      if o['procs'][0]['events'][0].get('crit'))
        self.assertFalse([p for p in strike['procs'] if p['spell'] == PT.ILLUMINATION_MANA])
        normal = next(o for s in range(60) for o in [self.cast('Illumination', 5, 20473, FRIEND, s, *also)]
                      if not o['procs'][0]['events'][0]['crit'])
        self.assertEqual(len(normal['procs']), 1)
        # Native: the rank's base price in magicka as she pays it (its rule's counterpart cost at the WoW mana ratio, with
        # her skill's discount): a critical heal is free, never a profit.
        for spell, cost in ((20473, 225), (25292, 660)):
            native = self.cast('Illumination', 5, spell, FRIEND, 1, *also, auras=[PT.DIVINE_FAVOR], skyrim=NATIVE)
            rule = self.cat.balance[spell]
            discount = skyrim.cost_factor(NATIVE.get(skyrim.SKILLS[rule.skill], 50))
            self.assertEqual(native['procs'][-1]['events'][0]['amount'], round(rule.base_cost * discount * cost / rule.wow_cost))

    def test_improved_blessing_of_wisdom(self):
        self.percent_of('Improved Blessing of Wisdom', 25290, target=FRIEND)

    def test_divine_favor(self):
        self.gated('Divine Favor', (PT.DIVINE_FAVOR,))
        cast = self.cast('Divine Favor', 1, PT.DIVINE_FAVOR)
        aura, = cast['auras']
        self.assertEqual((aura['type'], aura['op'], aura['amount'], aura['charges']), ('spell_mod', 7, 100, 1))
        self.assertEqual(cast['cooldown_ms'], 120000)
        for spell in (25292, 19943):
            for seed in range(5):
                out = self.cast('Divine Favor', 1, spell, FRIEND, seed, auras=[PT.DIVINE_FAVOR])
                self.assertEqual((out['crit_chance'], out['events'][0]['crit'], out['consumed_auras']),
                                 (100.0, True, [PT.DIVINE_FAVOR]))
        # Not Exorcism: neither its crit nor the charge.
        plain = self.cast('Divine Favor', 1, 10314, UNDEAD, 1)
        favoured = self.cast('Divine Favor', 1, 10314, UNDEAD, 1, auras=[PT.DIVINE_FAVOR])
        self.assertEqual(favoured['crit_chance'], plain['crit_chance'])
        self.assertNotIn('consumed_auras', favoured)
        # Holy Shock (mask 0x200000): its heal and strike crit and spend the charge.
        also = (('Holy Shock', 1),)
        for target, kind in ((FRIEND, 'heal'), (HOSTILE, 'damage')):
            landed = 0
            for seed in range(10):
                out = self.cast('Divine Favor', 1, 20473, target, seed, *also, auras=[PT.DIVINE_FAVOR])
                event = out['procs'][0]['events'][0]
                if event['type'] == kind:            # a strike's own hit roll may miss: no crit rolled, no charge spent
                    landed += 1
                    self.assertEqual((event['crit'], out['procs'][0]['crit_chance'], out['consumed_auras'][:1]),
                                     (True, 100.0, [PT.DIVINE_FAVOR]))
                else:
                    self.assertNotIn(PT.DIVINE_FAVOR, out.get('consumed_auras', []))
            self.assertTrue(landed)

    def test_lasting_judgement(self):
        for seal in (20349, 20357):            # Seal of Light -> Judgement of Light, Seal of Wisdom -> of Wisdom
            control = self.cast('Lasting Judgement', 0, 20271, HOSTILE, 1, auras=[seal])['procs'][0]['auras'][0]
            self.assertEqual(control['duration_ms'], 10000)
            for rank in self.each('Lasting Judgement'):
                aura = self.cast('Lasting Judgement', rank, 20271, HOSTILE, 1, auras=[seal])['procs'][0]['auras'][0]
                self.assertEqual(aura['duration_ms'], 10000 + 10000 * rank)
        crusader = self.cast('Lasting Judgement', 3, 20271, HOSTILE, 1, auras=[20308])['procs'][0]['auras'][0]
        self.assertEqual(crusader['duration_ms'], 10000)

    def test_holy_power(self):
        base = self.stats('Holy Power', 0)['spell_crit']
        heal = self.cast('Holy Power', 0, 25292, FRIEND)['crit_chance']
        for rank in self.each('Holy Power'):
            crit = self.stats('Holy Power', rank)['spell_crit']
            self.assertAlmostEqual(crit['1'], base['1'] + rank)
            self.assertEqual({k: v for k, v in crit.items() if k != '1'}, {k: v for k, v in base.items() if k != '1'})
            self.assertAlmostEqual(self.cast('Holy Power', rank, 25292, FRIEND)['crit_chance'], heal + rank)

    def test_holy_shock(self):
        self.gated('Holy Shock', (20473, 20929, 20930), FRIEND)
        for spell, (hurt, heal) in PT.HOLY_SHOCK.items():
            s = self.cat.spell(spell)
            out = self.cast('Holy Shock', 1, spell, HOSTILE)
            self.assertEqual((out['cost'], out['cooldown_ms'], out['gcd_ms']), (s['ManaCost'], 30000, 1500))
            self.assertEqual(out['category_spells'], [20473, 20929, 20930])
            self.assertEqual((out['cooldown_category'], out['category_cooldown_ms']), (892, 30000))
            self.assertEqual(out['events'], [])
            for target, payload, kind, side in ((HOSTILE, hurt, 'damage', 'target'), (FRIEND, heal, 'heal', 'target'),
                                                (SELF, heal, 'heal', 'caster')):
                lo, hi = (int(v) for v in self.roll(payload))
                seen = 0
                for seed in range(20):
                    proc, = [p for p in self.cast('Holy Shock', 1, spell, target, seed)['procs'] if p.get('holy_shock')]
                    self.assertEqual((proc['spell'], proc['holy_shock'], proc['target']), (payload, kind, side))
                    event, = proc['events']
                    if event['type'] == kind and not event['crit']:
                        seen += 1
                        self.assertTrue(lo <= event['amount'] + event.get('resisted', 0) <= hi, event)
                    elif event['type'] == kind:
                        self.assertTrue(int(lo * 1.5) <= event['amount'] + event.get('resisted', 0) <= int(hi * 1.5))
                    else:
                        self.assertIn(event['type'], ('miss', 'resist'))
                self.assertTrue(seen)
        # Spell::CheckCast: an enemy must be in front; a unit she can neither attack nor assist is refused.
        self.assertEqual(self.refused('Holy Shock', 1, 20473, {**HOSTILE, 'in_front': False}), 'SPELL_FAILED_UNIT_NOT_INFRONT')
        self.cast('Holy Shock', 1, 20473, {**FRIEND, 'in_front': False})
        self.assertEqual(self.refused('Holy Shock', 1, 20473, {**HOSTILE, 'attackable': False}), 'SPELL_FAILED_BAD_TARGETS')
        self.assertEqual(self.refused('Holy Shock', 1, 20473, {**HOSTILE, 'distance_yd': 25}), 'SPELL_FAILED_OUT_OF_RANGE')
        # Its tooltip quotes both spells; native, each converts at its own rule (Wrath's strike, Healing Touch's heal).
        tip = self.cast('Holy Shock', 1, 20473, FRIEND)['tooltip']['linked_spells']
        self.assertEqual(tip['25912']['effects'][0], [204, 220])
        self.assertEqual(tip['25914']['effects'][0], [204, 220])
        native = self.cast('Holy Shock', 1, 20473, FRIEND, 1, skyrim=NATIVE)
        plain = self.cast('Holy Shock', 1, 20473, FRIEND, 1)
        self.assertEqual(native['procs'][0]['events'][0]['amount'],
                         max(1, round(plain['procs'][0]['events'][0]['amount'] * self.cat.balance[25914].health_factor)))
        # Its strike on a unit under her Judgement of Wisdom (25912: triggered, SPELL_ATTR_EX3_NOT_A_PROC; a harmful
        # spell, TAKE_HARMFUL_SPELL) may return her mana, at the judgement's 50%; a strike that failed raises nothing.
        mana, landed = [], 0
        for seed in range(40):
            out = self.cast('Holy Shock', 1, 20473, {**HOSTILE, 'judgements': [20355]}, seed)
            strike, = [p for p in out['procs'] if p.get('holy_shock')]
            procs = [p for p in out['procs'] if p.get('from_spell') == 25912]
            if any(e['type'] == 'damage' and e['amount'] > 0 for e in strike['events']):
                landed += 1
            else:
                self.assertEqual(procs, [])
            mana += procs
        self.assertTrue(.25 < len(mana) / landed < .75)
        self.assertTrue(all((p['source_spell'], p['spell'], p['target'], p['events']) ==
                            (20355, 20353, 'caster', [{'type': 'power', 'power': 'mana', 'amount': 59}]) for p in mana))

    # --- Protection ----------------------------------------------------------------------------------------------

    def test_improved_devotion_aura(self):
        self.percent_of('Improved Devotion Aura', 10293)

    def test_redoubt(self):
        attack = {'damage': 100, 'kind': 'spell', 'school': 2, 'crit': True}
        self.assertFalse([p for p in self.incoming('Redoubt', 0, 1, attack).get('procs', [])])
        for rank in self.each('Redoubt'):
            buff = self.cat.spell(self.rank_spell('Redoubt', rank))['EffectTriggerSpell1']
            proc, = self.incoming('Redoubt', rank, 1, attack)['procs']
            aura, = proc['auras']
            self.assertEqual((proc['spell'], proc['target'], aura['type'], aura['amount'], aura['charges'], aura['duration_ms']),
                             (buff, 'caster', 'block_chance', 6 * rank, 5, 10000))
            # A normal hit raises nothing.
            self.assertNotIn('procs', self.incoming('Redoubt', rank, 1, {**attack, 'crit': False}))

    def test_precision(self):
        target = {'level': 60}
        base_sword = melee.chances(self.char('Precision', 0, items=[self.sword]), target)['miss']
        for items, fits in (([self.sword], True), ([self.two_hander], True), ([], False)):
            base = melee.chances(self.char('Precision', 0, items=items), target)['miss']
            for rank in self.each('Precision'):
                ch = self.char('Precision', rank, items=items)
                self.assertAlmostEqual(melee.chances(ch, target)['miss'], base - (rank if fits else 0))
                self.assertEqual(self.stats('Precision', rank, items=items)['paladin']['melee_hit_percent'],
                                 rank if fits else 0)
                self.assertEqual(ch.spell_hit(), 0)
        # A melee ability (Seal of Command's 20424: MeleeSpellHitResult) takes it off once, as the swing does: the miss
        # side of its die ends exactly at the swing table's miss.
        class Roll:
            def __init__(self, value):
                self.value = value

            def randint(self, lo, hi):
                return self.value
        strike = self.cat.spell(20424)
        for rank in (0,) + tuple(self.each('Precision')):
            ch = self.char('Precision', rank, items=[self.sword])
            edge = int(melee.chances(ch, {**target, 'in_front': True})['miss'] * 100)
            self.assertEqual(edge, int((base_sword - rank) * 100))
            self.assertEqual(melee.ability_outcome(ch, strike, {**target, 'in_front': True}, Roll(edge)), 'miss')
            self.assertNotEqual(melee.ability_outcome(ch, strike, {**target, 'in_front': True}, Roll(edge + 1)), 'miss')

    def test_guardians_favor(self):
        self.assertEqual(self.cast("Guardian's Favor", 0, 10278)['cooldown_ms'], 300000)
        self.assertEqual(self.cast("Guardian's Favor", 0, 1044)['auras'][0]['duration_ms'], 10000)
        self.assertEqual(self.cast("Guardian's Favor", 0, 10278)['category_cooldown_ms'], 300000)
        for rank in self.each("Guardian's Favor"):
            protection = self.cast("Guardian's Favor", rank, 10278)
            self.assertEqual((protection['cooldown_ms'], protection['category_cooldown_ms']), (300000 - 60000 * rank,) * 2)
            freedom = self.cast("Guardian's Favor", rank, 1044)
            self.assertEqual([a['duration_ms'] for a in freedom['auras']], [10000 + 3000 * rank] * 2)
            self.assertEqual(freedom['cooldown_ms'], 20000)

    def test_toughness(self):
        self.assertEqual(self.stats('Toughness', 0)['paladin']['item_armor_multiplier'], 1.0)
        for rank in self.each('Toughness'):
            out = self.stats('Toughness', rank)
            self.assertAlmostEqual(out['paladin']['item_armor_multiplier'], 1 + 0.02 * rank)
            self.assertAlmostEqual(out['form']['armor_percent'], 2 * rank)

    def test_blessing_of_kings(self):
        self.gated('Blessing of Kings', (20217,), FRIEND, requires=(25898,))   # Greater Blessing of Kings
        aura, = self.cast('Blessing of Kings', 1, 20217, FRIEND)['auras']
        self.assertEqual((aura['aura'], aura['amount']), (137, 10))
        # SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE on every stat (misc -1), with the Human's own 5% Spirit (20598) beside it.
        create, _ = character.base_rows(self.cat.raw['character'], 2, 1, 60)
        blessed = self.stats('Blessing of Kings', 1, auras=[20217])['stats']
        human = {'spi': (self.value(20598),)}
        self.assertEqual(blessed, {k: F.stat_total(create['inte' if k == 'int' else k], 0, (), human.get(k, ()) + (10,))
                                   for k in ('str', 'agi', 'sta', 'int', 'spi')})
        self.assertEqual(self.stats('Blessing of Kings', 1)['stats'],
                         {k: F.stat_total(create['inte' if k == 'int' else k], 0, (), human.get(k, ()))
                          for k in ('str', 'agi', 'sta', 'int', 'spi')})

    def test_improved_righteous_fury(self):
        self.assertEqual(self.cast('Improved Righteous Fury', 0, 25780)['auras'][0]['amount'], 60)
        for rank, pct in zip(self.each('Improved Righteous Fury'), (16, 33, 50)):
            aura, = self.cast('Improved Righteous Fury', rank, 25780)['auras']
            amount = int(60 * (100 + pct) / 100)
            self.assertEqual((aura['amount'], aura['multiplier']), (amount, 1 + amount / 100))
            exorcism = self.cast('Improved Righteous Fury', rank, 10314, UNDEAD, auras=[25780])
            self.assertAlmostEqual(exorcism['threat_multiplier'], 1 + amount / 100)

    def test_shield_specialization(self):
        control = self.char('Shield Specialization', 0, items=[self.shield])
        for rank in self.each('Shield Specialization'):
            out = self.stats('Shield Specialization', rank, items=[self.shield])
            self.assertEqual(out['defense']['block_value'],
                             F.shield_block_value(control.block_flat, control.stats['str'], 1 + 0.1 * rank))

    def test_anticipation(self):
        base = paladin_defense.defense_table(self.char('Anticipation', 0, items=[self.sword, self.shield]), {'level': 60})
        self.assertEqual(self.stats('Anticipation', 0)['paladin']['defense_skill'], 300)
        for rank in self.each('Anticipation'):
            self.assertEqual(self.stats('Anticipation', rank)['paladin']['defense_skill'], 300 + 2 * rank)
            table = paladin_defense.defense_table(self.char('Anticipation', rank, items=[self.sword, self.shield]),
                                                  {'level': 60})
            for side in ('miss', 'dodge', 'parry', 'block'):
                self.assertAlmostEqual(table[side], base[side] + 0.08 * rank, msg=side)
            self.assertAlmostEqual(table['crit'], base['crit'] - 0.08 * rank)

    def test_improved_hammer_of_justice(self):
        out = self.cast('Improved Hammer of Justice', 0, 10308, HOSTILE)
        self.assertEqual((out['cooldown_ms'], out['cooldown_category'], out['category_cooldown_ms']), (60000, 32, 60000))
        for rank in self.each('Improved Hammer of Justice'):
            out = self.cast('Improved Hammer of Justice', rank, 10308, HOSTILE)
            # Player::AddCooldown (Player.cpp:20264-20269): SPELLMOD_COOLDOWN on the spell's own and its category's time.
            self.assertEqual((out['cooldown_ms'], out['category_cooldown_ms']), (60000 - 5000 * rank,) * 2)

    def test_improved_concentration_aura(self):
        # Flat ALL_EFFECTS on all three effects: pushback (35%) and the silence/interrupt mechanic resistances (0%).
        for eff in (1, 2, 3):
            self.percent_of('Improved Concentration Aura', 19746, eff, flat=True)

    def test_blessing_of_sanctuary(self):
        self.gated('Blessing of Sanctuary', (20911, 20912, 20913, 20914), FRIEND, requires=(25899,))

    def test_reckoning(self):
        attack = {'damage': 100, 'kind': 'spell', 'school': 2, 'crit': True}
        for rank in self.each('Reckoning'):
            sid = self.rank_spell('Reckoning', rank)
            self.assertEqual(self.cat.spell(sid)['ProcChance'], 20 * rank)
            procs = 0
            for seed in range(400):
                out = self.incoming('Reckoning', rank, seed, attack)
                extra = [p for p in out.get('procs', []) if p['spell'] == paladin_defense.RECKONING_ATTACK]
                if extra:
                    procs += 1
                    self.assertEqual((extra[0]['source_spell'], extra[0]['proc_chance'], extra[0]['events']),
                                     (sid, 20 * rank, [{'type': 'extra_attacks', 'count': 1, 'max_pending': 5}]))
            # Binomial(400, 0.2 x rank) within four standard deviations.
            p = 0.2 * rank
            self.assertLessEqual(abs(procs - 400 * p), 4 * (400 * p * (1 - p)) ** .5 + 1e-9, (rank, procs))
        control = self.incoming('Reckoning', 0, 1, attack).get('procs', [])   # its filler's Redoubt may proc
        self.assertFalse([p for p in control if p['spell'] == paladin_defense.RECKONING_ATTACK])

    def test_one_handed_weapon_specialization(self):
        name = 'One-Handed Weapon Specialization'
        for items, fits in (([self.sword], True), ([self.two_hander], False), ([], False)):
            base = self.char(name, 0, items=items)
            for rank in self.each(name):
                ch = self.char(name, rank, items=items)
                factor = 1 + 0.02 * rank if fits else 1.0
                self.assertAlmostEqual(ch.multiplier(79, 1), base.multiplier(79, 1) * factor)
                low, high, _ = melee.roll_range(ch, ch.melee_weapon())
                b_low, b_high, _ = melee.roll_range(base, base.melee_weapon())
                self.assertAlmostEqual(low, b_low * factor)
                self.assertAlmostEqual(high, b_high * factor)

    def test_holy_shield(self):
        self.gated('Holy Shield', (20925, 20927, 20928), items=[self.shield])

    # --- Retribution -------------------------------------------------------------------------------------------

    def test_improved_blessing_of_might(self):
        self.percent_of('Improved Blessing of Might', 25291, target=FRIEND)

    def test_benediction(self):
        for spell in (20271, 20293, 20308, 20349, 20357, 20164):
            seal = {'auras': [20293]} if spell == 20271 else {}
            target = HOSTILE if spell == 20271 else SELF
            base = self.cast('Benediction', 0, spell, target, **seal)['cost']
            for rank in self.each('Benediction'):
                cost = self.cast('Benediction', rank, spell, target, **seal)['cost']
                self.assertEqual(cost, int(base * (100 - 3 * rank) / 100), (spell, rank))
        self.assertEqual(self.cast('Benediction', 0, 20271, HOSTILE, auras=[20293])['cost'], 1512 * 6 // 100)
        self.assertEqual(self.cast('Benediction', 5, 25292, FRIEND)['cost'], self.cast('Benediction', 0, 25292, FRIEND)['cost'])

    def test_improved_judgement(self):
        self.assertEqual(self.cast('Improved Judgement', 0, 20271, HOSTILE, auras=[20293])['cooldown_ms'], 10000)
        for rank in self.each('Improved Judgement'):
            self.assertEqual(self.cast('Improved Judgement', rank, 20271, HOSTILE, auras=[20293])['cooldown_ms'],
                             10000 - 1000 * rank)

    def test_improved_seal_of_the_crusader(self):
        name = 'Improved Seal of the Crusader'

        def ap(rank, custom=True):
            sealed = self.caster(name, rank, items=[self.sword], auras=[20308])
            caster = {k: v for k, v in sealed.items() if k != 'auras'}
            for c in (sealed, caster):
                self.svc.char(c).custom_rules = custom      # as tools/oracle.py turns WoWBridge's rules off
            aura = next(a for a in self.ok({'op': 'resolve', 'spell': 20308, 'seed': 1, 'target': SELF,
                                            'caster': caster})['auras'] if a['aura'] == 99)
            sheet = self.ok({'op': 'stats', 'caster': sealed})['attack_power']
            for c in (sealed, caster):
                self.svc.char(c).custom_rules = True
            return aura['amount'], sheet
        seal, sheet = ap(0)
        jotc = self.cast(name, 0, 20271, HOSTILE, 1, auras=[20308])['procs'][0]['auras'][0]['amount']
        for rank in self.each(name):
            pct = self.value(self.rank_spell(name, rank))
            # Judgement of the Crusader's holy damage taken (ALL_EFFECTS, mask 0x20000000): CMaNGOS.
            aura = self.cast(name, rank, 20271, HOSTILE, 1, auras=[20308])['procs'][0]['auras'][0]
            self.assertEqual((aura['type'], aura['amount']), ('damage_taken', int(jotc * (100 + pct) / 100)))
            # The seal's attack power (SPELLMOD_ATTACK_POWER): a labelled custom rule; CMaNGOS keeps the base amount.
            boosted, boosted_sheet = ap(rank)
            self.assertEqual(boosted, int(seal * (100 + pct) / 100))
            self.assertEqual(boosted_sheet, sheet + boosted - seal)
            self.assertEqual(ap(rank, custom=False), (seal, sheet))

    def test_deflection(self):
        base = self.stats('Deflection', 0)['defense']['parry']
        for rank in self.each('Deflection'):
            self.assertEqual(self.stats('Deflection', rank)['defense']['parry'], base + rank)

    def test_vindication(self):
        name = 'Vindication'
        for rank in self.each(name):
            debuff = self.cat.spell(self.rank_spell(name, rank))['EffectTriggerSpell1']
            caster = self.caster(name, rank, items=[self.two_hander])
            delay = self.cat.item(self.two_hander)['delay']
            procs, landed = 0, 0
            for seed in range(300):
                out = self.ok({'op': 'melee', 'seed': seed, 'caster': caster, 'target': SWING})
                mine = [p for p in out.get('procs', []) if p['source_spell'] == self.rank_spell(name, rank)]
                landed += out['outcome'] in ('hit', 'crit', 'glance', 'block') and out['damage'] > 0
                if mine:
                    procs += 1
                    p, = mine
                    self.assertEqual((p['spell'], p['target']), (debuff, 'target'))
                    if p['auras']:          # its own magic hit roll may miss the target
                        self.assertEqual([(a['type'], a['stat'], a['amount'], a['duration_ms'], a['debuff']) for a in p['auras']],
                                         [('stat_percent', 0, -5 * rank, 10000, True), ('stat_percent', 1, -5 * rank, 10000, True)])
            # spell_proc_event 9452: 3 procs a minute at the weapon's own speed (UnitAuraProcHandler.cpp:592-604).
            p = 3 * delay / 600 / 100
            self.assertLessEqual(abs(procs - landed * p), 4 * (landed * p * (1 - p)) ** .5 + 1, (rank, procs, landed))
        none = [self.ok({'op': 'melee', 'seed': s, 'caster': self.caster(name, 0, items=[self.two_hander]), 'target': SWING})
                for s in range(50)]
        self.assertFalse(any(p['source_spell'] in PT.VINDICATION for o in none for p in o.get('procs', [])))
        # Seal of Command's strike (SPELL_ATTR_EX3_NOT_A_PROC, a melee ability) can raise it too.
        caster = self.caster(name, 3, ('Seal of Command', 1), items=[self.two_hander], auras=[20920])
        from_seal = [p for s in range(400) for p in self.ok({'op': 'melee', 'seed': s, 'caster': caster,
                                                             'target': SWING}).get('procs', [])
                     if p.get('from_spell') == 20424]
        self.assertTrue(from_seal and all(p['source_spell'] == 26021 for p in from_seal))

    def test_conviction(self):
        for items, fits in (([self.sword], True), ([self.two_hander], True), ([], False)):
            base = self.stats('Conviction', 0, items=items)['melee_crit']
            for rank in self.each('Conviction'):
                self.assertAlmostEqual(self.stats('Conviction', rank, items=items)['melee_crit'], base + (rank if fits else 0))

    def test_hammer_of_wrath_takes_no_main_hand_weapon_auras(self):
        # Hammer of Wrath is DmgClass RANGED: its hit and crit are the ranged attack type's (UpdateWeaponHitChances,
        # StatSystem.cpp:518; _ApplyWeaponDependentAuraCritMod, Player.cpp:7191). Precision and Conviction want her main
        # hand's weapon (EquippedItemClass 2), and she has no ranged weapon: neither moves it, with any weapon.
        low = {**HOSTILE, 'health_pct': 10}
        for name, key in (('Precision', 'hit_chance'), ('Conviction', 'crit_chance')):
            for items in ([self.sword], [self.two_hander], [self.sword, 12651]):   # 12651: a crossbow (subclass 18)
                base = self.cast(name, 0, 24239, low, items=items)
                for rank in self.each(name):
                    self.assertEqual(self.cast(name, rank, 24239, low, items=items)[key], base[key], (name, rank, items))
        self.assertEqual(self.cast('Precision', 0, 24239, low, items=[self.sword])['hit_chance'], 95.0)

    def test_seal_of_command(self):
        self.gated('Seal of Command', (20375, 20915, 20918, 20919, 20920))

    def test_pursuit_of_justice(self):
        control = self.stats('Pursuit of Justice', 0)
        self.assertEqual((control['paladin']['movement_speed_percent'], control['paladin']['mounted_speed_percent']), (0, 0))
        for rank in self.each('Pursuit of Justice'):
            out = self.stats('Pursuit of Justice', rank)
            self.assertEqual((out['paladin']['movement_speed_percent'], out['paladin']['mounted_speed_percent'],
                              out['form']['movement_speed_percent']), (4 * rank,) * 3)

    def test_eye_for_an_eye(self):
        name = 'Eye for an Eye'
        crit = {'damage': 1000, 'kind': 'spell', 'school': 2, 'crit': True}
        for rank in self.each(name):
            sid = self.rank_spell(name, rank)
            share = self.value(sid)
            self.assertEqual(share, 15 * rank)
            struck = 0
            for seed in range(30):
                out = self.incoming(name, rank, seed, crit)
                taken = out['defense']['damage']
                proc, = [p for p in out['procs'] if p['source_spell'] == sid]
                self.assertEqual((proc['spell'], proc['target']), (PT.EYE_FOR_AN_EYE_DAMAGE, 'target'))
                event, = proc['events']
                if event['type'] == 'damage' and not event['crit'] and not event['resisted']:
                    struck += 1
                    self.assertEqual(event['amount'], share * taken // 100)
            self.assertTrue(struck)
            # At most half her maximum health (int32(GetMaxHealth()) / 2).
            big = next(o for s in range(30) for o in [self.incoming(name, rank, s, {**crit, 'damage': 20000})]
                       if o['procs'][0]['events'][0]['type'] == 'damage' and not o['procs'][0]['events'][0]['crit'])
            self.assertEqual(big['procs'][0]['events'][0]['amount'],
                             min(share * big['defense']['damage'] // 100, self.char(name, rank).max_health // 2))
            # Neither a non-critical spell, a melee crit nor a sitting Paladin.
            self.assertFalse(self.incoming(name, rank, 1, {**crit, 'crit': False}).get('procs'))
            melee_crit = next(o for s in range(300) for o in [self.incoming(name, rank, s, {'damage': 100})]
                              if o['defense']['outcome'] == 'crit')
            self.assertFalse([p for p in melee_crit.get('procs', []) if p['source_spell'] == sid])
        self.assertFalse(self.incoming(name, 0, 1, crit).get('procs'))
        # Native: the share stays in the hit's own Skyrim units; the half-health cap is the plugin's to apply.
        native = next(o for s in range(30) for o in [self.incoming(name, 2, s, {**crit, 'damage': 100}, skyrim=NATIVE)]
                      if o['procs'][0]['events'][0]['type'] == 'damage' and not o['procs'][0]['events'][0]['crit'])
        proc = native['procs'][0]
        self.assertEqual((proc['events'][0]['amount'], proc['max_health_fraction']), (round(30 * native['defense']['damage'] / 100), 0.5))

    def test_improved_retribution_aura(self):
        self.percent_of('Improved Retribution Aura', 10301)

    def test_two_handed_weapon_specialization(self):
        name = 'Two-Handed Weapon Specialization'
        for items, fits in (([self.two_hander], True), ([self.sword], False), ([], False)):
            base = self.char(name, 0, items=items)
            for rank in self.each(name):
                ch = self.char(name, rank, items=items)
                factor = 1 + 0.02 * rank if fits else 1.0
                self.assertAlmostEqual(ch.multiplier(79, 1), base.multiplier(79, 1) * factor)
                low, high, _ = melee.roll_range(ch, ch.melee_weapon())
                b_low, b_high, _ = melee.roll_range(base, base.melee_weapon())
                self.assertAlmostEqual((low, high), (b_low * factor, b_high * factor))

    def test_sanctity_aura(self):
        self.gated('Sanctity Aura', (20218,))
        base = self.tooltip('Sanctity Aura', 1, 10314, target=UNDEAD)
        holy = self.tooltip('Sanctity Aura', 1, 10314, target=UNDEAD, auras=[20218])
        self.assertEqual(holy, [int(v * 1.1) for v in base])   # SPELL_AURA_MOD_DAMAGE_PERCENT_DONE, Holy (misc 2)

    def test_vengeance(self):
        name = 'Vengeance'
        for rank in self.each(name):
            buff = self.cat.spell(self.rank_spell(name, rank))['EffectTriggerSpell1']

            def mine(out):
                return [p for p in out.get('procs', []) if p['source_spell'] == self.rank_spell(name, rank)]
            caster = self.caster(name, rank, items=[self.two_hander])
            swings = [self.ok({'op': 'melee', 'seed': s, 'caster': caster, 'target': SWING}) for s in range(120)]
            for out in swings:
                self.assertEqual(bool(mine(out)), out['outcome'] == 'crit')
            proc, = mine(next(o for o in swings if o['outcome'] == 'crit'))
            self.assertEqual((proc['spell'], proc['target']), (buff, 'caster'))
            self.assertEqual([(a['type'], a['school_mask'], a['amount'], a['duration_ms']) for a in proc['auras']],
                             [('damage_done_percent', 1, 3 * rank, 8000), ('damage_done_percent', 2, 3 * rank, 8000)])
            # A spell's crit (Exorcism) and a judgement's crit raise it; a critical heal does not.
            exorcisms = [self.cast(name, rank, 10314, UNDEAD, s) for s in range(150)]
            self.assertTrue(all(bool(mine(o)) == o['events'][0].get('crit', False) for o in exorcisms))
            judged = [self.cast(name, rank, 20271, HOSTILE, s, auras=[20293]) for s in range(150)]
            self.assertTrue(all(bool(mine(o)) == o['procs'][0]['events'][0].get('crit', False) for o in judged))
            self.assertTrue(any(mine(o) for o in judged))
            heals = [self.cast(name, rank, 25292, FRIEND, s) for s in range(150)]
            self.assertTrue(any(o['events'][0]['crit'] for o in heals) and not any(mine(o) for o in heals))
        # Seal of Command's strike is a triggered spell: Vengeance (SPELL_ATTR_EX3_CAN_PROC_FROM_PROCS) still procs.
        self.assertTrue(self.cat.spell(20059)['AttributesEx3'] & PT.ATTR_EX3_CAN_PROC_FROM_PROCS)
        caster = self.caster(name, 5, ('Seal of Command', 1), items=[self.two_hander], auras=[20920])
        seal_crits = 0
        for seed in range(600):
            out = self.ok({'op': 'melee', 'seed': seed, 'caster': caster, 'target': SWING})
            strike = [p for p in out.get('procs', []) if p['spell'] == 20424]
            from_strike = [p for p in out.get('procs', []) if p.get('from_spell') == 20424 and p['spell'] == 20055]
            crit = bool(strike) and strike[0]['events'][0].get('crit', False)
            self.assertEqual(bool(from_strike), crit)
            seal_crits += crit
        self.assertTrue(seal_crits)

    def test_repentance(self):
        humanoid = {**HOSTILE, 'creature_type': 'Humanoid'}
        self.gated('Repentance', (20066,), humanoid)
        # Its magic hit roll may miss; a landed one is a 6-second stun (SPELL_AURA_MOD_STUN) that damage ends
        # (AuraInterruptFlags 0x2), one target at a time (MECHANIC_KNOCKOUT with family flags: IsSingleTargetSpell).
        out = next(o for s in range(20) for o in [self.cast('Repentance', 1, 20066, humanoid, s)] if o['auras'])
        stun, = out['auras']
        self.assertEqual((stun['type'], stun['duration_ms'], stun['breaks_on_damage'], out['cooldown_ms']),
                         ('stun', 6000, True, 60000))
        self.assertEqual(stun['single_target']['chain'], 20066)
        self.assertEqual(self.refused('Repentance', 1, 20066, UNDEAD), 'SPELL_FAILED_BAD_TARGETS')

    # --- the talents op, the book and the Druid --------------------------------------------------------------------

    def test_talents_op_offers_the_three_trees_and_learns_one_rank_at_a_time(self):
        snap = self.ok({'op': 'talents', 'class': 'paladin', 'caster': {'level': 60}})
        self.assertEqual([(t['name'], len(t['talents'])) for t in snap['trees']],
                         [('Holy', 14), ('Protection', 15), ('Retribution', 15)])
        self.assertEqual(snap['points_available'], 51)
        rows = {t['id']: t for tree in snap['trees'] for t in tree['talents']}
        self.assertEqual({(rows[tid]['name'], rows[p['talent']]['name'], p['rank'])
                          for tid in rows for p in rows[tid]['prerequisites']},
                         {('Divine Favor', 'Illumination', 5), ('Holy Shock', 'Divine Favor', 1),
                          ('Shield Specialization', 'Redoubt', 5), ('Holy Shield', 'Blessing of Sanctuary', 1),
                          ('Vengeance', 'Conviction', 5)})
        self.assertIn('tier_points', rows[self.ids['Holy Shock']]['reasons'])
        self.assertIn('prerequisite', rows[self.ids['Holy Shock']]['reasons'])
        learned = self.ok({'op': 'talents', 'class': 'paladin', 'caster': {'level': 10},
                           'learn': {'talent': self.ids['Divine Strength'], 'rank': 1}})
        self.assertEqual(learned['learned'], {'talent': self.ids['Divine Strength'], 'rank': 1, 'spell': 20262})
        bad = self.ask({'op': 'talents', 'class': 'paladin', 'caster': {'level': 60}, 'learn': {'talent': self.ids['Holy Shock']}})
        self.assertIn('tier_points', bad['error'])
        for top in ('Holy Shock', 'Holy Shield', 'Repentance'):
            ranks = self.ranks(top, 1)
            snap = self.ok({'op': 'talents', 'class': 'paladin', 'caster': {'level': 60, 'talents': ranks}})
            self.assertEqual(snap['points_spent'], sum(ranks.values()))
            self.assertIn(self.rank_spell(top, 1), snap['learned_spells'])

    def test_a_catalog_without_the_code_linked_spells(self):
        # build/catalog.json may predate the manifest: Holy Shock's spells, 20272, 20233/20236, 67..., 25997 missing.
        linked = {to for rows in paladin.links().values() for tos in rows.values() for to in tos} - set(self.cat.ranks)
        raw = {**self.cat.raw, 'spells': {k: v for k, v in self.cat.raw['spells'].items() if int(k) not in linked}}
        svc = Service(Catalog(raw=raw), None)
        ask = lambda req: svc.respond(json.dumps(req).encode())
        caster = self.caster('Holy Shock', 1)
        self.assertEqual(ask({'op': 'resolve', 'spell': 20473, 'caster': caster, 'target': FRIEND})['error'],
                         'SPELL_FAILED_SPELL_UNAVAILABLE')
        out = ask({'op': 'resolve', 'spell': 10310, 'caster': self.caster('Improved Lay on Hands', 2), 'target': FRIEND})
        self.assertTrue(out['ok'] and 'procs' not in out, out)
        swing = self.caster('Vindication', 3, items=[self.two_hander])
        self.assertTrue(all(ask({'op': 'melee', 'seed': s, 'caster': swing, 'target': SWING})['ok'] for s in range(30)))
        crit = {'damage': 100, 'kind': 'spell', 'school': 2, 'crit': True}
        out = ask({'op': 'incoming_hit', 'seed': 1, 'caster': self.caster('Eye for an Eye', 2), 'target': HOSTILE, 'attack': crit})
        self.assertTrue(out['ok'] and not out.get('procs'), out)

    def test_druid_requests_never_reach_the_paladin_talents(self):
        druid = {'level': 60, 'talents': {'762': 5}, 'items': [873]}
        out = self.ok({'op': 'stats', 'caster': druid})
        self.assertNotIn('paladin', out)
        ch = self.svc.char(druid)
        self.assertFalse(any('custom_rule' in a for a in ch.auras))
        self.assertIsNone(PT.linked_tooltips(self.cat, ch, self.cat.spell(9912)))
        self.assertEqual(PT.finish(self.cat, ch, {'op': 'resolve', 'spell': 9912, 'caster': druid}, {'events': []}),
                         {'events': []})


if __name__ == '__main__':
    unittest.main()
