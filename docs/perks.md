# Skyrim perks and WoW spells

Taken perks now modify the appropriate class spells. The plugin observes all
**251 vanilla skill-tree PERK records**, including separate rank forms, using
`PlayerCharacter::HasPerk`; it does not infer perks from skill level. **86 perks**
have service translations. Every other perk has an explicit native or
no-analogue decision in the tables below.

## Data and class selection

`data/perk_rules.json` owns selectors, rank values, stacking decisions and the
reasons below. `data/build_perks.py` reads the actual Skyrim.esm `AVIF.PNAM` tree
nodes and `PERK.NNAM` rank links using `data/esm.py`, producing `data/perks.json`
and `skse/src/Perks.inc`. The artifact preserves ordered entry points, conditions,
function data and a source SHA-256. It excludes similarly named NPC/test perks.
`PRKE` distinguishes ability grants from entry points; `PRKC` identifies the
condition's parameter group; `EPFD` is interpreted according to its `EPFT`, not
always as a float. For example, Regeneration's multiplier is 1.5, while Dark
Souls' function data is a form reference, not a tiny damage multiplier.

The selected class's `data/progression.json` rows supply effective skill and
chain identity, including utility overrides and excluded/external ranks.
There are no druid/mage/rogue/etc. branches in the perk evaluator. A skill-line
bonus requires both the class skill **and the appropriate effect**: Armsman does
not buff Sprint, Alchemist does not buff Eviscerate just because Assassination
progresses through Alchemy, and Conjuration duration perks do not turn Rebirth
into a temporary summoned companion. Triggered combat payloads inherit their
active parent's line; pet-training wrappers do not become player attacks.
Druid Feral remains One-handed, including bear attacks; Barbarian applies to
Two-handed class lines and humanoid two-handed swings, not a second feral bonus.

`service/perks.py` adds exact spell-ID selectors to the same modifier list used
by talents (`service/formulas.py`, `service/talent_effects.py`). Exact selectors
avoid family-mask overlaps and support spells with zero family flags. Damage,
DoT, healing, duration, cost, crit chance and crit bonus use `SPELLMOD_*` before
crit/mitigation and native conversion. Percentage bonuses for the same op add
with WoW talents: +25% Augmented Shock plus +10% Moonfury gives +35%; -50% cost
plus -10% talent cost gives -60%, with the existing zero-cost clamp. The highest
owned rank replaces its lower ranks, rather than adding them. Assassin's Blade
replaces Backstab for eligible openers. Cached base characters are never mutated.
Mage Armor uses an effect-index filter on `SPELLMOD_ALL_EFFECTS`, so Mark's
armor grows without also tripling its stats or resistances.

The plugin supplies `caster.perks`, an object of exact editor-ID keys and boolean
ownership, and current `caster.class`. It also supplies `wearing_armor` (any worn
light/heavy armor, including a shield) and `stealthed` (actual Skyrim sneak);
feral `prowling` also qualifies. Missing equipment state does not grant Mage
Armor. Unknown perk keys, nonboolean ownership or malformed state are rejected.
Requests omitting `perks` retain legacy behavior. Perks are read again after
console additions/removals, level-up and loads; Skyrim owns their persistence.
The cost-display signature includes perks and class, so its next ordinary
refresh (within five seconds) uses the same discounts as the cast service.

Example payload fragment:

```json
{"class":11,"perks":{"DestructionApprentice25":true,"AugmentedShock":true,"Regeneration":true},"skyrim":{"destruction":50,"restoration":50}}
```

## Cost tiers

Each Novice/Apprentice/Adept/Expert/Master perk halves only its matching school's
**tier**, not every lower tier and not unrelated schools. Owning all five does
not discount a spell five times. Explicit druid counterpart budgets in
`service/skyrim_balance.py` retain their native tier across the chain:

| Tier | Native minimum skill | Explicit examples |
| --- | --- | --- |
| Novice | below 25 | Mark, Faerie Fire, Barkskin, Cat/Bear, Cure/Abolish Poison |
| Apprentice | 25–49 | Wrath, Moonfire, Rejuvenation, Regrowth, Innervate, Hibernate, Dire Bear |
| Adept | 50–74 | Starfire, Entangling Roots, Healing Touch, Thorns, Aquatic Form |
| Expert | 75–99 | Tranquility |
| Master | 100 | Hurricane, Rebirth |

The school comes from **class progression**, while the explicit budget sets the
tier. Thus Entangling Roots uses Destruction cost perks (Balance progression),
even though its existing combat conversion/training uses Alteration. Gift's
progression is Restoration and its Oakflesh budget is Novice; Mark's override is
Alteration. These distinctions preserve the existing progression choices.

For other class/rank budgets, the tier follows the whole Skyrim skill needed to
unlock that rank using the documented progression curve: WoW levels 1–7 are
Novice, 8–25 Apprentice, 26–42 Adept, 43–59 Expert, and 60 Master. A generic
Firebolt/Oakflesh balance fallback does not make every rank Apprentice/Novice.
Non-mana spells never receive magic-school cost perks. Stance perks can reduce
rage/energy; the core rounds costs in its own resource units before conversion.

## Mapping tables

`Native` means the actual Skyrim ability, equipment, crafting, detection or
combat operation retains its perk; the service adds no duplicate bonus.
`No analogue` means there is no appropriate implemented WoW effect to alter.
These are deliberate decisions, not unreported service modifiers. In particular,
this work does not supply pets, ingredients, poison charges, lock/theft results,
new fear/paralysis mechanics or dual-casting state.

Numbers that directly match vanilla entry points retain their values. Weapon
critical procs and material-dependent axe bleeds are explicit **translations**:
WoW already owns its attack/crit die and bleeds, so those perks improve that die
or existing ticks rather than adding a second Skyrim weapon hit. The axe
+25/50/75% tick choices are balance choices, not claimed vanilla EPFD multipliers.

### Destruction

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `DestructionNovice00` / `0F2CA8` | destruction class line; Novice mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `DestructionApprentice25` / `0C44BF` | destruction class line; Apprentice mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `DestructionAdept50` / `0C44C0` | destruction class line; Adept mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `DestructionExpert75` / `0C44C1` | destruction class line; Expert mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `DestructionMaster100` / `0C44C2` | destruction class line; Master mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `AugmentedFlames` / `0581E7`<br>`AugmentedFlames60` / `10FCF8` | Fire; damaging spells | Elemental damage, including periodic damage; WoW Nature is the lightning counterpart. Highest rank replaces its predecessor. |
| `AugmentedFrost` / `0581EA`<br>`AugmentedFrost60` / `10FCF9` | Frost; damaging spells | Elemental damage, including periodic damage; WoW Nature is the lightning counterpart. Highest rank replaces its predecessor. |
| `AugmentedShock` / `058200`<br>`AugmentedShock60` / `10FCFA` | Nature (shock); damaging spells | Elemental damage, including periodic damage; WoW Nature is the lightning counterpart. Highest rank replaces its predecessor. |
| `DestructionDualCasting` / `0153CF` | No analogue | WoW has one cast and GCD, with no two-hand spell composition or dual-cast resource surcharge. No extra potency, duration, or cost is invented. |
| `RuneMaster` / `105F32` | No analogue | Requires a placed-rune range entry point. Hurricane is a channeled storm; Hunter traps are a different progression line and require trap ownership/placement mechanics. No class spell is treated as a Skyrim rune. |
| `DeepFreeze` / `0F3933`<br>`Disintegrate` / `0F3F0E`<br>`Impact` / `0153D2`<br>`IntenseFlames` / `0F392E` | No analogue | These are linked elemental critical-health/dual-cast secondary effects. The current combat protocol has no post-hit health fraction, disintegration, or dual-cast state; adding automatic fear, paralysis, execution or stagger to ordinary WoW hits would change their spell mechanics. |

### Restoration

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `RestorationNovice00` / `0F2CAA` | restoration class line; Novice mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `RestorationApprentice25` / `0C44C7` | restoration class line; Apprentice mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `RestorationAdept50` / `0C44C8` | restoration class line; Adept mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `RestorationExpert75` / `0C44C9` | restoration class line; Expert mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `RestorationMaster100` / `0C44CA` | restoration class line; Master mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `Regeneration` / `0581F8` | direct, periodic and area heals; Frenzied Regeneration | Healing magnitude, including periodic and channel healing, is 50% stronger. Swiftmend consumes already-modified HoT ticks and inherits this bonus without applying it again. |
| `Respite` / `0581F9` | direct, periodic and area heals; Frenzied Regeneration | Restore Skyrim stamina equal to the converted healing amount, on the healed target. |
| `RestorationDualCasting` / `0153D1` | No analogue | WoW has one cast and GCD, with no two-hand spell composition or dual-cast resource surcharge. No extra potency, duration, or cost is invented. |
| `Recovery30` / `0581F4`<br>`Recovery50` / `0581F5` | Native | PERK adds PerkMagickaRecovery1/2 abilities (006C3D0/006C3D1), whose EFIT magnitudes are +25/+50 magicka regeneration. Skyrim already regenerates the magicka spent on WoW spells; do not add a second service regen bonus. |
| `AvoidDeath` / `0A3F64` | Native | PERK adds PerkAvoidDeathAbility (00A3F65) and its native death-ward script. WoW damage/heals change real Skyrim health, so the same emergency-heal ability observes it; there is no service-owned incoming-damage/day timer to duplicate. |
| `Necromage` / `0581E4` | Native | Target Undead/vampire conditions on ModSpellMagnitude (+25%) and ModSpellDuration (+50%) can match keyword-free carrier spells. Leave these entry points to Skyrim; adding the same service modifiers would double-apply. Service amounts/timers are before this native target-only adjustment. |
| `wardAbsorb` / `068BCC` | No analogue | Ward Absorb changes incoming ward magicka absorption. WoW Power Word: Shield absorbs health damage, not spell magicka, and the plugin has no ward-collision event; do not turn shield healing into resource generation. |

### Alteration

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `AlterationNovice00` / `0F2CA6` | alteration class line; Novice mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `AlterationApprentice25` / `0C44B7` | alteration class line; Apprentice mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `AlterationAdept50` / `0C44B8` | alteration class line; Adept mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `AlterationExpert75` / `0C44B9` | alteration class line; Expert mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `AlterationMaster100` / `0C44BA` | alteration class line; Master mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `Stability` / `0581FC` | alteration class line; finite utility/protection auras (Barkskin/Mark etc.) | Extend Alteration utility and protective auras by 50%; do not extend destructive spells or forms with indefinite duration. |
| `MageArmor30` / `0D7999`<br>`MageArmor50` / `0D799A`<br>`MageArmor70` / `0D799B` | positive armor wards; requires no worn armor | Double / 2.5x / triple positive magical armor while wearing no light or heavy armor; never amplify stats or resistance effects. |
| `AlterationDualCasting` / `0153CD` | No analogue | WoW has one cast and GCD, with no two-hand spell composition or dual-cast resource surcharge. No extra potency, duration, or cost is invented. |
| `MagicResistance30` / `053128`<br>`MagicResistance50` / `053129`<br>`MagicResistance70` / `05312A`<br>`atronach` / `0581F7` | Native | PERK grants native spell absorption (30%) or magic resistance (+10/+20/+30). Incoming Skyrim magic already uses these defenses. They do not increase outgoing WoW spells, and health carriers intentionally skip a second resist/absorb roll. |

### Illusion

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `IllusionNovice00` / `0F2CA9` | illusion class line; Novice mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `IllusionApprentice25` / `0C44C3` | illusion class line; Apprentice mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `IllusionAdept50` / `0C44C4` | illusion class line; Adept mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `IllusionExpert75` / `0C44C5` | illusion class line; Expert mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `IllusionMaster100` / `0C44C6` | illusion class line; Master mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `IllusionDualCasting` / `0153D0` | No analogue | WoW has one cast and GCD, with no two-hand spell composition or dual-cast resource surcharge. No extra potency, duration, or cost is invented. |
| `QuietCasting` / `0581FD` | Native | The unconditional Skyrim casting-sound entry point already makes native carrier casts silent for detection. WoW audiovisual effects are presentation; damaging an actor still provokes combat. Do not invent a threat reduction or invisibility modifier. |
| `Animage` / `0581E1`<br>`AspectOfTerror` / `059B78`<br>`HypnoticGaze` / `059B77`<br>`KindredMage` / `0581E2`<br>`MasterOfTheMind` / `059B76`<br>`Rage` / `0C44B5` | No analogue | These adjust Illusion target-level caps or permit new creature types. Classic fear/sleep/charm have explicit target masks and resistance, not Skyrim level-cap magnitudes. Preserve those rules rather than changing durations or allowing forbidden targets. Hibernate remains the class-data Alteration utility override. |

### Conjuration

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `ConjurationNovice00` / `0F2CA7` | conjuration class line; Novice mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `ConjurationApprentice25` / `0C44BB` | conjuration class line; Apprentice mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `ConjurationAdept50` / `0C44BC` | conjuration class line; Adept mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `ConjurationExpert75` / `0C44BD` | conjuration class line; Expert mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `ConjurationMaster100` / `0C44BE` | conjuration class line; Master mana cost | Halve only the matching school and tier, as the vanilla casting-perk condition does. |
| `ConjurationDualCasting` / `0153CE` | No analogue | WoW has one cast and GCD, with no two-hand spell composition or dual-cast resource surcharge. No extra potency, duration, or cost is invented. |
| `Atromancy` / `0CB419`<br>`DarkSouls` / `0581DE`<br>`ElementalPotency` / `0CB41A`<br>`MysticBinding` / `0640B3`<br>`Necromancy` / `0581DD`<br>`OblivionBinding` / `0D799C`<br>`SoulStealer` / `0D799E`<br>`Summoner30` / `105F30`<br>`Summoner70` / `105F31`<br>`TwinSouls` / `0D5F1C` | No analogue | These require summoned/reanimated actor duration, health, count, bound-weapon identity, soul capture, or summon placement. The plugin has no pet/summoned-actor lifecycle. Rebirth revives an existing ally permanently, and forms transform the caster; neither is a temporary summon. Conjuration cost perks still affect the class line. |

### One-handed

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `Armsman00` / `0BABE4`<br>`Armsman20` / `079343`<br>`Armsman40` / `079342`<br>`Armsman60` / `079344`<br>`Armsman80` / `079345` | one-handed class line; damaging spells | Scale damaging abilities of the class line; white attacks follow the actual weapon skill (feral follows One-handed). |
| `Bladesman30` / `05F56F`<br>`Bladesman60` / `0C1E90`<br>`Bladesman90` / `0C1E91` | one-handed class line; direct physical attacks | Translate the weapon critical proc to WoW critical chance +10/15/20 points and critical bonus +0/25/50%, using the class line rather than Skyrim weapon material. |
| `FightingStance` / `052D50` | one-handed class line; direct physical attacks | WoW offensive weapon abilities are the stamina-spending power-attack counterpart: 25% less rage/energy/mana cost; exclude utility. |
| `SavageStrike` / `03AF81` | one-handed class line; heavy strikes | Heavy strikes receive the 25% standing power-attack damage bonus; no decapitation in the WoW protocol. |
| `HackAndSlash30` / `03FFFA`<br>`HackAndSlash60` / `0C3678`<br>`HackAndSlash90` / `0C3679` | one-handed class line; physical bleed ticks | Translate the material-dependent axe bleed proc to +25/50/75% existing physical bleed ticks; no extra proc or invented weapon material. |
| `BoneBreaker30` / `05F592`<br>`BoneBreaker60` / `0C1E92`<br>`BoneBreaker90` / `0C1E93` | No analogue | Native mace-specific armor penetration requires weapon identity. Feral attacks have no mace material/subtype, and the service spell-mod protocol has no armor-penetration op. Preserve target armor rather than reinterpreting penetration as arbitrary flat damage. |
| `CriticalCharge` / `0CB406` | No analogue | Needs sprint power-attack state and its critical proc. WoW Charge/Feral Charge are movement/control and have no corresponding critical weapon strike; no damage or crit is invented. |
| `DualFlurry30` / `106256`<br>`DualFlurry50` / `106257`<br>`DualSavagery` / `106258`<br>`ParalyzingStrike` / `03AFA6` | No analogue | Requires backwards/sprinting/dual-wield power attacks, swing speed, paralysis or multi-target hit events absent from the class casting protocol. Existing Swipe/Whirlwind target counts and cast/attack timings remain their WoW values. |

### Two-handed

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `Barbarian00` / `0BABE8`<br>`Barbarian20` / `079346`<br>`Barbarian40` / `079347`<br>`Barbarian60` / `079348`<br>`Barbarian80` / `079349` | two-handed class line; damaging spells | Scale damaging abilities of the class line; white attacks follow the actual weapon skill (feral follows One-handed). |
| `DeepWounds30` / `03AF83`<br>`DeepWounds60` / `0C1E94`<br>`DeepWounds90` / `0C1E95` | two-handed class line; direct physical attacks | Translate the weapon critical proc to WoW critical chance +10/15/20 points and critical bonus +0/25/50%, using the class line rather than Skyrim weapon material. |
| `ChampionsStance` / `052D51` | two-handed class line; direct physical attacks | WoW offensive weapon abilities are the stamina-spending power-attack counterpart: 25% less rage/energy/mana cost; exclude utility. |
| `DevastatingBlow` / `052D52` | two-handed class line; heavy strikes | Heavy strikes receive the 25% standing power-attack damage bonus; no decapitation in the WoW protocol. |
| `Limbsplitter30` / `0C5C05`<br>`Limbsplitter60` / `0C5C06`<br>`Limbsplitter90` / `0C5C07` | two-handed class line; physical bleed ticks | Translate the material-dependent axe bleed proc to +25/50/75% existing physical bleed ticks; no extra proc or invented weapon material. |
| `Skullcrusher30` / `03AF84`<br>`Skullcrusher60` / `0C1E96`<br>`Skullcrusher90` / `0C1E97` | No analogue | Native mace-specific armor penetration requires weapon identity. Feral attacks have no mace material/subtype, and the service spell-mod protocol has no armor-penetration op. Preserve target armor rather than reinterpreting penetration as arbitrary flat damage. |
| `GreatCriticalCharge` / `0CB407` | No analogue | Needs sprint power-attack state and its critical proc. WoW Charge/Feral Charge are movement/control and have no corresponding critical weapon strike; no damage or crit is invented. |
| `Sweep` / `03AF9E`<br>`Warmaster` / `03AFA7` | No analogue | Requires backwards/sprinting/dual-wield power attacks, swing speed, paralysis or multi-target hit events absent from the class casting protocol. Existing Swipe/Whirlwind target counts and cast/attack timings remain their WoW values. |

### Archery

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `Overdraw00` / `0BABED`<br>`Overdraw20` / `07934A`<br>`Overdraw40` / `07934B`<br>`Overdraw60` / `07934D`<br>`Overdraw80` / `079354` | archery class line; damaging spells | Scale damaging abilities of the class line; white attacks follow the actual weapon skill (feral follows One-handed). |
| `CriticalShot30` / `105F1C`<br>`CriticalShot60` / `105F1E`<br>`CriticalShot90` / `105F1F` | archery class line; direct physical attacks | Translate the weapon critical proc to WoW critical chance +10/15/20 points and critical bonus +0/25/50%, using the class line rather than Skyrim weapon material. |
| `Bullseye` / `058F64`<br>`EagleEye30` / `058F61`<br>`HuntersDiscipline` / `051B12`<br>`PowerShot` / `058F62`<br>`QuickShot` / `105F19`<br>`Ranger` / `058F63`<br>`SteadyHand40` / `103ADA`<br>`SteadyHand60` / `103ADB` | Native | Native bow zoom/slow time, draw speed, stagger/paralysis, arrow recovery and aiming movement stay in Skyrim. There is no WoW ammunition/draw/aim event in the plugin; do not change spell cooldowns, ranges or apply a bow proc twice. |

### Block

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `ShieldWall00` / `0BCCAE`<br>`ShieldWall20` / `079355`<br>`ShieldWall40` / `079356`<br>`ShieldWall60` / `079357`<br>`ShieldWall80` / `079358` | block class line; positive armor wards | Improve protective Block-line armor wards (e.g. Devotion Aura). Native shield blocking remains Skyrim-owned. |
| `DeadlyBash` / `05F594` | block class line; Shield Bash / Shield Slam damage | Fivefold damage to Shield Bash/Shield Slam, not interrupts or dispels; native shield-bash damage is not replayed. |
| `BlockRunner` / `106253`<br>`DeflectArrows` / `058F68`<br>`DisarmingBash` / `058F66`<br>`ElementalProtection` / `058F69`<br>`PowerBashPerk` / `058F67`<br>`QuickReflexes` / `0D8C33`<br>`ShieldCharge` / `058F6A` | Native | These belong to actual Skyrim shield use, projectile deflection, elemental blocking, movement, disarm/stagger and slow time. Native blocking keeps them; WoW Shield Bash is an interrupt and Shield Wall is percentage mitigation, not those physical shield events. |

### Light Armor

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `AgileDefender00` / `0BE123`<br>`AgileDefender20` / `079376`<br>`AgileDefender40` / `079389`<br>`AgileDefender60` / `079391`<br>`AgileDefender80` / `079392`<br>`CustomFit` / `051B1B`<br>`DeftMovement` / `107831`<br>`MatchingSet` / `051B17`<br>`Unhindered` / `051B1C`<br>`WindWalker` / `105F22` | Native | Native worn-light-armor rating, matching/full-set checks, weight, stamina regeneration and hit avoidance already belong to Skyrim equipment. Mark/Barkskin/form armor are magical effects, not worn armor pieces; do not multiply them with equipment-only perks. |

### Heavy Armor

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `Conditioning` / `058F6D`<br>`Cushioned` / `0BCD2B`<br>`FistsOfSteel` / `058F6E`<br>`Juggernaut00` / `0BCD2A`<br>`Juggernaut20` / `07935E`<br>`Juggernaut40` / `079361`<br>`Juggernaut60` / `079362`<br>`Juggernaut80` / `079374`<br>`MatchingSetHeavy` / `107832`<br>`ReflectBlows` / `105F33`<br>`TowerOfStrength` / `058F6C`<br>`WellFitted` / `058F6F` | Native | Native worn-heavy-armor rating, matching/full-set checks, weight, unarmed gauntlets, fall/stagger mitigation and reflected blows belong to Skyrim equipment/events. Feral paws are service attacks, not native gauntlet punches; no additional outgoing spell multiplier is implied. |

### Sneak

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `Backstab` / `058210` | stealth melee openers; requires stealth | Double stealth opener damage (Ravage, Shred, Pounce; Ambush, Backstab, Garrote, Cheap Shot where damaging). |
| `AssassinsBlade` / `058211` | stealth melee openers; requires stealth | Vanilla dagger total is 6 x 2.5 = 15 versus 3 unperked: translate to 5x opener damage. Replaces Backstab rather than multiplying twice. |
| `DeadlyAim` / `1036F0` | archery class line; damaging spells; requires stealth | Vanilla bow sneak bonus multiplier 1.5 becomes 50% more damaging shots from stealth. |
| `LightFoot` / `05820C`<br>`MuffledMovement` / `058213`<br>`ShadowWarrior` / `058214`<br>`Silence` / `105F24`<br>`SilentRoll` / `105F23`<br>`Stealth00` / `0BE126`<br>`Stealth20` / `0C07C6`<br>`Stealth40` / `0C07C7`<br>`Stealth60` / `0C07C8`<br>`Stealth80` / `0C07C9` | Native | Prowl/Shadowmeld use real Skyrim sneak and detection. Native sneak skill, movement noise, traps, rolls and combat hide already apply there; do not turn detection percentages into extra damage, spell cost or a second stealth aura. |

### Alchemy

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `Alchemist00` / `0BE127`<br>`Alchemist20` / `0C07CA`<br>`Alchemist40` / `0C07CB`<br>`Alchemist60` / `0C07CC`<br>`Alchemist80` / `0C07CD` | alchemy class line; Instant / Deadly Poison payloads | Amplify damaging WoW poison effects only, not all Assassination attacks or poison recipe item counts. |
| `Poisoner` / `058217` | alchemy class line; Instant / Deadly Poison payloads | Damaging poisons gain 25%, sharing the additive WoW spell-mod pool with Alchemist and talents. |
| `Benefactor` / `058216`<br>`ConcentratedPoison` / `105F2F`<br>`Experimenter50` / `058218`<br>`Experimenter70` / `105F2A`<br>`Experimenter90` / `105F2B`<br>`GreenThumb` / `105F2E`<br>`Physician` / `058215`<br>`Purity` / `05821D`<br>`Snakeblood` / `105F2C` | Native | These improve crafted restoration potions, beneficial potion effects, ingredient knowledge, poison resistance, purity, weapon poison doses or harvesting. WoW heal spells are not brewed potions. Recipe casts only create items; current casting has no consumable-dose/inventory crafting event to multiply. Snakeblood already affects real Skyrim incoming poison. |

### Enchanting

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `CorpusEnchanter` / `058F7D`<br>`Enchanter00` / `0BEE97`<br>`Enchanter20` / `0C367C`<br>`Enchanter40` / `0C367D`<br>`Enchanter60` / `0C367E`<br>`Enchanter80` / `0C367F`<br>`ExtraEffect` / `058F7F`<br>`FireEnchanter` / `058F80`<br>`FrostEnchanter` / `058F81`<br>`InsightfulEnchanter` / `058F7E`<br>`SoulSiphon` / `108A44`<br>`SoulSqueezer` / `058F7C`<br>`StormEnchanter` / `058F82` | Native | These affect actual enchantment crafting, charges, soul capture and enchantment slots. Mark, Thorns and shaman imbues are temporary spell auras, not crafted Skyrim enchantments; do not grant them Enchanter potency or extra slots. Native enchanted items keep their normal perk-built values. |

### Smithing

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `AdvancedArmors` / `0CB414`<br>`ArcaneBlacksmith` / `05218E`<br>`DaedricSmithing` / `0CB413`<br>`DragonArmor` / `052190`<br>`DwarvenSmithing` / `0CB40E`<br>`EbonySmithing` / `0CB412`<br>`ElvenSmithing` / `0CB40F`<br>`GlassSmithing` / `0CB411`<br>`OrcishSmithing` / `0CB410`<br>`SteelSmithing` / `0CB40D` | Native | Material crafting/tempering and enchanting-item repair affect actual Skyrim equipment, not WoW spell magnitudes. The service never manufactures weapon stats from a smithing perk. |

### Lockpicking

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `AdeptLocks50` / `0C3680`<br>`ApprenticeLocks25` / `0BE125`<br>`ExpertLocks75` / `0C3681`<br>`GoldenTouch` / `05820A`<br>`Locksmith` / `058208`<br>`MasterLocks100` / `0C3682`<br>`NoviceLocks00` / `0F392A`<br>`QuickHands` / `106259`<br>`TreasureHunter` / `105F26`<br>`Unbreakable` / `058209`<br>`WaxKey` / `107830` | Native | Lock difficulty, sweet spots, picks, loot and keys belong to the Skyrim lock interface. Knowing WoW Pick Lock neither bypasses Skyrim locks nor supplies a lock minigame result; no cost/damage spell modifier fits. |

### Pickpocket

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `Cutpurse` / `058204`<br>`ExtraPockets` / `096590`<br>`KeyMaster` / `0D79A0`<br>`LightFingers00` / `0BE124`<br>`LightFingers20` / `018E6A`<br>`LightFingers40` / `018E6B`<br>`LightFingers60` / `018E6C`<br>`LightFingers80` / `018E6D`<br>`Misdirection` / `058201`<br>`NightThief` / `058202`<br>`PerfectTouch` / `058205`<br>`Poisoned` / `105F28` | Native | The native theft interface owns success, worn-item stealing, keys, capacity and reverse-pickpocket poisoning. WoW Pick Pocket does not implement that inventory/crime operation; no combat modifier fits. |

### Speech

| Vanilla perk keys / Skyrim.esm form IDs | WoW effect / ownership | Why and how |
| --- | --- | --- |
| `Allure` / `058F75`<br>`Bribery` / `058F72`<br>`Haggling00` / `0BE128`<br>`Haggling20` / `0C07CE`<br>`Haggling40` / `0C07CF`<br>`Haggling60` / `0C07D0`<br>`Haggling80` / `0C07D1`<br>`Intimidation` / `105F29`<br>`Investor` / `058F7B`<br>`MasterTrader` / `1090A5`<br>`Merchant` / `058F7A`<br>`Persuasion` / `1090A2`<br>`fence` / `058F79` | Native | Native buying/selling, persuasion, bribery, investment and stolen-goods rules own these benefits. Combat taunts/fears are not vendor prices or dialogue skill checks. |

## No double application in the plugin

WoW IDs are service/catalog IDs, not Skyrim `SpellItem` forms. `Cast::Spend`
subtracts the service's magicka cost directly. Damage/heals, roots and buffs use
`skse/src/Carriers.cpp` runtime forms: their effects have `associatedSkill=None`,
no vanilla school/element/healing/flesh keywords, and zero cost. Buff mapping
uses a vanilla MGEF only to find its actor value and sign, then creates a fresh,
keyword-free lasting effect. It does **not** cast the original Oakflesh, healing
spell or elemental spell. Consequently vanilla casting-perk conditions,
Augmented damage, Regeneration, Mage Armor and Stability do not reapply to the
service's scaled values. Weapon/sneak/bash entry points require real Skyrim
weapon hits, which these health carriers are not. `kNoDualCastMods` is also set
explicitly on carriers.

Necromage is the exception to keyword-based exclusion: its conditions depend on
the target being undead/vampire. It therefore remains native only, as documented
in its row; the service does not add the same +25%/+50% again. Service diagnostics
and scheduled ticks describe values/timers before any such native target-only
adjustment. Quiet Casting likewise retains its unconditional native sound entry
point. Native incoming defenses and Avoid Death remain engine-owned.

Respite is attached after Skyrim conversion: direct/area heal results carry
`stamina`, ordinary HoTs carry a snapshotted `respite` marker, and Frenzied
Regeneration marks its rage-to-health ticks. The plugin restores stamina on the
**healed actor**, at the actual converted heal magnitude, including HoT/channel
and rage-heal ticks. Stamina restoration is separately capped by Skyrim and
can occur at full health. HoT markers persist in the existing aura save record.
Swiftmend inherits Regeneration from the consumed HoT's already-modified WoW
`per_tick`; it does not multiply that perk a second time.

Mage Armor improves Mark/Gift, Ice Armor, Demon Armor and other positive armor
wards. **Barkskin has pushback/cast modifiers, not armor, and Thorns has reactive
damage, not armor** in these CMaNGOS rows: neither receives invented armor or a
Mage Armor damage bonus. Stability extends finite class-Alteration auras,
including Barkskin; Augmented Shock improves Thorns' Nature damage shield.
