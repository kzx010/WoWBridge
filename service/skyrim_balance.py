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
FERAL_SKILL_PER_POINT = .005
# A weapon's native damage at her skill, before perks (Actor::CalcWeaponDamage): fDamagePCSkillMin (engine default 1;
# Skyrim.esm has no override) up to Skyrim.esm's fDamagePCSkillMax 1.5 at skill 100, i.e. base x (1 + skill/200).
DAMAGE_PC_SKILL_MIN, DAMAGE_PC_SKILL_MAX = 1.0, 1.5
FERAL_BASE_FLOOR = .5
FORM_ARMOR_PERCENT = {'bear': 50, 'dire_bear': 100, 'moonkin': 100}

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
    # The Paladin's defence and control anchors (data/skyrim_magic.py inventory rows; the effect's MGEF school/skill).
    'paralyze': {'form': 'Skyrim.esm:05AD5F', 'name': 'Paralyze', 'source': 'Skyrim.esm', 'cost': 450, 'school': 'Alteration', 'minimum_skill': 75, 'magnitude': 0.0, 'duration': 10, 'area': 0, 'effect': 'Skyrim.esm:01EA6E'},
    'sun_fire': {'form': 'Dawnguard.esm:003F52', 'name': 'Sun Fire', 'source': 'Dawnguard.esm', 'cost': 24, 'school': 'Restoration', 'minimum_skill': 25, 'magnitude': 25.0, 'duration': 0, 'area': 0, 'effect': 'Dawnguard.esm:003F50'},
    'vampires_bane': {'form': 'Dawnguard.esm:0038B6', 'name': "Vampire's Bane", 'source': 'Dawnguard.esm', 'cost': 72, 'school': 'Restoration', 'minimum_skill': 50, 'magnitude': 40.0, 'duration': 0, 'area': 15, 'effect': 'Dawnguard.esm:00844B'},
    'turn_undead': {'form': 'Skyrim.esm:05DD5D', 'name': 'Turn Undead', 'source': 'Skyrim.esm', 'cost': 168, 'school': 'Restoration', 'minimum_skill': 50, 'magnitude': 13.0, 'duration': 30, 'area': 0, 'effect': 'Skyrim.esm:05DD5B'},
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
    bonus_health_factor: float | None = None  # shared top-rank conversion, without the novice floor
    rank_cost_by_health: bool = False  # percentage-mana resurrection ranks otherwise cost the same
    weapon_skill: bool = False  # trains and is discounted by her main hand's skill (skyrim.cast_skill): the seals

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
    25298: Rule('Starfire', 'Destruction', (('firebolt', 2),), (('fireball', 1.875),), 340, 584,
                rank_health_floor=.4, rank_cost_floor=.25,
                reason='75-health single-target heavy hit at two Firebolt costs, keeping efficiency near Wrath.'),
    17402: Rule('Hurricane', 'Destruction', (('blizzard', .5),), (('blizzard', 12),), 1495, 1340,
                rank_health_floor=.25, rank_cost_floor=.25,
                reason='24 damage/second for ten seconds at half Blizzard cost: stationary channel and cooldown remain.'),
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
                rank_health_floor=.25, rank_cost_floor=.25,
                reason='150-health single heal at Close Wounds cost; retains its long WoW cast.'),
    9863: Rule('Tranquility', 'Restoration', (('grand_healing', .5),), (('grand_healing', 1.25),), 925, 1470,
               rank_health_floor=.25, rank_cost_floor=.25,
               reason='250 health per member at half Grand Healing cost, compensating for channel and five-minute cooldown.'),
    20748: Rule('Rebirth', 'Restoration', (('dead_thrall', 1),), (('close_wounds', 1),), 1057, 2200,
                mana=(('restore_magicka', 1, 2800),),
                rank_cost_by_health=True,
                reason='Top rank retains master pricing; lower ranks pay their resurrection-health share, with a quarter-price floor. Reagent and cooldown remain.'),
    18658: Rule('Hibernate', 'Alteration', (('calm', 40/30),), wow_cost=150,
                reason='Forty seconds of apprentice pacification, charged as 40/30 of Calm; sleep still breaks on damage.'),
    9901: Rule('Soothe Animal', 'Alteration', (('calm', .25),), wow_cost=100,
               reason='Detection suppression is weaker than pacification: quarter of Calm, for the original 15 seconds.'),
    9907: Rule('Faerie Fire', 'Alteration', (('oakflesh', 1),), wow_cost=115,
               native={'armor': ((0x10319D, 40, 505),)},
               reason='Inverse novice flesh armor: reduce armor by 40; retain reveal immunities and duration.'),
    22812: Rule('Barkskin', 'Alteration', (('oakflesh', .25),), wow_cost=186,
                reason='Quarter of novice flesh cost for 15 seconds of casting protection. The core grants pushback/cast modifiers, not armor; do not invent armor.'),
    29166: Rule('Innervate', 'Restoration', (('healing', 1),), wow_cost=62,
                native={'mana_regen_percent': ((0x7A0FD, 200, 400),)},
                reason='One Healing second in cost and +200% native regeneration make the original 20-second regeneration worthwhile; no instant restore.'),
    9885: Rule('Mark of the Wild', 'Alteration', (('oakflesh', 1),), wow_cost=445,
               native={'armor': ((0xE5F55, 40, 285),),
                       # Strength and Spirit at gear's scales (skyrim_effects.STAT): carry weight, weapon damage, regen.
                       'stat': ((0x493AA, 20, 12), (0x49504, 20, 12), (0x49507, 20, 12), (0x7A0F4, 12, 12),
                                (0x7A0FF, 6, 12), (0x7A106, 6, 12), (0x7A0FD, 12, 12)),
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
    9634: Rule('Dire Bear Form', 'Alteration', (('stoneflesh', .5),), (('fortify_health', 2),), 684, 1240,
               reason='Half Stoneflesh cost permits combat re-entry; fixed bonus health remains 40.'),
    768: Rule('Cat Form', 'Alteration', (('oakflesh', .5),), wow_cost=684,
              reason='Half Oakflesh cost permits combat re-entry; no added health.'),
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

# The Paladin's defence and control chains, anchored at each top rank (rank_rules extends them to lower ranks).
# Skills are the client's trainable ones: Alteration for protection and paralysis (Ash Shell, Paralyze), Restoration
# for holy magic (Skyrim's sun and undead spells are Restoration). Health references are the naked level-60 maximum
# rolls (Holy Shield: one block's damage; Consecration: all eight ticks per target).
PALADIN_DEFENSE = {
    5573: Rule('Divine Protection', 'Alteration', (('ash_shell', 8/30),), wow_cost=35,
               reason="Eight seconds of Ash Shell's encasement (immune, unable to attack) at its per-second price; "
                      'Forbearance and the shared five-minute category remain.'),
    1020: Rule('Divine Shield', 'Alteration', (('ash_shell', 12/30),), wow_cost=110,
               reason='Twelve seconds of Ash Shell protection at its per-second price; the player may still attack at half speed.'),
    10278: Rule('Blessing of Protection', 'Alteration', (('ash_shell', 10/30),), wow_cost=105,
                reason="Ten seconds of a follower's physical immunity at Ash Shell's per-second price (7% of a Paladin's "
                       '1512 create mana); Forbearance and the five-minute category remain.'),
    10308: Rule('Hammer of Justice', 'Alteration', (('paralyze', 6/10),), wow_cost=100,
                reason="Six seconds of Paralyze's ten (expert hard control), as Roots and Hibernate price theirs; the "
                       'one-minute cooldown remains.'),
    10314: Rule('Exorcism', 'Restoration', (('sun_fire', 3),), (('sun_fire', 3),), 345, 563,
                rank_health_floor=.25, rank_cost_floor=.25,
                reason="Three Sun Fires (75 health at 72 magicka) against undead and demons, Starfire's scale; its "
                       '15-second cooldown and creature types remain.'),
    24239: Rule('Hammer of Wrath', 'Restoration', (('sun_fire', 3 * 425/345),), (('sun_fire', 3),), 425, 556,
                rank_health_floor=.25, rank_cost_floor=.25,
                reason="Exorcism's 75-health budget for a finisher below 20% health; cost by the WoW mana ratio to Exorcism."),
    10318: Rule('Holy Wrath', 'Restoration', (('vampires_bane', 2),), (('vampires_bane', 1.5),), 805, 576,
                rank_health_floor=.25, rank_cost_floor=.25,
                reason="60 health to each undead or demon in 20 yards at two Vampire's Bane casts; the 2-second cast and "
                       'one-minute cooldown remain.'),
    20924: Rule('Consecration', 'Restoration', (('vampires_bane', 2 * 565/805),), (('vampires_bane', 1),), 565, 384,
                rank_health_floor=.25, rank_cost_floor=.25,
                reason="Holy Wrath's per-point area budget (40 health per target over eight ticks); cost by the WoW mana "
                       'ratio to Holy Wrath.'),
    10326: Rule('Turn Undead', 'Restoration', (('turn_undead', 20/30),), wow_cost=75,
                reason="Twenty seconds of Skyrim's Turn Undead (thirty); damage can still end it, one target at a time."),
    25780: Rule('Righteous Fury', 'Restoration', (('courage', 1),), wow_cost=453,
                reason='A cheap 30-minute threat utility priced as Courage (30% of create mana in WoW).'),
    20928: Rule('Holy Shield', 'Restoration', (('sun_fire', 3 * 240/345),), (('sun_fire', 2/3),), 240, 130,
                reason="Each block's Holy damage at Exorcism's per-point scale (about 17 for 130); four charges and the "
                       '+30% block chance remain.'),
}


def like(name, skill, druid, wow_cost=0, wow_health=0, **kw):
    """A Paladin rule at the rates of the Druid spell it plays like: druid's Skyrim health per WoW point and magicka per
    WoW mana, for this spell's own WoW reference (naked level 60; percentages of a Paladin's 1512 create mana)."""
    return Rule(name, skill,
                cost=tuple((key, units * wow_cost / druid.wow_cost) for key, units in druid.cost) if wow_cost else (),
                health=tuple((key, units * wow_health / druid.wow_health) for key, units in druid.health)
                if wow_health else (), wow_cost=wow_cost or 1, wow_health=wow_health or 1, **kw)


# The Paladin. Heals play like Healing Touch, Judgement's strikes like Wrath, a seal's procs on her swing like the
# Druid's white hits, and the seals cost what Wrath would for their WoW mana (they buy damage). WoW mana she gets back
# converts at the rate her heals spend it, so a restore is worth what it pays for.
HEALING_TOUCH, WRATH = SPELLS[25297], SPELLS[9912]
MANA_AS_SPENT = (('restore_magicka', HEALING_TOUCH.base_cost / COUNTERPARTS['restore_magicka']['magnitude'],
                  HEALING_TOUCH.wow_cost),)
SEAL = ('A 30-second seal at Wrath\'s magicka per WoW mana, trained by the weapon the player swings (One- or Two-handed, as '
        'their white hits); its procs convert at their own rules, a new seal replaces it.')
MIGHT_NATIVE = ((0x7A0FF, .1, 1), (0x7A106, .1, 1))       # skyrim_effects.STAT[38]: attack power on gear
# Seal of the Crusader's attack power. In WoW it adds to a swing whose weapon damage the seal cut to 100/140 (the total
# is cut again for both); natively its Fortify multiplies the already cut hit, so its share is restored by 140/100.
CRUSADER_NATIVE = tuple((mgef, rate * 1.4, units) for mgef, rate, units in MIGHT_NATIVE)
PALADIN = {
    25292: like('Holy Light', 'Restoration', HEALING_TOUCH, 660, 1770, rank_health_floor=.25, rank_cost_floor=.25,
                reason='The big heal at Healing Touch\'s rates: 99 health for 104 magicka; the 2.5 s cast stays. '
                       'CMaNGOS heals through 19968 (level 1: its bonus coefficient takes a 0.2875 level penalty).'),
    19943: like('Flash of Light', 'Restoration', HEALING_TOUCH, 140, 388, rank_health_floor=.25, rank_cost_floor=.25,
                reason='The quick heal at Healing Touch\'s rates: 22 health for 22 magicka; the 1.5 s cast stays.'),
    10310: like('Lay on Hands', 'Restoration', HEALING_TOUCH, 0, 2201, mana=MANA_AS_SPENT,
                reason='Their maximum health (a naked level-60 Human\'s 2201: 123 at Healing Touch\'s rate; the event\'s '
                       'caster_max_health lets the plugin heal their native maximum) for all their magicka (use_all_mana, '
                       'no price); ranks 2-3\'s 250/550 mana to the target as spent (39/87). One-hour cooldown stays.'),
    20271: like('Judgement', 'Destruction', WRATH, 90,
                reason='A 10-yard holy strike at Wrath\'s magicka per WoW mana (6% of 1512: 20 magicka); what it casts '
                       'converts at the judgement\'s rule. 10 s cooldown stays.'),
    20293: like('Seal of Righteousness', 'OneHanded', WRATH, 200, weapon_skill=True, reason=SEAL),
    20308: like('Seal of the Crusader', 'OneHanded', WRATH, 160, weapon_skill=True, native={'attack_power': CRUSADER_NATIVE},
                reason=SEAL + ' Its attack power at the gear rate, as Blessing of Might\'s, restored by 140/100 because the '
                              'native Fortify multiplies the hit the seal already cut (325: +45.5% One- and Two-Handed '
                              'damage); its haste and damage factors go into their WoW swing.'),
    20920: like('Seal of Command', 'OneHanded', WRATH, 210, weapon_skill=True, reason=SEAL),
    20349: like('Seal of Light', 'OneHanded', WRATH, 210, weapon_skill=True, reason=SEAL),
    20357: like('Seal of Wisdom', 'OneHanded', WRATH, 200, weapon_skill=True, reason=SEAL),
    20164: like('Seal of Justice', 'OneHanded', WRATH, 196, weapon_skill=True, reason=SEAL + ' 13% of 1512.'),
    13819: like('Summon Warhorse', 'Conjuration', HEALING_TOUCH, 100,
                reason='Their level-40 class mount at Healing Touch\'s magicka per WoW mana (100); +60% speed and the 3 s '
                       'summon stay (skse/src/Mounts.cpp).'),
    23214: like('Summon Charger', 'Conjuration', HEALING_TOUCH, 150,
                reason='Their level-60 class mount at Healing Touch\'s magicka per WoW mana (150); +100% speed and the 3 s '
                       'summon stay.'),
    # The rest of her book at the rates of the Druid spell each plays like, so WoW's relative prices hold (percentages
    # truncate as Spell::CalculatePowerCost does: 8% of 1512 is 120). Holy Shock pays 2.3 times Flash of Light's
    # magicka per health, as it pays 325 mana for what Flash of Light heals for 140).
    20930: like('Holy Shock', 'Restoration', HEALING_TOUCH, 325,
                reason='Its 325 mana at Healing Touch\'s magicka per WoW mana; the heal or strike it casts converts at '
                       'its own rule (Healing Touch\'s or Wrath\'s per point). 30 s cooldown stays.'),
    20216: like('Divine Favor', 'Restoration', HEALING_TOUCH, 60,
                reason='4% of 1512 at Healing Touch\'s magicka per WoW mana; its one certain critical heal and 2-minute '
                       'cooldown stay.'),
    4987: like('Cleanse', 'Restoration', SPELLS[8946], 120,
               reason='8% of 1512 at Cure Poison\'s price per WoW mana; one poison, one disease and one harmful magic '
                      'removed (Skyrim\'s own types, Cast Dispel).'),
    1152: like('Purify', 'Restoration', SPELLS[8946], 120,
               reason='8% of 1512 at Cure Poison\'s price per WoW mana; one poison and one disease removed.'),
    20773: like('Redemption', 'Restoration', SPELLS[20748], 1209, 600, mana=SPELLS[20748].mana,
                reason='Rebirth\'s resurrection at its rates: 80% of 1512 mana, back with 600 health and 800 mana as '
                       'Rebirth converts its 2200 and 2800; out of combat, a 10-second cast.'),
    20066: like('Repentance', 'Alteration', SPELLS[18658], 60,
                reason='Six seconds of incapacitation at Hibernate\'s magicka per WoW mana (60); damage breaks it.'),
}


# The Paladin's auras and blessings (service/paladin_auras.py), anchored at each chain's top rank. They play like the
# Druid's buffs: a blessing costs Mark of the Wild's magicka per WoW mana (Oakflesh for 445: 103 / 445), an aura is
# free as in WoW; Devotion's armor and the resistance auras take Mark's per-point rates (40 armor per 285; 20/3 resist
# per 20, the gear table's 3 points a percent); Retribution is Thorns (8 health per 18 WoW damage on each melee hit);
# Sanctuary's Holy damage on a block is Holy Shield's per point. Blessing of Might's attack power and Kings's stats take
# the shared gear rates (skyrim_effects.STAT: 0.1% One-/Two-Handed damage per attack power; Mark's per-stat point).
# Blessing of Wisdom's mana converts as her heals spend it. Effects with no Skyrim counterpart (Salvation's threat,
# Light's healing, Sanctity's Holy damage, Concentration's pushback, Freedom's immunity, Sacrifice's transfer,
# Sanctuary's reduction) stay WoW rules the service and the plugin apply; none is dropped.
MARK, THORNS = SPELLS[9885], SPELLS[9910]
HOLY_SHIELD = PALADIN_DEFENSE[20928]
# Kings's +10% on a naked level-60 Human Paladin (105/65/100/70/81 -> +10 str, +6 agi, +10 sta, +7 int, +9 spi,
# StatSystem.cpp in single precision) at Mark of the Wild's per-point rates (its 'stat' natives per 12 points): stamina
# -> health, intellect -> magicka, agility -> stamina, strength -> carry weight and One-/Two-Handed damage, spirit ->
# magicka regeneration.
KINGS_REFERENCE = {'str': 10, 'agi': 6, 'sta': 10, 'int': 7, 'spi': 9}
KINGS_NATIVE = tuple((mgef, KINGS_REFERENCE[stat] * per12 / 12, 10) for mgef, per12, stat in (
    (0x493AA, 20, 'sta'), (0x49504, 20, 'int'), (0x49507, 20, 'agi'), (0x7A0F4, 12, 'str'), (0x7A0FF, 6, 'str'),
    (0x7A106, 6, 'str'), (0x7A0FD, 12, 'spi')))
BLESSING = ('A {duration} blessing on one unit at Mark of the Wild\'s magicka per WoW mana ({cost} WoW mana); '
            'one of hers per unit (spell_group 7).')


def _blessing(name, cost, duration='5-minute', **kw):
    return like(name, 'Alteration', MARK, cost, reason=kw.pop('reason', '') + BLESSING.format(duration=duration, cost=cost),
                **kw)


def _sanctuary(name, cost, duration):
    """Sanctuary's block damage (35 Holy) at Holy Shield's Skyrim health per WoW point."""
    rule = _blessing(name, cost, duration, minimum_health=True,
                     reason='-24 damage a hit (white-hit scale, as their block value) and 35 Holy to a blocked attacker '
                            '(Holy Shield\'s per-point rate). ')
    return replace(rule, health=tuple((key, units * 35 / HOLY_SHIELD.wow_health) for key, units in HOLY_SHIELD.health),
                   wow_health=35)


AURA = ' A party aura (30 yd), free and permanent as in WoW; one of hers at a time (spell_group 8).'
PALADIN_AURAS = {
    10293: Rule('Devotion Aura', 'Alteration', native={'armor': MARK.native['armor']},
                reason="Mark of the Wild's armor rate (40 per 285): 735 armor is 103 (129 with Improved Devotion Aura)."
                       + AURA),
    10301: like('Retribution Aura', 'Restoration', THORNS, 0, 20, minimum_health=True,
                reason="Thorns: 20 Holy damage to each melee attacker is 9 health (13 with Improved Retribution "
                       "Aura), the damage shield on every party member." + AURA),
    19746: Rule('Concentration Aura', 'Alteration',
                reason='Pushback and interrupt/silence resistance for WoW casting; no Skyrim counterpart.' + AURA),
    19900: Rule('Fire Resistance Aura', 'Alteration', native={'resistance': ((0x48C8B, 20 / 3, 20),)},
                reason="Mark's resistance rate (gear's 3 points a percent): 60 Fire resistance is 20%." + AURA),
    19898: Rule('Frost Resistance Aura', 'Alteration', native={'resistance': ((0x48F45, 20 / 3, 20),)},
                reason="Mark's resistance rate: 60 Frost resistance is 20%." + AURA),
    19896: Rule('Shadow Resistance Aura', 'Alteration', native={'resistance': ((0xB7A35, 20 / 3, 20),)},
                reason="Mark's resistance rate: 60 Shadow resistance is 20% Resist Magic (gear's Shadow -> Magic)." + AURA),
    20218: Rule('Sanctity Aura', 'Restoration',
                reason='+10% Holy damage for their WoW spells and swings; followers deal no Holy damage.' + AURA),
    25291: _blessing('Blessing of Might', 130, native={'attack_power': MIGHT_NATIVE},
                     reason='Attack power at the gear rate: 185 is +18.5% One- and Two-Handed damage. '),
    25290: _blessing('Blessing of Wisdom', 125, mana=MANA_AS_SPENT,
                     reason='33 mana every 5 s, converted as their heals spend mana (5 magicka). '),
    20217: _blessing('Blessing of Kings', 120, native={'stat_percent': KINGS_NATIVE},
                     reason='+10% stats: a naked level-60 Paladin\'s +10% at Mark\'s per-stat rates. '),
    1038: _blessing('Blessing of Salvation', 120, reason='-30% threat (their WoW threat; Skyrim has no threat table). '),
    19979: _blessing('Blessing of Light', 135,
                     reason='+400 to their Holy Light and +115 to their Flash of Light on the unit, converted with the heal. '),
    20914: _sanctuary('Blessing of Sanctuary', 135, '5-minute'),
    1044: Rule('Blessing of Freedom', 'Alteration', (('oakflesh', 151 / 445),), wow_cost=151,
               reason="Ten seconds' immunity to roots and snares, Mark's magicka per WoW mana; not in spell_group 7, so "
                      'it stays beside their other blessing.'),
    20729: _blessing('Blessing of Sacrifice', 100, '30-second',
                     reason='55 of each hit to the player instead (white-hit scale, op party_hit). '),
    25916: _blessing('Greater Blessing of Might', 260, '15-minute', native={'attack_power': MIGHT_NATIVE},
                     reason='As Blessing of Might, a Symbol of Kings each; the class-wide targets wait on D3. '),
    25918: _blessing('Greater Blessing of Wisdom', 250, '15-minute', mana=MANA_AS_SPENT,
                     reason='As Blessing of Wisdom, a Symbol of Kings each; the class-wide targets wait on D3. '),
    25898: _blessing('Greater Blessing of Kings', 150, '15-minute', native={'stat_percent': KINGS_NATIVE},
                     reason='As Blessing of Kings, a Symbol of Kings each; the class-wide targets wait on D3. '),
    25895: _blessing('Greater Blessing of Salvation', 241, '15-minute',
                     reason='As Blessing of Salvation (16% of 1512 mana), a Symbol of Kings each; the class-wide '
                            'targets wait on D3. '),
    25890: _blessing('Greater Blessing of Light', 260, '15-minute',
                     reason='As Blessing of Light, a Symbol of Kings each; the class-wide targets wait on D3. '),
    25899: _sanctuary('Greater Blessing of Sanctuary', 270, '15-minute'),
}


def paladin_payload_rules(cat):
    """Rules for what the seals and judgements cast (data/paladin_manifest.json links; not book ranks): damage riding
    her swing at the white-hit budget (FERAL_MELEE; a native weapon's swing lends its own factor), Judgement of
    Righteousness and Command's strikes at Wrath's, heals at Healing Touch's, mana as spent, debuffs and the stun with
    no health of their own. Shared factors: each rank's WoW numbers scale on their own."""
    from service import formulas as F, paladin, paladin_talents

    def payload(s, on_swing):
        effects = {s[f'Effect{e}'] for e in (1, 2, 3)}
        top = int(F.effect_value(s, 1, 60, None, maximum=True))
        if on_swing and effects & {paladin.E_SCHOOL_DAMAGE, paladin.E_WEAPON_PERCENT_DAMAGE}:
            return replace(FERAL_MELEE, name=s['SpellName'], minimum_health=True,
                           reason="A seal's holy damage on their swing: the white-hit budget (Iron Sword per 96.5).")
        if paladin.E_SCHOOL_DAMAGE in effects:
            return like(s['SpellName'], 'Destruction', WRATH, 0, top, minimum_health=True,
                        reason="A holy strike (a judgement's, Holy Shock's, Eye for an Eye's) at Wrath's health per WoW point.")
        if paladin.E_HEAL in effects:
            return like(s['SpellName'], 'Restoration', HEALING_TOUCH, 0, top, minimum_health=True,
                        reason="A proc's heal at Healing Touch's health per WoW point.")
        if paladin.E_ENERGIZE in effects:
            return Rule(s['SpellName'], 'Restoration', mana=MANA_AS_SPENT, reason="A proc's mana, as their heals spend it.")
        return Rule(s['SpellName'], 'Destruction', reason='A debuff or a control effect: no health or cost of its own.')

    rules = {}
    seals, judgements = paladin.seals(), paladin.judgements()
    for kind, rows in paladin.links().items():
        for source, targets in rows.items():
            for sid in targets:
                if sid not in cat.spells or sid in rules:
                    continue
                if kind in ('dummy_proc', 'trigger') and source in seals:        # the seal's procs
                    rules[sid] = payload(cat.spell(sid), on_swing=True)
                elif kind == 'judgement' and source in seals:                     # the judgement Judgement casts
                    damage = paladin._link('dummy', sid)                          # Command's: its damage rank
                    shown = cat.spell(damage) if damage in cat.spells else cat.spell(sid)
                    rules[sid] = replace(payload(shown, on_swing=False), name=cat.spell(sid)['SpellName'])
                    if damage in cat.spells:
                        rules.setdefault(damage, payload(shown, on_swing=False))
                elif kind == 'script' and source in judgements:                   # Light / Wisdom for the attacker
                    rules[sid] = payload(cat.spell(sid), on_swing=False)
                elif (kind == 'dummy' and source in paladin_talents.HOLY_SHOCK) or \
                        (kind == 'dummy_proc' and source in paladin_talents.EYE_FOR_AN_EYE):
                    # Talents' code-cast spells: Holy Shock's strike and heal (paladin_talents.resolve), Eye for an
                    # Eye's holy strike (its share of a hit she took stays in that hit's units: weapon_factor 1).
                    rules[sid] = payload(cat.spell(sid), on_swing=False)
    return rules


def class_base_mana(cat, sid, level=60):
    """The create mana (player_classlevelstats basemana) of the class whose book teaches this spell, at a level:
    Spell::CalculatePowerCost takes ManaCostPercentage of the caster's own (a Paladin's 1512 at 60, a Druid's 1244).
    A spell outside the class books, or a catalog without classlevelstats_by_class, uses the catalog character's."""
    c = cat.raw['character']
    cls = spell_classes().get(int(sid))
    rows = (c.get('classlevelstats_by_class') or {}).get(str(cls)) if cls != c.get('class', 11) else None
    if rows:
        return rows[str(level)]['basemana']
    return c.get('classlevelstats_by_level', {}).get(str(level), c['classlevelstats'])['basemana']


_SPELL_CLASSES = None


def spell_classes():
    """Spell -> ChrClasses ID of every class book (data/progression.json), for class_base_mana."""
    global _SPELL_CLASSES
    if _SPELL_CLASSES is None:
        from service.progression import DEFAULT
        import json
        book = json.loads(DEFAULT.read_text())
        _SPELL_CLASSES = {row['spell']: definition['id'] for definition in book['classes'].values()
                          for row in definition['spells'] + definition.get('excluded', [])}
    return _SPELL_CLASSES


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
    pinned = {**SPELLS, **PALADIN}   # the Druid's slice and the Paladin's pinned top ranks
    rules = dict(pinned)
    rules.update({sid: rule for sid, rule in PALADIN_DEFENSE.items() if sid in cat.spells})
    rules.update({sid: rule for sid, rule in PALADIN_AURAS.items() if sid in cat.spells})
    anchors = {cat.chain(sid): rule for sid, rule in rules.items()}
    from service.paladin import script_heal
    for sid, rule in paladin_payload_rules(cat).items():
        rules.setdefault(sid, rule)
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
            cost = s['ManaCost'] + s['ManaCostPercentage'] * class_base_mana(cat, top) // 100
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
            if rule.rank_cost_by_health:
                health = sum(int(F.effect_value(s, e, 60, None, maximum=True))
                             for e in (1, 2, 3) if s[f'Effect{e}'] == 113)
                share = max(.25, min(1, health / rule.wow_health))
                rank = replace(rank, cost=tuple((key, units * share) for key, units in rule.cost))
            if rule.rank_health_floor or rule.rank_cost_floor:
                # Normalize the finished CMaNGOS outcome to this rank's actual
                # integer baseline, then assign a bounded native rank budget.
                health = 0
                for e in (1, 2, 3):
                    value = int(F.effect_value(s, e, 60, None, maximum=True))
                    # A Paladin script heal's rank value is its heal (paladin.linked_heal).
                    if s[f'Effect{e}'] in (2, 10) or (s[f'Effect{e}'] == 77 and script_heal(sid)):
                        health += value
                    elif s[f'EffectApplyAuraName{e}'] in (3, 8):
                        amp = s[f'EffectAmplitude{e}']
                        health += value * (cat.duration(s, 60) // amp if amp else 1)
                cost = s['ManaCost'] + s['ManaCostPercentage'] * class_base_mana(cat, sid) // 100
                cost += s['ManaCostPerlevel'] * (F.spell_rank_level(s, 60) - s['BaseLevel'])
                health_share = rule.rank_health_floor + (1-rule.rank_health_floor) * health / rule.wow_health
                cost_share = rule.rank_cost_floor + (1-rule.rank_cost_floor) * cost / rule.wow_cost
                rank = replace(rank,
                               health=tuple((key, units*health_share) for key, units in rule.health),
                               cost=tuple((key, units*cost_share) for key, units in rule.cost),
                               wow_health=max(1, health), wow_cost=max(1, cost),
                               bonus_health_factor=rule.health_factor)
            rules[sid] = rank
    # Spell.dbc: Grasp triggers separate Roots spell IDs with the same rank
    # payloads (CMaNGOS Aura::TriggerSpell). Share the manual rank's native
    # budget so both resolve/tooltip and proc conversion use the same path.
    for triggered, manual in zip((19975, 19974, 19973, 19972, 19971, 19970),
                                 (339, 1062, 5195, 5196, 9852, 9853)):
        if triggered in cat.spells and manual in rules:
            rules[triggered] = rules[manual]
    return rules
