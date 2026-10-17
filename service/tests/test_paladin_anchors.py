"""The Paladin's remaining book spells have explicit Skyrim anchors (skyrim_balance PALADIN): WoW's relative prices
hold, and Redemption brings a friend back with health and magicka in Skyrim units."""
import unittest

from service.tests import test_paladin_talents as T   # its helpers, not its tests

SKYRIM = {'level': 40, 'skills': {'Restoration': 50, 'Alteration': 50, 'Conjuration': 50}}
SELF = {'kind': 'self'}


class PaladinAnchors(unittest.TestCase):
    setUpClass = classmethod(T.PaladinTalents.setUpClass.__func__)
    setUp, ranks, ask, ok = T.PaladinTalents.setUp, T.PaladinTalents.ranks, T.PaladinTalents.ask, T.PaladinTalents.ok

    def resolve(self, spell, target=SELF, talents=(), **extra):
        talents = self.ranks(*talents[0], *talents[1:]) if talents else {}
        return self.ok({'op': 'resolve', 'spell': spell, 'seed': 1, 'target': target,
                        'caster': {'class': 'paladin', 'race': 1, 'level': 60, 'talents': talents, 'skyrim': SKYRIM, **extra}})

    def test_prices_follow_wow_mana_at_their_counterparts_rates(self):
        flash = self.resolve(19943)['cost']
        shock = self.resolve(20930, talents=(('Holy Shock', 1),))['cost']
        # 325 mana against Flash of Light's 140, both at Healing Touch's magicka per WoW mana.
        self.assertAlmostEqual(shock / flash, 325 / 140, delta=0.35)
        self.assertGreater(self.resolve(20216, talents=(('Divine Favor', 1),))['cost'], 0)
        cleanse, purify = self.resolve(4987)['cost'], self.resolve(1152)['cost']
        self.assertEqual(cleanse, purify)
        self.assertGreater(cleanse, 0)
        self.assertGreater(self.resolve(20066, {'kind': 'unit', 'reaction': 'hostile', 'level': 60, 'distance_yd': 5,
                                                'creature_type': 'Humanoid'}, talents=(('Repentance', 1),))['cost'], 0)

    def test_redemption_waits_for_peace(self):
        # Spell::CheckCast (Spell.cpp:4706): SPELL_ATTR_NOT_IN_COMBAT_ONLY_PEACEFUL.
        out = self.ask({'op': 'resolve', 'spell': 20773, 'seed': 1, 'caster': {'class': 'paladin', 'race': 1, 'level': 60,
                        'in_combat': True}, 'target': {'kind': 'unit', 'reaction': 'friendly', 'level': 60, 'dead': True,
                                                       'distance_yd': 5}})
        self.assertEqual(out['error'], 'SPELL_FAILED_AFFECTING_COMBAT')

    def test_divine_intervention(self):
        caster = {'class': 'paladin', 'race': 1, 'level': 60, 'reagents': {'17033': 1}}
        follower = {'kind': 'unit', 'reaction': 'friendly', 'level': 60, 'distance_yd': 5, 'party': True}
        for target in ({'kind': 'self'}, {**follower, 'party': False}):   # never herself; a party member only
            out = self.ask({'op': 'resolve', 'spell': 19752, 'seed': 1, 'caster': caster, 'target': target})
            self.assertEqual(out['error'], 'SPELL_FAILED_BAD_TARGETS')
        out = self.ok({'op': 'resolve', 'spell': 19752, 'seed': 1, 'caster': caster, 'target': follower})
        # 19753 on the follower for three minutes (immune to every school, stunned, unattackable), Sanctuary on it, and
        # the caster dies; the Symbol of Divinity is spent.
        self.assertEqual([(a['spell'], a['type'], a['duration_ms']) for a in out['auras']],
                         [(19753, 'school_immunity', 180000), (19753, 'stun', 180000), (19753, 'unattackable', 180000)])
        self.assertEqual(out['auras'][0]['school_mask'], 127)
        self.assertEqual(out['events'], [{'type': 'sanctuary', 'target': 'target'}, {'type': 'instakill', 'target': 'caster'}])
        self.assertEqual(out['reagents'], [{'item': 17033, 'count': 1}])

    def test_judged_hit_gives_the_striking_follower_its_payload(self):
        # Paladin.cpp:36-75: whoever strikes a unit under Judgement of Light / Wisdom casts the payload on itself.
        def ask(judgement, kind='melee', damage=30, seed=0):
            return self.ok({'op': 'judged_hit', 'seed': seed, 'caster': {'class': 'paladin', 'race': 1, 'level': 60},
                            'attacker': {'level': 40}, 'target': {'kind': 'unit', 'judgements': [judgement]},
                            'attack': {'kind': kind, 'damage': damage}})
        light = [p for seed in range(60) for p in ask(20346, seed=seed)['procs']]
        self.assertTrue(light and all(p['target'] == 'attacker' and p['events'][0]['type'] == 'heal' for p in light))
        wisdom = [p for seed in range(60) for p in ask(20354, seed=seed)['procs']]
        self.assertTrue(wisdom and all(p['events'][0]['type'] == 'power' for p in wisdom))
        # Light's ProcFlags hold melee only (0x28); a spell striking the unit raises none; a miss (no damage) none.
        self.assertFalse([p for seed in range(60) for p in ask(20346, 'spell', seed=seed)['procs']])
        self.assertFalse(ask(20346, damage=0)['procs'])

    def test_redemption_revives_with_health_and_magicka(self):
        out = self.resolve(20773, {'kind': 'unit', 'reaction': 'friendly', 'level': 60, 'dead': True, 'distance_yd': 5})
        revive = next(e for e in out['events'] if e['type'] in ('resurrect', 'revive'))
        self.assertGreater(revive.get('health', 0), 0)
        self.assertGreater(revive.get('mana', 0), 0)


if __name__ == '__main__':
    unittest.main()
