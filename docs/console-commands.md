# WoWBridge console and test commands

Open Skyrim's console with `~`. Close it after each command so frame updates
and service replies can finish; reopen it to read the result. `wb` commands
log their results to `WoWBridge.log`. Player commands ignore the console's selected
reference; `wb target` uses the actor selected in the WoW target frame. Load a game with packaged character data before
using anything except `wb` / `wb help`.

## WoWBridge commands

Commands, skill names and set keys are case-insensitive. Numeric arguments are
whole numbers in the ranges shown. Implemented in [Console.cpp](../skse/src/Console.cpp).

| Command / arguments | Effect | Example |
| --- | --- | --- |
| `wb start` | Chooses eligible race and sex through native popups after the console closes; class is offered only during first-time setup or while onboarding customization is pending. On a new game, waits for character creation; activation waits for movement/combat controls. Existing characters keep their Skyrim name. | `wb start` |
| `wb` | Toggles the testing panel; close the console to use it. The Testing Tools micromenu button opens the same panel. | `wb` |
| `wb cooldowns` | Refreshes WoW spell/item cooldowns and GCD; preserves active casts/auras and native power/shout timers. Also available on the testing panel's Spells page. | `wb cooldowns` |
| `wb afflict` | Gives their Skyrim's Rattles (a disease), a lingering Damage Health poison and an ice hazard's frost (a harmful touch spell): one debuff of each 1.12 dispel type Skyrim effects take (Disease, Poison, Magic) to try Cure Poison, Abolish Poison, Purify or Cleanse on. Testing panel: Spells > Give Skyrim debuffs. Self-test: `wow_dispel`. | `wb afflict` |
| `wb defenses` | Prints live player/selected WoW target defense and regen AVs, bridge-subtracted values, converted resistance percentages, absorption and armor. Also on World / Target → Print live defenses. Close/reopen the console for the service reply. | `wb defenses` |
| `wb paladin ACTION` | Requests a learned Paladin spell through the normal cast lifecycle. Actions and fixture prerequisites are listed below; also available on the Paladin page. | `wb paladin judgement` |
| `wb paladin attack` | Toggles WoW white swings on the selected target; pacify blocks them. | `wb paladin attack` |
| `wb paladin hit [SEED]` | Queues a five-native-health incoming melee roll from the selected living fixture. Uses the production defence queue, including immunity and procs. | `wb paladin hit 37` |
| `wb paladin status` | Prints active auras/charges, ground areas, proc evidence, pending Reckoning attacks, native hook availability and the last incoming/party request/reply. | `wb paladin status` |
| `wb paladin fear_probe\|paralysis_probe\|stagger_probe\|interrupt_probe` | Applies a ten-second native fear/paralysis, a small magic stagger, or a hostile interruption of their current WoW cast. Needs a Paladin; talent/aura resistance is rolled normally. Paladin-panel rows have the same names; `status` prints mechanic percentages and counts. | `wb paladin fear_probe` |
| `wb help` | Lists commands and skill names. | `wb help` |
| `wb sex male` | Selects packaged male appearance; refreshes body, outfit, portrait and previews, sets `UnitSex` to 2 and the native player's male flag/voice. | `wb sex male` |
| `wb sex female` | Makes the player female (`UnitSex` 3, female flag/voice): the packaged female look if the player was male, their own look kept if the player already was. Selection is saved per game. | `wb sex female` |
| `wb appearance` | Opens the WoW creation controls for eligible race, sex and appearance; class buttons are enabled only during first-time setup or while onboarding customization is pending. Accept keeps the identity in the save, Cancel keeps the current one. Character page: Choose class and race. | `wb appearance` |
| `wb identity CLASS RACE,SEX,SKIN,FACE,HAIR,COLOR,FACIAL` | Testing override: applies a validated identity, allowing class changes after onboarding. Classes: 11 Druid (races 4/6), 2 Paladin (1/3). Sex: 0 male, 1 female. Class changes clear the old class's progression. Paladin spell actions are available on the Paladin testing page. | `wb identity 2 3,1,0,0,0,0,0` |
| `wb level N` (1–65535) | Sets Skyrim level directly. Increases trigger WoW level-up feedback; decreases synchronize quietly. Skips Skyrim's attribute choice (see below). | `wb level 60` |
| `wb skill NAME N` (0–100) | Sets one native skill's base value; refreshes class spell progression. | `wb skill destruction 30` |
| `wb skills N` (0–100) | Sets all 18 native skills; refreshes progression. | `wb skills 100` |
| `wb learnall` | Learns every packaged non-talent class rank, including quest/item-book ranks, and publishes book/bar updates. Skills stay as they are. | `wb learnall` |
| `wb unlearn` | Discards retained class ranks and rebuilds from current skills/talents. General/racial and Skyrim spells remain. | `wb unlearn` |
| `wb form cat` | Enters Cat Form with energy, effects and form action page. | `wb form cat` |
| `wb form bear` | Enters ordinary Bear Form (not Dire Bear). | `wb form bear` |
| `wb form travel` | Enters Travel Form. | `wb form travel` |
| `wb form aquatic` | Enters Aquatic Form, permitting a land preview; swim speed applies while swimming. | `wb form aquatic` |
| `wb form none` | Leaves the current form. | `wb form none` |
| `wb give ENTRY\|NAME` | Gives one WoW item, WoWBridge.esp's or the Armory's, by decimal entry or by name (any case; else the one name containing the text; quote names of more than two words). Does not equip it. | `wb give 873`, `wb give "lionheart helm"` |
| `wb give set KEY\|ID` | Gives one of every piece in a WoWBridge.esp set (by key) or an Armory set (by ItemSet.dbc ID; the pieces Skyrim has); gives nothing if any form is missing. | `wb give set cenarion`, `wb give set 211` |
| `wb sets` | Lists set keys, original names and entries, then the Armory's sets by ID with their Skyrim level. | `wb sets` |
| `wb find TEXT` | Lists the WoW items whose name contains TEXT (or whose entry is TEXT): WoWBridge.esp's, then up to 20 of the Armory's with Skyrim level and quality. | `wb find transcendence` |
| `wb random LEVEL QUALITY` / `wb randomequip LEVEL QUALITY` (1–100, 0–5) | Gives (or gives and equips) a random Armory item of that quality at the highest Skyrim level up to LEVEL (the lowest above where none is). Quality 0 poor .. 5 legendary. | `wb random 30 3` |
| `wb randomset LEVEL` | Gives a random complete Armory set of the highest Skyrim level up to LEVEL. | `wb randomset 48` |
| `wb usetrinkets` | Gives/equips Diamond Flask [20130] and The Burrower's Shell [23558], for stat/heal and absorb use tests. Also on Armory → Equip two use trinkets. Use character slots 13/14 or drag to action buttons. | `wb usetrinkets` |
| `wb shadows status` / `receive on\|off` / `blob on\|off` / `cascades on\|off` / `mask` | Session sun-shadow switches (default off), diagnostics and a full-resolution mask capture. See shadows. | `wb shadows receive on` |
| `wb npcshoulders on\|off\|status` | Toggles native NPCs' attached WoW shoulder models (default on, session only), or reports gathered wearers and last world-frame draw counts. Armory items in slot 57, within 32 yd. Also on the Armory testing page. | `wb npcshoulders status` |
| `wb npcweapons on\|off\|status` | Nearby NPCs' real WoW weapon/shield models in hand and at the sheath (32 yd, default on). Restores native geometry when off; reports items, sockets and last world-frame draws. Armory testing-panel rows also control it. | `wb npcweapons status` |
| `wb npchelms on\|off\|status` | Nearby NPCs' Human male/female WoW helm models on the head (32 yd, default on). Hides native clones and DBC-selected hair/beard after upload; restores them on release. Armory testing-panel rows also control it. | `wb npchelms status` |
| `wb npchelms describe FORMID` | Reports worn biped rows, model, visibility masks, hair/beard geometry and last draw frame. | `wb npchelms describe FF001234` |
| `wb npchelms lift\|out\|scale\|pitch N[f]` | Tunes per-sex head fit: node-local Z/Y (up/forward) units, positive scale multiplier, model pitch in degrees. Male by default; append `f` for female. | `wb npchelms pitch 5f` |
| `wb npcweapons type NAME` | Selects the session calibration row: dagger, sword, mace, axe, greatsword, battleaxe, hammer, staff, bow, crossbow or shield (default sword). | `wb npcweapons type staff` |
| `wb npcweapons lift\|out\|scale\|roll N[f]` | Tunes the selected type: node-local Z/X offsets in Skyrim units, positive scale multiplier, grip roll in degrees. Male by default; append `f` for female. | `wb npcweapons roll 15f` |
| `wb playerweapons on\|off\|status` | Skyrim weapon/shield clones on their WoW hands and sheath points (default off). Reports visibility, attachment distance, native updates, engine enchantment effect geometry and stance. Also on the Armory testing panel. | `wb playerweapons status` |
| `wb playerweapons glow\|frontview\|stance on\|off` | Independent session switches (each starts on): clone enchantment shader, self-test-only drawn front camera, and native weapon ready stance. The master bridge still starts off. See armory. | `wb playerweapons glow on` |
| `wb playerweapons type NAME` | Selects the session fit: dagger, sword, mace, axe, greatsword, battleaxe, hammer, staff, bow, crossbow or shield. | `wb playerweapons type shield` |
| `wb playerweapons lift\|out\|scale\|roll N` | Selected kind's attachment-frame Z/X offsets in Skyrim units before body scale, positive mesh scale, or grip roll in degrees. | `wb playerweapons roll 15` |
| `wb lootroll [COUNT]` (1–1000, default 100) | Rolls each vanilla list the Armory joined, and its vendor lists, COUNT times at their level and prints the WoW items found (`g_result.rolls`). | `wb lootroll 200` |
| `wb magicka N` (0–1000000) | Sets and refills base native magicka, also used as WoW mana. Fortify gear can raise the actual pool. | `wb magicka 5000` |
| `wb reset` | Stops casts/channels/placement, clears WoW cooldowns/auras and their native effects, and leaves forms/stealth. Keeps unrelated Skyrim magic, spells, skills, items and sex. | `wb reset` |
| `wb talents reset` | Refunds spent talents, removes talent-granted spells and rebuilds the class book from skills (also discards retained ranks). | `wb talents reset` |
| `wb talentpoints N` (0–51) | Legacy helper: sets Skyrim level to N + 9 (0 gives level 9), giving N total talent points minus those spent. Skips attribute choices. | `wb talentpoints 51` |
| `wb xp SKILL N` (0–1000000) | Grants native skill XP and normal level-up eligibility. | `wb xp destruction 1000` |
| `wb heal` | Refills health, magicka and stamina. | `wb heal` |
| `wb health N` / `wb stamina N` (1–1000000) | Sets and refills the native base pool. | `wb health 1000` |
| `wb equip ENTRY\|NAME` | Gives the WoW item (WoWBridge.esp's or the Armory's) if absent, then equips it. | `wb equip 873` |
| `wb nativeitem HEX COUNT` (1–100) | Gives an installed inventory item; rejects actors/world objects. | `wb nativeitem 0000000F 10` |
| `wb learnwow ID` | Learns one packaged non-talent rank of the playable class, including passive ranks. Optional second argument is the class key; other classes are rejected. | `wb learnwow 5176` |
| `wb learnskyrim HEX` | Learns an installed Skyrim spell. | `wb learnskyrim 00012FCD` |
| `wb damage N` / `wb speed N` (10–300) | Sets native weapon damage / movement base multiplier as a percent. | `wb damage 150` |
| `wb multipliers` | Restores those base multipliers to 100%; keeps active magical modifiers. | `wb multipliers` |
| `wb travel PLACE` | Travels to the winning live reference; logs the reference/world/position, or the fixed command fallback if unavailable. | `wb travel riverwood` |
| `wb npcscan` / `wb npcscan status` | Travels to Whiterun market, settles for 8 s, then measures NPC work for 10 s; prints mean/max ms and actor counts. Status prints the last result. | `wb npcscan` |
| `wb spawn TYPE COUNT` (1–5) | Spawns a dummy, wolf, chicken, bandit, bear, skeever, mudcrab or generic townsperson nearby. Dummy: AI disabled, 10000 HP. Bandit starts combat; townsperson starts unaggressive. | `wb spawn dummy 1` |
| `wb clearspawn` | Disables only actors spawned by these tools since loading the current game. | `wb clearspawn` |
| `wb target kill` / `revive` / `heal` | Runs native kill, resurrect (retaining inventory), or full-health reset on the loaded WoW target. Rejects missing targets, inappropriate alive/dead state, and player kill/revive. Native essential/protected rules apply. | `wb target heal` |
| `wb time HOUR` (0–23) | Sets the native game hour; does not change timescale. | `wb time 12` |
| `wb weather clear` | Forces SkyrimClear outdoors. | `wb weather clear` |
| `wb bounty clear` | Clears violent and nonviolent crime gold in all nine hold factions; does not stop an ongoing fight. | `wb bounty clear` |
| `wb god` | Toggles native god mode on each call. WoW knowledge, form and cooldown requirements still apply. | `wb god` |
| `wb book CLASS TAB` (0–255) | Prints the shipped class's ranks by tab, with known state, Skyrim skill threshold and acquisition source; republishes the panel catalog. 0 lists all tabs. Other classes are browse-only. | `wb book druid 1` |
| `wb learnchain ROOT [CLASS]` | Acquires every non-talent rank of a chain in the playable class. Does not change skills. | `wb learnchain 5176 druid` |
| `wb learntab TAB [CLASS]` (0–255) | Acquires non-talent ranks in the playable class's tab; 0 means all tabs. Talent ranks require spending talent points. | `wb learntab 3 druid` |
| `wb levelup health\|magicka\|stamina COUNT` (1–20) | Makes each next level eligible, opens Skyrim Skills/LevelUp, selects the native attribute callback and accepts its own confirmation. Each choice gives +10 chosen attribute and one perk; Stamina adds +5 carry weight. Wait for the result after closing the console. | `wb levelup stamina 5` |
| `wb levelup cancel` | Cancels pending scripted leveling; retains completed levels. | `wb levelup cancel` |
| `wb merchant spawn\|open\|remove` | Spawns a test vendor (in Lucan's vendor faction, its chest lent to this vendor's own stock until removed) with native samples, five spell books and two mount items to buy, and immediately requests native barter; reopens barter within reach, or removes both helpers. | `wb merchant spawn` |
| `wb givebook SPELL` / `wb givebook chain ROOT` / `wb givebook all` | Gives the spell book of one druid rank, of every rank of a chain, or all 179; use them from the bags or read them. | `wb givebook 25297` |
| `wb givereins KEY\|all` | Gives a mount's item (reins, whistle, horn) or every one. | `wb givereins swift_frostsaber` |
| `wb mounts forget\|restore` | Forgets every learned mount (a new game's state) or learns all of them again. | `wb mounts forget` |
| `wb gold N` (1–1000000) | Adds native septims; panel presets are 100, 1,000 and 10,000. | `wb gold 1000` |
| `wb lootchest spawn\|open\|remove` | Spawns/activates a varied native chest through the WoW loot backend, reopens it within reach, or removes it. | `wb lootchest spawn` |
| `wb scenario beast` | Places an AI-enabled passive domestic goat for Soothe/Hibernate and before-combat detection tests. | `wb scenario beast` |
| `wb scenario observers` | Places an AI-enabled Whiterun guard and generic farmer for Prowl/native-sneak comparisons and spell aggression. | `wb scenario observers` |
| `wb scenario corpse` | Places and kills a disposable wolf for Rebirth and dead-target resource/loot tests. Native kill completion may take a frame. | `wb scenario corpse` |
| `wb scenario shrine` | Places a Kynareth shrine beside the player. Allocate talents, activate the shrine and accept its normal price popup. | `wb scenario shrine` |
| `wb scenario inventory` | Gives three each of the varied loot fixture's native items for bag replacement, deposits and tooltips. | `wb scenario inventory` |
| `wb scenario reagents` | Gives 20 of every packaged reagent, resolving local ESP IDs automatically. | `wb scenario reagents` |
| `wb scenario threat` | Places an AI-enabled Whiterun guard made your follower and two wolves (1000 health) fighting it, for Growl and Challenging Roar (each wolf held on you for 3 / 6 s, then released; after Roar it returns to the guard) and Cower (a wolf attacking you turns on the follower fighting nearby). | `wb scenario threat` |
| `wb scenario snare` | Puts a native -50% slow (30 s) and WoWBridge's root carrier (8 s) on you; entering or leaving any form ends both and nothing else. | `wb scenario snare` |
| `wb scenario cc` | Places two still wolves (AI off, 1000 health) side by side for crowd control against Skyrim's damage: Hibernate, Entangling Roots or Soothe Animal one, then hurt it with a Skyrim spell or let a follower; any damage wakes it or ends its soothing, a root may break (WoW's damage roll; its own ticks never break it). Hibernate or Root the other and the first is freed (one target at a time, any rank). | `wb scenario cc` |
| `wb scenario crafting` | Gives Staff of Jordan, Stormrage Chestguard and ten each leather, strips, iron, steel and filled grand gems for native improvement/enchanting. | `wb scenario crafting` |
| `wb talents price` | Resets the Kynareth wipe price ladder to its first 1-gold wipe; preserves talent allocation and known ranks. | `wb talents price` |
| `wb emote TOKEN` | Validates a client emote token and plays its ordinary chat, voice and animation route. Panel lists all 1.12 tokens; STAND ends held poses. | `wb emote dance` |
| `wb react EMOTE` / `wb react status` | Plays the emote at nearby NPCs (or your target) and prints who answered and why others were skipped: a glance and a native idle per group (guards, townsfolk, followers); 30 s per NPC, 3 s between reactions, at most 3 NPCs; no crime or combat. Spoken greetings count as a wave. | `wb react wave` |
| `wb say` / `wb say long` | Your target (or the nearest actor within 20 yards) says a line as Skyrim's subtitles do: "<name> says:" in chat and the client's chat bubble over its head while it is on screen and in sight (Show Chat Bubbles); `long` wraps. Emotes page buttons. | `wb say long` |
| `wb showhelm on\|off\|toggle` / `wb showcloak on\|off\|toggle` | Uses the normal saved Show Helm/Cloak setting and refreshes the character/preview. Gear page has toggles. | `wb showhelm off` |

Skill names:

```text
onehanded twohanded marksman block smithing heavyarmor lightarmor
pickpocket lockpicking sneak alchemy speechcraft alteration conjuration
destruction illusion restoration enchanting
```

Aliases for `wb skill`: `archery` → `marksman`, `speech` → `speechcraft`,
`one_handed` → `onehanded`, `two_handed` → `twohanded`.
Learned ranks survive skill decreases until `wb unlearn`. Progression/book,
talent reset and entering-form helpers need the gameplay service. Form helpers
bypass learned-spell, mana, cooldown and outdoor/swimming restrictions; normal
casts keep their checks. Talents are spent in the TalentFrame (N, from level 10);
`wb learnall` excludes talent ranks. See progression,
talents, appearance
and gear for verification and feature history.

Set keys: `cenarion`, `nightslayer`, `giantstalker`, `earthfury`, `might`,
`lawbringer`, `arcanist`, `felheart`, `prophecy`, `stormrage` (eight pieces each).
Imported gear has no equip/use level requirement; `wb give` ignores class rows.
It accepts equipment entries; use `player.additem` below for reagents.

## Testing panel

Open with `wb` or the Testing Tools (formerly Social) micromenu button. Fourteen pages cover Character,
Skills, Gear, Items, Spells, Travel, Spawns, Mounts, World / Target, Class Spells,
Features, Emotes, Level Up and Armory. Opening,
filtering, selecting entries and changing pages do not change gameplay state. Click a named action
to apply it; the status line reports errors or completion and prevents duplicate
requests while a service operation is pending. Frames start locked and support
the standard right-click lock menu; Escape or the close button dismisses it.

Gear lists every WoW item, WoWBridge.esp's and the Armory's 8,500 (sent once a session; rows read
"Name  [entry]  Quality level"), and every set, the Armory's with their Skyrim level; its entry field also takes a
name. Armory gives a random item of a Skyrim level and quality (or equips it), a random set of a level, and rolls the
lists the Armory joined (`wb lootroll 100`); quality and level presets fill its fields. Native item/spell fields
accept hexadecimal runtime form IDs; WoW fields accept decimal entries/spell IDs.
Skill XP uses Skyrim's normal advancement; direct level/skill values keep the
existing distinction described above. Skills has value presets and one-click XP
amounts; Character has direct level presets. Selected skills and item/spell
entries show a gold highlight and `>` marker. Lists align left, support filtering,
mouse-wheel paging and Previous/Next, and show full entry text in tooltips.
World / Target offers target kill/revive/heal, time presets, weather, hold
bounties and a god-mode toggle.

Class Spells reads the **installed** progression catalog (currently nine classes),
ordered by tab, spell name and rank. Each row shows Known/Unknown, its Skyrim
skill threshold and any item/talent/external acquisition gate. The active class's
markers follow authoritative book publications without reopening. Select a row
before Learn selected rank/chain; choose the tab number for Learn whole tab.
The class key field and Previous/Next class controls browse only shipped classes;
learning a different class is rejected. The player remains a druid. Talent ranks
stay under the talent allocator even when a whole tab is learned. Give rank's book,
Give chain's books and Give all books put spell books in their bags (a row's tooltip
names its book). Mounts lists every mount ("not learned" ones marked) and each
mount's item: Forget all mounts, Restore all mounts, Give all reins.

Level Up uses the audited Skyrim **1.7.104** native menu callbacks and confirmation,
rather than `setlevel`. It makes one level eligible per menu visit, preserves
pre-existing excess overall XP, and allows gameplay polling between successive
choices so the WoW chat/sound/light and progression can update. It does not train
individual skills; use Skills → Add skill XP for skill-driven spell acquisition.
The native menu chooses the attribute, carry weight and perk; result evidence
checks their deltas. A missing callback, timeout, unsupported runtime or existing
Skills/LevelUp/message box produces a reported failure. Direct Character level
presets remain labelled as skipping choices.

The temporary merchant uses **a private duplicate of Lucan's general vendor
faction** (`ServicesRiverwoodRiverwoodTrader`, `0005A665`), on a spawned generic
farmer (`001034E4`). Skyrim.esm's faction has 07:00–20:00 hours, a location
restriction (`PLVD`, Riverwood Trader) and a shared merchant reference
(`VENC`, `00078C0B`). Copying only Lucan's actor would leave these restrictions
and shared stock in force. The helper clears location/conditions, sets hours
0–24 and points the copied faction at its own stocked native chest, then calls
`RE::BarterMenu::OpenMenu`. No original actor, faction or merchant inventory is
modified. The vendor has 10,000 gold and five of each fixture item; the native
menu handles buying/selling, prices and input recovery. Remove merchant disables
and marks both references for deletion. Vendor AI is disabled to hold it nearby;
barter is opened directly, without relying on dialogue/schedules.

The non-respawning loot chest (`TreasChestSmallEMPTYNoRespawn`, `000F8478`)
contains three each iron sword, iron cuirass, minor health potion, wine, blue
mountain flower, a book, a note, key, empty/filled petty soul gems and an unnamed
BasicKnife record. The unnamed record deliberately tests the existing exclusion
from loot/bags; named items remain usable in the ordinary loot/deposit flow.
Open/Remove loot chest operate on that helper only. `clearspawn` removes scenario
actors/shrines; merchant and loot chest have separate removal actions. Tracking
is cleared on game load; saving with helpers present can persist their references.

Features connects the manual playtest routes:

| Feature / manual check | Setup / route |
| --- | --- |
| Soothe, Hibernate, intermittent action flashes | Passive beast, Class Spells learn desired chain, Spells refresh cooldowns; cast other spells and change target/forms while observing slots. |
| Prowl detection / harmful spell aggression | Guard + townsperson; compare native sneak and Prowl with the same light, distances and skills. These are native observers, not a promised detection result. |
| Growl, Challenging Roar, Cower | Wolves fight a guard follower (`threat`); learn the ranks (Class Spells), take Bear or Cat Form and watch whom each wolf attacks. |
| Shapeshift freeing roots and snares | Native slow and root on you (`snare`); take or leave any form. |
| Rebirth / dead target resources | Reagents + wolf corpse; target corpse, cast learned Rebirth, then inspect living/dead target resources. |
| Damage/heal numbers, range/LOS, mitigation | 10000-HP dummy, native damage/resources, class rank/talent/gear selection; Travel dungeon/bandits/dragon for natural encounters. |
| Bags, full bags, deposits, split stacks, books/notes and tooltips | Varied inventory items + loot chest; Gear gives/equips sets; native inventory supplies ordinary worn/enchanted/tempered cases. |
| Native crafting, improved/enchanted WoW items | Crafting fixture + Travel Warmaidens; use native forge/grindstone/workbench/enchanting table. |
| Talent price ladder / no-points error | Local shrine, Reset price, Refund talents, total points 0–51 and Open talents; zero total points sets level 9. |
| Ground/flying mount, buff, right-click dismissal and saved preference | Mounts lists the entire installed catalog, including flyers; Dismount, Travel flight/lake; use `/mount` for preference. |
| Emote chat/voice/animation, held poses | Emotes lists all tokens, Play and Stand; select a target for targeted text. |
| Show Helm/Cloak, equipment appearance and sex | Gear toggles + give/equip; Character Male/Female and inventory/character preview. |
| Native barter, money and UI/input return | Features merchant + gold presets; buy, sell, cancel, remove helper. Travel Riverwood Trader remains the real-vendor dialogue route. |
| Carriage/taxi and trainer | Travel stables (exit to Bjorlam's carriage) / Faendal's house, then activate the native service. |
| Quest log / NPC dialogue / courier observation | Town travel + native NPC activation; regular quest-generated delivery remains a play check. |
| Maps/compass, aquatic/flight, zone/time/weather | Travel towns/dungeons/lake/flight, Features world map, World / Target clock/weather; compare native compass at the same position. |
| Chat, macros, bindings, bars, frame locks and saved UI profiles | Features macros, regular chat/interface menus, drag bars/frames, right-click lock; save/reload a disposable game to test persistence. |
| Skyrim spells / shouts / perks | Spells native spell ID, Open Skyrim magic / Skills; native acquisition remains available there. |

`wow_panel_helpers` is **explicit-only** (`[Test] only=wow_panel_helpers`). It
records separate pass/fail and evidence for each gold preset, each native
Health/Magicka/Stamina choice (level, attribute, carry, perk, WoW event/sound/light request and native visual counts),
class known/unknown agreement, rank/chain/tab learning, talent-price reset, rejected arguments, vendor placement, visible stocked native barter,
buy/sell, removal, and the WoW loot window/native item inventory. Its paused
barter observer invokes native trade callbacks without desktop input. Baseline
level, perks, skill/XP data, attribute pools, gold, progression and supplied
helper references are restored on normal completion and timeout. The orchestrator
must run this step in game; portable/syntax checks cannot establish its native pass.

Travel keys and native destinations (new CELL editor IDs, exterior grids,
NPC bases, weather and hold faction IDs verified directly in Skyrim.esm):

| Key | Destination / native command | Test purpose |
| --- | --- | --- |
| `riverwood` | `coc Riverwood` | Town and nearby wildlife. |
| `whiterun` | `cow WhiterunWorld 6 -2` | City services. |
| `solitude` | `cow SolitudeWorld -13 25` | City services. |
| `windhelm` | `cow WindhelmWorld 33 9` | City services. |
| `riften` | `cow RiftenWorld 42 -24` | City services. |
| `markarth` | `cow MarkarthWorld -43 1` | City services. |
| `dawnstar` | `cow Tamriel 6 24` | Coast and town. |
| `blackreach` | `cow Blackreach 0 0` | Underground worldspace. |
| `qasmoke` | `coc QASmoke` | Indoor item testing. |
| `barrow` | `coc BleakFallsBarrow01` | Dungeon floor plan, line of sight and combat. |
| `lake` | `cow Tamriel -8 -12` (IlinaltasDeepExterior01) | Open water by Ilinalta's Deep for Aquatic Form. |
| `flight` | `coc HighHrothgarExterior01` | Mountain flight and outdoor mounts. |
| `shrine` | `coc WhiterunTempleofKynareth` | Activate the shrine to refund talents. |
| `merchant` | `coc RiverwoodRiverwoodTrader` | Lucan; merchant and inventory checks. |
| `crafting` | `coc WhiterunWarmaidens` | Native improvement/enchanting; forge outside. |
| `stables` | `coc WhiterunStables` | Exit to Bjorlam and native carriage/taxi. |
| `trainer` | `coc RiverwoodFaendalsHouse` | Faendal; archery trainer, may be outdoors by day. |
| `bandits` | `coc ValtheimKeepExterior01` | Valtheim bandits and outdoor combat. |
| `dragon` | `coc DragonLair04Exterior01` | Dragon lair; presence depends on quest/respawn state. |

Spawn base IDs: `dummy` / `wolf` = `00023ABE` (EncWolf), `chicken` =
`000A91A0` (EncChicken), `bandit` = `0001BCD8` (EncBandit01Melee1HNordM),
`bear` = `00023A8A` (EncBear), `skeever` = `00023AB7` (EncSkeever),
`mudcrab` = `00021875` (EncMudcrabGiant), `townsperson` = `001034E4`
(WEFarmerMale, a generic farmer). NPC schedules and save state still apply.
The panel previews learned forms only. Console form previews also work before
a form is learned: the active form supplies power and action-page state without
creating a learned shapeshift button.
Native damage percentage affects Skyrim weapon attacks, not WoW service damage.
Spawn ownership is cleared on loading another game. Clear spawned stops combat
and disables tracked actors; it does not delete them or affect unrelated actors.
These are intentional testing controls and their changes persist if you save
the game.

## Vanilla Skyrim console

| Command | Effect / example |
| --- | --- |
| `coc CELL` | Travel to a cell: `coc Riverwood` outdoors, `coc QASmoke` for the interior test cell. |
| `player.setlevel N` | Direct overall level change, like `wb level`: `player.setlevel 60`. **Does not grant Health/Magicka/Stamina choices.** |
| `player.advskill SKILL XP` | Earn native skill XP: `player.advskill destruction 1000` (repeat as needed). When a level is ready, click the WoW XP bar or use Shift+Tab → Skills to choose Health/Magicka/Stamina. Class spell acquisition follows skills separately. |
| `player.setav SKILL N` | Set a skill directly: `player.setav destruction 100`. Book updates follow; this does not earn skill XP. Use vanilla names above, e.g. `marksman` for Archery. |
| `player.setav magicka N` / `player.restoreav magicka N` | Set the base pool, then refill: `player.setav magicka 5000`, `player.restoreav magicka 5000`. |
| `player.additem FORM_ID COUNT` | Add items by **hex Skyrim form ID**, not WoW entry. Gold: `player.additem 0000000F 10000`; lockpicks: `player.additem 0000000A 20`. WoW forms are listed below. |
| `help "NAME" 4` | Find a loaded form's full runtime ID: `help "Staff of Jordan" 4` or `help "Ironwood Seed" 4`. Choose the matching WEAP/ARMO/MISC item. |
| `tgm` | Toggle native god mode. WoW knowledge/form/cooldown checks still apply. |
| `tcl` | Toggle collision; clear the console's selected reference to apply it to player movement. |
| `set timescale to N` | Change world-clock rate: `set timescale to 1`; restore vanilla with `set timescale to 20`. WoW frame timers are separate. |
| `fw WEATHER_FORM_ID` | Force weather: `fw 0000081A` selects SkyrimClear. |
| `save NAME` / `load NAME` | Save/reload the game and SKSE co-save: `save WBMaleManualTest`, then `load WBMaleManualTest`. |

### WoW item form IDs

WoWBridge.esp is **ESL-flagged**. The table lists stable hexadecimal **local**
item IDs from [esp_forms.json](../data/esp_forms.json), with names from
gear.json / catalog.json.
A runtime ID is `FE` + the ESP's three-digit light-plugin index + the three-digit
local ID. Resolve the full ID with `help "NAME" 4`; do not assume a load index.
For example, **only if its light index is `000`**, Staff of Jordan is
`FE000800`, so `player.additem FE000800 1` gives one staff; Ironwood Seed is
`FE000822`, so `player.additem FE000822 10` gives ten seeds.
`wb give 873` resolves the equipment form automatically. Ability/enchantment,
recipe and effect records are not inventory items and are omitted.

WoWBridgeArmory.esp is a **regular** plugin after WoWBridge.esp: a runtime ID is its two-digit load index + the
six-digit local ID in [armory_forms.json](../data/armory_forms.json) (`item:ENTRY`). Use `wb give`/`wb find`, or
`help "NAME" 4`, rather than assume an index.

#### Equipment (95 items)

| WoW entry | Item | Local form (hex) |
| --- | --- | --- |
| 873 | Staff of Jordan | `800` |
| 1168 | Skullflame Shield | `841` |
| 5201 | Emberstone Staff | `803` |
| 16795 | Arcanist Crown | `845` |
| 16796 | Arcanist Leggings | `849` |
| 16797 | Arcanist Mantle | `84B` |
| 16798 | Arcanist Robes | `84E` |
| 16799 | Arcanist Bindings | `851` |
| 16800 | Arcanist Boots | `854` |
| 16801 | Arcanist Gloves | `857` |
| 16802 | Arcanist Belt | `859` |
| 16803 | Felheart Slippers | `85B` |
| 16804 | Felheart Bracers | `85E` |
| 16805 | Felheart Gloves | `861` |
| 16806 | Felheart Belt | `864` |
| 16807 | Felheart Shoulder Pads | `866` |
| 16808 | Felheart Horns | `868` |
| 16809 | Felheart Robes | `86A` |
| 16810 | Felheart Pants | `86C` |
| 16811 | Boots of Prophecy | `86F` |
| 16812 | Gloves of Prophecy | `872` |
| 16813 | Circlet of Prophecy | `874` |
| 16814 | Pants of Prophecy | `876` |
| 16815 | Robes of Prophecy | `879` |
| 16816 | Mantle of Prophecy | `87B` |
| 16817 | Girdle of Prophecy | `87D` |
| 16819 | Vambraces of Prophecy | `87F` |
| 16820 | Nightslayer Chestpiece | `882` |
| 16821 | Nightslayer Cover | `885` |
| 16822 | Nightslayer Pants | `888` |
| 16823 | Nightslayer Shoulder Pads | `88A` |
| 16824 | Nightslayer Boots | `88D` |
| 16825 | Nightslayer Bracelets | `88F` |
| 16826 | Nightslayer Gloves | `891` |
| 16827 | Nightslayer Belt | `893` |
| 16828 | Cenarion Belt | `895` |
| 16829 | Cenarion Boots | `897` |
| 16830 | Cenarion Bracers | `899` |
| 16831 | Cenarion Gloves | `89C` |
| 16833 | Cenarion Vestments | `89E` |
| 16834 | Cenarion Helm | `8A0` |
| 16835 | Cenarion Leggings | `8A2` |
| 16836 | Cenarion Spaulders | `8A4` |
| 16837 | Earthfury Boots | `8A6` |
| 16838 | Earthfury Belt | `8A8` |
| 16839 | Earthfury Gauntlets | `8AA` |
| 16840 | Earthfury Bracers | `8AC` |
| 16841 | Earthfury Vestments | `8AE` |
| 16842 | Earthfury Helmet | `8B0` |
| 16843 | Earthfury Legguards | `8B2` |
| 16844 | Earthfury Epaulets | `8B4` |
| 16845 | Giantstalker's Breastplate | `8B6` |
| 16846 | Giantstalker's Helmet | `8B8` |
| 16847 | Giantstalker's Leggings | `8BA` |
| 16848 | Giantstalker's Epaulets | `8BC` |
| 16849 | Giantstalker's Boots | `8BE` |
| 16850 | Giantstalker's Bracers | `8C0` |
| 16851 | Giantstalker's Belt | `8C2` |
| 16852 | Giantstalker's Gloves | `8C4` |
| 16853 | Lawbringer Chestguard | `8C7` |
| 16854 | Lawbringer Helm | `8C9` |
| 16855 | Lawbringer Legplates | `8CB` |
| 16856 | Lawbringer Spaulders | `8CD` |
| 16857 | Lawbringer Bracers | `8CF` |
| 16858 | Lawbringer Belt | `8D1` |
| 16859 | Lawbringer Boots | `8D3` |
| 16860 | Lawbringer Gauntlets | `8D5` |
| 16861 | Bracers of Might | `8D7` |
| 16862 | Sabatons of Might | `8D9` |
| 16863 | Gauntlets of Might | `8DC` |
| 16864 | Belt of Might | `8DE` |
| 16865 | Breastplate of Might | `8E1` |
| 16866 | Helm of Might | `8E5` |
| 16867 | Legplates of Might | `8E7` |
| 16868 | Pauldrons of Might | `8EA` |
| 16897 | Stormrage Chestguard | `805` |
| 16898 | Stormrage Boots | `809` |
| 16899 | Stormrage Handguards | `80C` |
| 16900 | Stormrage Cover | `80E` |
| 16901 | Stormrage Legguards | `811` |
| 16902 | Stormrage Pauldrons | `814` |
| 16903 | Stormrage Belt | `816` |
| 16904 | Stormrage Bracers | `818` |
| 17182 | Sulfuras, Hand of Ragnaros | `8ED` |
| 18348 | Quel'Serrar | `8F0` |
| 18608 | Benediction | `8F2` |
| 18609 | Anathema | `8F6` |
| 18713 | Rhok'delar, Longbow of the Ancient Keepers | `8F9` |
| 18715 | Lok'delar, Stave of the Ancient Keepers | `8FC` |
| 18816 | Perdition's Blade | `8FF` |
| 18842 | Staff of Dominance | `901` |
| 19019 | Thunderfury, Blessed Blade of the Windseeker | `904` |
| 19364 | Ashkandi, Greatsword of the Brotherhood | `906` |
| 22397 | Idol of Ferocity | `81B` |
| 22398 | Idol of Rejuvenation | `81E` |

#### Reagents and supplies (32 items)

| WoW entry | Item | Local form (hex) |
| --- | --- | --- |
| 2928 | Dust of Decay | `828` |
| 2930 | Essence of Pain | `829` |
| 3371 | Empty Vial | `82A` |
| 3372 | Leaded Vial | `82B` |
| 3818 | Fadeleaf | `82C` |
| 5140 | Flash Powder | `82D` |
| 5173 | Deathweed | `82E` |
| 5530 | Blinding Powder | `82F` |
| 5565 | Infernal Stone | `830` |
| 6265 | Soul Shard | `831` |
| 8923 | Essence of Agony | `832` |
| 8924 | Dust of Deterioration | `833` |
| 8925 | Crystal Vial | `834` |
| 16583 | Demonic Figurine | `835` |
| 17020 | Arcane Powder | `836` |
| 17021 | Wild Berries | `823` |
| 17026 | Wild Thornroot | `821` |
| 17028 | Holy Candle | `837` |
| 17029 | Sacred Candle | `838` |
| 17030 | Ankh | `839` |
| 17031 | Rune of Teleportation | `83A` |
| 17032 | Rune of Portals | `83B` |
| 17033 | Symbol of Divinity | `83C` |
| 17034 | Maple Seed | `824` |
| 17035 | Stranglethorn Seed | `825` |
| 17036 | Ashwood Seed | `826` |
| 17037 | Hornbeam Seed | `827` |
| 17038 | Ironwood Seed | `822` |
| 17056 | Light Feather | `83D` |
| 17057 | Shiny Fish Scales | `83E` |
| 17058 | Fish Oil | `83F` |
| 21177 | Symbol of Kings | `840` |

#### Mount items (23 items) and spell books (179)

| WoW entry | Item | Teaches | Local form (hex) |
| --- | --- | --- | --- |
| 18766 | Reins of the Swift Frostsaber | `swift_frostsaber` | `A84` |
| 18767 | Reins of the Swift Mistsaber | `swift_mistsaber` | `A82` |
| 18768 | Reins of the Swift Dawnsaber | `swift_dawnsaber` | `A83` |
| 18772 | Swift Green Mechanostrider | `swift_green_mechanostrider` | `A87` |
| 18773 | Swift White Mechanostrider | `swift_white_mechanostrider` | `A86` |
| 18774 | Swift Yellow Mechanostrider | `swift_yellow_mechanostrider` | `A85` |
| 18776 | Swift Palomino | `swift_palomino` | `A88` |
| 18777 | Swift Brown Steed | `swift_brown_steed` | `A8A` |
| 18778 | Swift White Steed | `swift_white_steed` | `A89` |
| 18785 | Swift White Ram | `swift_white_ram` | `A8D` |
| 18786 | Swift Brown Ram | `swift_brown_ram` | `A8B` |
| 18787 | Swift Gray Ram | `swift_gray_ram` | `A8C` |
| 18788 | Swift Blue Raptor | `swift_blue_raptor` | `A8E` |
| 18789 | Swift Olive Raptor | `swift_olive_raptor` | `A8F` |
| 18790 | Swift Orange Raptor | `swift_orange_raptor` | `A90` |
| 18791 | Purple Skeletal Warhorse | `purple_skeletal_warhorse` | `A91` |
| 18793 | Great White Kodo | `great_white_kodo` | `A92` |
| 18794 | Great Brown Kodo | `great_brown_kodo` | `A94` |
| 18795 | Great Gray Kodo | `great_gray_kodo` | `A93` |
| 18796 | Horn of the Swift Brown Wolf | `swift_brown_wolf` | `A95` |
| 18797 | Horn of the Swift Timber Wolf | `swift_timber_wolf` | `A96` |
| 25470 | Swift Golden Gryphon | `swift_golden_gryphon` | `A97` |
| 25531 | Swift Green Wind Rider | `swift_green_wind_rider` | `A98` |

Spell books use `book:SPELL` keys in esp_forms.json (`wb givebook SPELL` resolves them); for example
Book of Moonfire is `A1A`, Book of Healing Touch XI is `A7D`, Book of Cat Form is `9D3`.
