"""Reviewable Skyrim balance data. See docs/skyrim-balance.md for units and derivations.

Counterparts are a pinned extract of build/skyrim_inventory.json, with player spell
records (including final DLC overrides), not similarly named NPC/test spells.
Change SPELLS here to tune costs, health budgets or native aura magnitudes.
"""
from dataclasses import dataclass, field, replace

# Skyrim.esm GMST values; PC multiplier is the engine default (no GMST override).
SKILL_COST_BASE = 0.0025
SKILL_COST_SCALE = 0.65
SKILL_COST_MULT = 1.0

COUNTERPARTS = {
    'firebolt': {'form': 'Skyrim.esm:012FD0', 'name': 'Firebolt', 'source': 'Skyrim.esm', 'cost': 41, 'school': 'Destruction', 'minimum_skill': 25, 'magnitude': 25.0, 'duration': 0, 'area': 0, 'effect': 'Skyrim.esm:012F03'},
    'fireball': {'form': 'Skyrim.esm:01C789', 'name': 'Fireball', 'source': 'Dawnguard.esm', 'cost': 133, 'school': 'Destruction', 'minimum_skill': 50, 'magnitude': 40.0, 'duration': 0, 'area': 15, 'effect': 'Skyrim.esm:01CEA1'},
    'blizzard': {'form': 'Skyrim.esm:07E8E4', 'name': 'Blizzard', 'source': 'Skyrim.esm', 'cost': 1106, 'school': 'Destruction', 'minimum_skill': 100, 'magnitude': 20.0, 'duration': 10, 'area': 40, 'effect': 'Skyrim.esm:0B79FE'},
    'healing': {'form': 'Skyrim.esm:012FCC', 'name': 'Healing', 'source': 'Skyrim.esm', 'cost': 12, 'school': 'Restoration', 'minimum_skill': 0, 'magnitude': 10.0, 'duration': 1, 'area': 0, 'effect': 'Skyrim.esm:01CEA4'},
    'fast_healing': {'form': 'Skyrim.esm:02F3B8', 'name': 'Fast Healing', 'source': 'Skyrim.esm', 'cost': 73, 'school': 'Restoration', 'minimum_skill': 25, 'magnitude': 50.0, 'duration': 0, 'area': 0, 'effect': 'Skyrim.esm:01CEA6'},
    'close_wounds': {'form': 'Skyrim.esm:0B62EF', 'name': 'Close Wounds', 'source': 'Skyrim.esm', 'cost': 126, 'school': 'Restoration', 'minimum_skill': 50, 'magnitude': 100.0, 'duration': 0, 'area': 0, 'effect': 'Skyrim.esm:0B62F0'},
    'grand_healing': {'form': 'Skyrim.esm:0B62EE', 'name': 'Grand Healing', 'source': 'Skyrim.esm', 'cost': 254, 'school': 'Restoration', 'minimum_skill': 75, 'magnitude': 200.0, 'duration': 0, 'area': 15, 'effect': 'Skyrim.esm:0B62ED'},
    'frostbite': {'form': 'Skyrim.esm:02B96B', 'name': 'Frostbite', 'source': 'Skyrim.esm', 'cost': 16, 'school': 'Destruction', 'minimum_skill': 0, 'magnitude': 8.0, 'duration': 1, 'area': 0, 'effect': 'Skyrim.esm:013CAA'},
    'oakflesh': {'form': 'Skyrim.esm:05AD5C', 'name': 'Oakflesh', 'source': 'Skyrim.esm', 'cost': 103, 'school': 'Alteration', 'minimum_skill': 0, 'magnitude': 40.0, 'duration': 60, 'area': 0, 'effect': 'Skyrim.esm:051B15'},
    'stoneflesh': {'form': 'Skyrim.esm:05AD5D', 'name': 'Stoneflesh', 'source': 'Skyrim.esm', 'cost': 194, 'school': 'Alteration', 'minimum_skill': 25, 'magnitude': 60.0, 'duration': 60, 'area': 0, 'effect': 'Skyrim.esm:059B7A'},
    'calm': {'form': 'Skyrim.esm:04DEE9', 'name': 'Calm', 'source': 'Dawnguard.esm', 'cost': 146, 'school': 'Illusion', 'minimum_skill': 25, 'magnitude': 9.0, 'duration': 30, 'area': 0, 'effect': 'Skyrim.esm:04DEE7'},
    'dead_thrall': {'form': 'Skyrim.esm:07E8DF', 'name': 'Dead Thrall', 'source': 'Skyrim.esm', 'cost': 1000, 'school': 'Conjuration', 'minimum_skill': 100, 'magnitude': 40.0, 'duration': 86313600, 'area': 0, 'effect': 'Skyrim.esm:07E8E0'},
    'waterbreathing': {'form': 'Skyrim.esm:05D175', 'name': 'Waterbreathing', 'source': 'Skyrim.esm', 'cost': 222, 'school': 'Alteration', 'minimum_skill': 50, 'magnitude': 0.0, 'duration': 60, 'area': 0, 'effect': 'Skyrim.esm:01EA73'},
    'courage': {'form': 'Skyrim.esm:04DEE8', 'name': 'Courage', 'source': 'Skyrim.esm', 'cost': 39, 'school': 'Illusion', 'minimum_skill': 0, 'magnitude': 100.0, 'duration': 60, 'area': 0, 'effect': 'Skyrim.esm:01EA79'},
    'ash_shell': {'form': 'Dragonborn.esm:017731', 'name': 'Ash Shell', 'source': 'Dragonborn.esm', 'cost': 251, 'school': 'Alteration', 'minimum_skill': 50, 'magnitude': 0.0, 'duration': 30, 'area': 0, 'effect': 'Dragonborn.esm:017732'},
    'flame_cloak': {'form': 'Skyrim.esm:03AE9F', 'name': 'Flame Cloak', 'source': 'ccQDRSSE001-SurvivalMode.esl', 'cost': 289, 'school': 'Destruction', 'minimum_skill': 50, 'magnitude': 10.0, 'duration': 60, 'area': 0, 'effect': 'Skyrim.esm:03AE9E'},
    'fortify_health': {'form': 'Skyrim.esm:03EAF2', 'name': 'Potion of Health', 'source': 'Skyrim.esm', 'cost': 0, 'school': 'None', 'minimum_skill': 0, 'magnitude': 20.0, 'duration': 60, 'area': 0, 'effect': 'Skyrim.esm:03EAF3'},
    'fortify_magicka': {'form': 'Skyrim.esm:03EAF7', 'name': 'Potion of Extra Magicka', 'source': 'Skyrim.esm', 'cost': 0, 'school': 'None', 'minimum_skill': 0, 'magnitude': 20.0, 'duration': 60, 'area': 0, 'effect': 'Skyrim.esm:03EAF8'},
    'fortify_stamina': {'form': 'Skyrim.esm:03EAFD', 'name': 'Potion of Enhanced Stamina', 'source': 'Skyrim.esm', 'cost': 0, 'school': 'None', 'minimum_skill': 0, 'magnitude': 20.0, 'duration': 300, 'area': 0, 'effect': 'Skyrim.esm:03EAF9'},
    'restore_magicka': {'form': 'Skyrim.esm:03EAE2', 'name': 'Potion of Plentiful Magicka', 'source': 'Skyrim.esm', 'cost': 0, 'school': 'None', 'minimum_skill': 0, 'magnitude': 75.0, 'duration': 0, 'area': 0, 'effect': 'Skyrim.esm:03EB17'},
    'magicka_regen': {'form': 'Skyrim.esm:03EB0D', 'name': 'Potion of Lasting Potency', 'source': 'Skyrim.esm', 'cost': 0, 'school': 'None', 'minimum_skill': 0, 'magnitude': 50.0, 'duration': 300, 'area': 0, 'effect': 'Skyrim.esm:03EB07'},
    'cure_poison': {'form': 'Skyrim.esm:065A64', 'name': 'Cure Poison', 'source': 'Skyrim.esm', 'cost': 0, 'school': 'None', 'minimum_skill': 0, 'magnitude': 100.0, 'duration': 0, 'area': 0, 'effect': 'Skyrim.esm:109ADD'},
    # WEAP DATA damage, read directly from Skyrim.esm (not a magic inventory row).
    'iron_sword': {'form': 'Skyrim.esm:012EB7', 'name': 'Iron Sword', 'source': 'Skyrim.esm',
                   'cost': 0, 'magnitude': 7, 'duration': 0, 'minimum_skill': 0, 'school': 'OneHanded'},
}


@dataclass(frozen=True)
class Rule:
    name: str
    skill: str
    cost: tuple = ()  # (counterpart, cast count / concentration seconds)
    health: tuple = ()  # (counterpart, magnitudes); one budget shared by direct + periodic
    wow_cost: float = 1  # unmodified level-60 cost, to retain WoW cost modifiers
    wow_health: float = 1  # level-60 naked maximum roll, before crit / mitigation
    native: dict = field(default_factory=dict)  # aura kind -> (MGEF, target, WoW reference)
    native_from: int | None = None
    mana: tuple = ()  # resurrection mana resource budget
    reason: str = ''
    minimum_health: bool = False  # positive low-rank outcomes must survive integer rounding
    rank_health_floor: float = 0  # fraction of top native power at zero WoW rank power
    rank_cost_floor: float = 0  # corresponding cost floor; do not make downranking free

    @property
    def base_cost(self):
        return sum(COUNTERPARTS[name]['cost'] * units for name, units in self.cost)

    @property
    def health_factor(self):
        return sum(COUNTERPARTS[name]['magnitude'] * units for name, units in self.health) / self.wow_health


# All slice entries, including resource-free feral utilities. Timing, targets and
# restrictions are supplied by CMaNGOS, never by this table.
SPELLS = {
    9912: Rule('Wrath', 'Destruction', (('firebolt', 1),), (('firebolt', 1.4),), 180, 276,
               rank_health_floor=.25, rank_cost_floor=.25,
               reason='35-health projectile budget; slower WoW casts and no native fire afterburn.'),
    9835: Rule('Moonfire', 'Destruction', (('firebolt', 2),), (('firebolt', 2.8),), 375, 611,
               rank_health_floor=.25, rank_cost_floor=.25,
               reason='70 health split between impact and four ticks; two Firebolt casts in cost.'),
    25298: Rule('Starfire', 'Destruction', (('fireball', 1),), (('fireball', 1.875),), 340, 584,
                rank_health_floor=.4, rank_cost_floor=.25,
                reason='75-health single-target heavy hit; retains its 3.5-second WoW cast at Fireball cost.'),
    17402: Rule('Hurricane', 'Destruction', (('blizzard', 1),), (('blizzard', 12),), 1495, 1340,
                rank_health_floor=.25, rank_cost_floor=.25,
                reason='Master area storm: 24 damage/second per target for ten seconds at Blizzard cost.'),
    9853: Rule('Entangling Roots', 'Alteration', (('ash_shell', .9), ('frostbite', 3)),
               (('frostbite', 3),), 125, 270,
               reason='27/30 of an adept immobilization plus three seconds of novice damage, spread over nine ticks. Roots can break; Ash Shell also prevents damage.'),
    25299: Rule('Rejuvenation', 'Restoration', (('fast_healing', 1),), (('fast_healing', 1.5),), 360, 888,
                rank_health_floor=.15, rank_cost_floor=.1,
                reason='75 health spread over four ticks at Fast Healing cost; delayed recovery stays below Regrowth.'),
    9858: Rule('Regrowth', 'Restoration', (('fast_healing', 1), ('healing', 2.5)),
               (('fast_healing', 1.6), ('healing', 3)), 880, 2183,
               rank_health_floor=.15, rank_cost_floor=.1,
               reason='110 health split between direct healing and seven ticks; total between Rejuvenation and Healing Touch.'),
    25297: Rule('Healing Touch', 'Restoration', (('close_wounds', 1),), (('close_wounds', 1.5),), 800, 2677,
                rank_health_floor=.1, rank_cost_floor=.1,
                reason='150-health single heal at Close Wounds cost; retains its long WoW cast.'),
    9863: Rule('Tranquility', 'Restoration', (('grand_healing', 1),), (('grand_healing', 1.25),), 925, 1470,
               rank_health_floor=.1, rank_cost_floor=.1,
               reason='Expert party heal: 250 health per member over five ticks at Grand Healing cost.'),
    20748: Rule('Rebirth', 'Restoration', (('dead_thrall', 1),), (('close_wounds', 1),), 1057, 2200,
                mana=(('restore_magicka', 1, 2800),),
                reason='Permanent resurrection is master reanimation in cost, with 100 health and 75 magicka; cooldown and reagent remain.'),
    18658: Rule('Hibernate', 'Alteration', (('calm', 40/30),), wow_cost=150,
                reason='Forty seconds of apprentice pacification, charged as 40/30 of Calm; sleep still breaks on damage.'),
    9901: Rule('Soothe Animal', 'Alteration', (('calm', .25),), wow_cost=100,
               reason='Detection suppression is weaker than pacification: quarter of Calm, for the original 15 seconds.'),
    9907: Rule('Faerie Fire', 'Alteration', (('oakflesh', 1),), wow_cost=115,
               native={'armor': ((0x10319D, 40, 505),)},
               reason='Inverse novice flesh armor: reduce armor by 40; retain reveal immunities and duration.'),
    22812: Rule('Barkskin', 'Alteration', (('oakflesh', .25),), wow_cost=186,
                reason='Quarter of novice flesh cost for 15 seconds of casting protection. The core grants pushback/cast modifiers, not armor; do not invent armor.'),
    29166: Rule('Innervate', 'Restoration', (('fast_healing', 1),), wow_cost=62,
                native={'mana_regen_percent': ((0x7A0FD, 50, 400),)},
                reason='Apprentice resource recovery cost; regeneration magnitude from the 50% Skyrim magicka-regeneration potion, with WoW 20-second duration.'),
    9885: Rule('Mark of the Wild', 'Alteration', (('oakflesh', 1),), wow_cost=445,
               native={'armor': ((0xE5F55, 40, 285),),
                       'stat': ((0x493AA, 20, 12), (0x49504, 20, 12), (0x49507, 20, 12)),
                       'resistance': ((0x48C8B, 20/3, 20), (0x48F45, 20/3, 20),
                                      (0x49295, 20/3, 20), (0xB7A35, 40/3, 20))},
               reason='Novice flesh armor plus small fortify-resource potion magnitudes; retain gear-table resistance equivalents and the long WoW duration.'),
    21850: Rule('Gift of the Wild', 'Alteration', (('oakflesh', 1200/445),), wow_cost=1200,
                native_from=9885, reason='Same magnitudes as Mark; party cast costs 1200/445 as much, preserving the WoW relative cost and 60-minute duration.'),
    9910: Rule('Thorns', 'Destruction', (('flame_cloak', 1),), (('frostbite', 1),), 320, 18,
               reason='Adept reactive cloak: 8 damage per incoming melee hit rather than continuous cloak damage; retain ten-minute duration.'),
    2782: Rule('Remove Curse', 'Restoration', (('fast_healing', 1),), wow_cost=124,
               reason='Apprentice curative utility cost; no vanilla learnable curse-removal spell.'),
    8946: Rule('Cure Poison', 'Restoration', (('healing', 1),), wow_cost=199,
               reason='Cure Poison potion supplies the role (no native mana cost); one novice Healing second prices one removal.'),
    2893: Rule('Abolish Poison', 'Restoration', (('healing', 5),), wow_cost=199,
               reason='Five novice curative actions: immediate removal and four WoW periodic removals.'),
    5487: Rule('Bear Form', 'Alteration', (('oakflesh', 1),), (('fortify_health', 1),), 684, 560,
               reason='Novice defensive transformation; fixed bonus health becomes 20.'),
    9634: Rule('Dire Bear Form', 'Alteration', (('stoneflesh', 1),), (('fortify_health', 2),), 684, 1240,
               reason='Apprentice defensive transformation; fixed bonus health becomes 40.'),
    768: Rule('Cat Form', 'Alteration', (('oakflesh', 1),), wow_cost=684,
              reason='Novice transformation; no added health.'),
    783: Rule('Travel Form', 'Alteration', (('courage', 1),), wow_cost=248,
              reason='Cheap utility transformation priced as Courage; speed remains a WoW percentage.'),
    1066: Rule('Aquatic Form', 'Alteration', (('waterbreathing', 1),), wow_cost=248,
               reason='Adept water-breathing utility; indefinite WoW form until cancelled.'),
    # Feral health uses one Iron Sword / naked level-60 Cat white-hit budget
    # (weapon 51..75 + 469 AP / 14 at 1 second = 96.5 average, before armor).
    # Shared scale retains ability multipliers, combo/extra-energy/AP differences.
    9881: Rule('Maul', 'OneHanded', health=(('iron_sword', 1),), wow_health=96.5),
    9908: Rule('Swipe', 'OneHanded', health=(('iron_sword', 1),), wow_health=96.5),
    9850: Rule('Claw', 'OneHanded', health=(('iron_sword', 1),), wow_health=96.5),
    9830: Rule('Shred', 'OneHanded', health=(('iron_sword', 1),), wow_health=96.5),
    9904: Rule('Rake', 'OneHanded', health=(('iron_sword', 1),), wow_health=96.5),
    9896: Rule('Rip', 'OneHanded', health=(('iron_sword', 1),), wow_health=96.5),
    31018: Rule('Ferocious Bite', 'OneHanded', health=(('iron_sword', 1),), wow_health=96.5),
    9827: Rule('Pounce', 'OneHanded', health=(('iron_sword', 1),), wow_health=96.5),
    9867: Rule('Ravage', 'OneHanded', health=(('iron_sword', 1),), wow_health=96.5),
    9846: Rule("Tiger's Fury", 'OneHanded', health=(('iron_sword', 1),), wow_health=96.5),
    22896: Rule('Frenzied Regeneration', 'OneHanded', health=(('healing', 1),), wow_health=200,
                reason='Ten rage gives one second of Healing (10 health); keep rage limits and each tick’s crit.'),
    6795: Rule('Growl', 'OneHanded'),
    9898: Rule('Demoralizing Roar', 'OneHanded'),
    8983: Rule('Bash', 'OneHanded'),
    5229: Rule('Enrage', 'OneHanded'),
    9913: Rule('Prowl', 'OneHanded'),
    9821: Rule('Dash', 'OneHanded'),
    9892: Rule('Cower', 'OneHanded'),
    17392: Rule('Faerie Fire (Feral)', 'OneHanded', native={'armor': ((0x10319D, 40, 505),)}),
}

# Ordinary attacks in an animal form share the ability weapon-health budget.
FERAL_MELEE = SPELLS[9850]


def rank_rules(cat):
    """Extend the pinned maximum-rank budgets to the catalog's class chains.

    Primary caster chains interpolate between a novice native budget and their
    pinned top-rank budget, with a corresponding cost curve. Their own naked
    level-60 references retain WoW rolls, bonuses and cost modifiers. Other chains
    share the maximum-rank factor. Explicit rules (Bear versus Dire Bear included)
    always take precedence; low ranks never receive the full top-rank budget.
    """
    from service import formulas as F
    from service.skyrim import SKILLS
    rules = dict(SPELLS)
    anchors = {cat.chain(sid): rule for sid, rule in SPELLS.items()}
    groups = {}
    for sid, row in cat.ranks.items():
        groups.setdefault(row['chain'], []).append(sid)
    skill_names = {key: name for name, key in SKILLS.items()}
    for chain, ids in sorted(groups.items()):
        rule = anchors.get(chain)
        if rule is None:
            # Class lines absent from the original slice use the same native
            # roles: projectile/healing/flesh utility. Passive/free abilities
            # preserve zero cost; resource abilities keep WoW resource units.
            top = max(ids, key=lambda sid: (cat.ranks[sid]['rank'], sid))
            s = cat.spell(top)
            skill = skill_names[cat.ranks[top]['skill']]
            health = 0
            for e in (1, 2, 3):
                value = F.effect_value(s, e, 60, None, maximum=True)
                if s[f'Effect{e}'] in (2, 10):
                    health += value
                elif s[f'EffectApplyAuraName{e}'] in (3, 8, 15):
                    amp = s[f'EffectAmplitude{e}']
                    health += value * (cat.duration(s, 60) // amp if amp else 1)
            counterpart = 'fast_healing' if skill == 'Restoration' else 'firebolt' if health else 'oakflesh'
            cost = s['ManaCost'] + s['ManaCostPercentage'] * cat.raw['character']['classlevelstats']['basemana'] // 100
            cost += s['ManaCostPerlevel'] * (F.spell_rank_level(s, 60) - s['BaseLevel'])
            rule = Rule(s['SpellName'], skill,
                        cost=((counterpart, 1),) if cost > 0 and s['PowerType'] == 0 else (),
                        health=((counterpart, 1),) if health > 0 else (),
                        wow_cost=max(1, cost), wow_health=max(1, health),
                        reason='Class-chain role fallback; preserve free/passive and non-mana costs.')
        for sid in ids:
            if sid in rules:
                continue
            s = cat.spell(sid)
            rank = replace(rule, name=s['SpellName'], minimum_health=True)
            if rule.rank_health_floor or rule.rank_cost_floor:
                # Normalize the finished CMaNGOS outcome to this rank's actual
                # integer baseline, then assign a bounded native rank budget.
                health = 0
                for e in (1, 2, 3):
                    value = int(F.effect_value(s, e, 60, None, maximum=True))
                    if s[f'Effect{e}'] in (2, 10):
                        health += value
                    elif s[f'EffectApplyAuraName{e}'] in (3, 8):
                        amp = s[f'EffectAmplitude{e}']
                        health += value * (cat.duration(s, 60) // amp if amp else 1)
                cost = s['ManaCost'] + s['ManaCostPercentage'] * cat.raw['character']['classlevelstats']['basemana'] // 100
                cost += s['ManaCostPerlevel'] * (F.spell_rank_level(s, 60) - s['BaseLevel'])
                health_share = rule.rank_health_floor + (1-rule.rank_health_floor) * health / rule.wow_health
                cost_share = rule.rank_cost_floor + (1-rule.rank_cost_floor) * cost / rule.wow_cost
                rank = replace(rank,
                               health=tuple((key, units*health_share) for key, units in rule.health),
                               cost=tuple((key, units*cost_share) for key, units in rule.cost),
                               wow_health=max(1, health), wow_cost=max(1, cost))
            rules[sid] = rank
    return rules
