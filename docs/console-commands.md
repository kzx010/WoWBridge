# WoWBridge console and test commands

Open Skyrim's console with `~`. Close it after each command so frame updates
and service replies can finish; reopen it to read the result. `wb` commands
always affect the player regardless of the selected reference, and log their
results to `WoWBridge.log`. Load a game with packaged character data before
using anything except `wb` / `wb help`.

## WoWBridge commands

Commands, skill names and set keys are case-insensitive. Numeric arguments are
whole numbers in the ranges shown. Implemented in [Console.cpp](../skse/src/Console.cpp).

| Command / arguments | Effect | Example |
| --- | --- | --- |
| `wb start` | Opens the native WoWBridge setup popup after the console closes. On a new game, waits for character creation; activation waits for movement/combat controls. Existing characters keep their Skyrim name. | `wb start` |
| `wb` | Toggles the testing panel; close the console to use it. The Social micromenu button opens the same panel. | `wb` |
| `wb cooldowns` | Refreshes WoW spell/item cooldowns and GCD; preserves active casts/auras and native power/shout timers. Also available on the testing panel's Spells page. | `wb cooldowns` |
| `wb help` | Lists commands and skill names. | `wb help` |
| `wb sex male` | Selects packaged male appearance; refreshes body, outfit, portrait and previews, sets `UnitSex` to 2 and the native player's male flag/voice. | `wb sex male` |
| `wb sex female` | Restores the original female appearance, `UnitSex` 3 and female flag/voice. Selection is saved per game. | `wb sex female` |
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
| `wb give ENTRY` (1–4294967295) | Gives one equipment item by decimal WoW entry from `gear.json`; needs its installed ESP form. Does not equip it. | `wb give 873` |
| `wb give set NAME` | Gives one of every piece in a named set; gives nothing if any form is missing. | `wb give set cenarion` |
| `wb sets` | Lists set keys, original names and entries. | `wb sets` |
| `wb magicka N` (0–1000000) | Sets and refills base native magicka, also used as WoW mana. Fortify gear can raise the actual pool. | `wb magicka 5000` |
| `wb reset` | Stops casts/channels/placement, clears WoW cooldowns/auras and their native effects, and leaves forms/stealth. Keeps unrelated Skyrim magic, spells, skills, items and sex. | `wb reset` |
| `wb talents reset` | Refunds spent talents, removes talent-granted spells and rebuilds the class book from skills (also discards retained ranks). | `wb talents reset` |
| `wb talentpoints N` (1–51) | Legacy helper: sets Skyrim level to N + 9, giving N total talent points minus those spent. Skips attribute choices. | `wb talentpoints 51` |
| `wb xp SKILL N` (0–1000000) | Grants native skill XP and normal level-up eligibility. | `wb xp destruction 1000` |
| `wb heal` | Refills health, magicka and stamina. | `wb heal` |
| `wb health N` / `wb stamina N` (1–1000000) | Sets and refills the native base pool. | `wb health 1000` |
| `wb equip ENTRY` | Gives the packaged WoW item if absent, then equips it. | `wb equip 873` |
| `wb nativeitem HEX COUNT` (1–100) | Gives an installed inventory item; rejects actors/world objects. | `wb nativeitem 0000000F 10` |
| `wb learnwow ID` | Learns one packaged non-passive class spell. | `wb learnwow 5176` |
| `wb learnskyrim HEX` | Learns an installed Skyrim spell. | `wb learnskyrim 00012FCD` |
| `wb damage N` / `wb speed N` (10–300) | Sets native weapon damage / movement base multiplier as a percent. | `wb damage 150` |
| `wb multipliers` | Restores those base multipliers to 100%; keeps active magical modifiers. | `wb multipliers` |
| `wb travel PLACE` | Travels to an allowed testing destination. | `wb travel riverwood` |
| `wb spawn TYPE COUNT` (1–5) | Spawns a wolf, chicken or AI-disabled 10000-HP dummy wolf nearby. | `wb spawn dummy 1` |
| `wb clearspawn` | Disables only actors spawned by these tools since loading the current game. | `wb clearspawn` |

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

Open with `wb` or the Social micromenu button. Eight pages cover Character,
Skills, Gear, Items, Spells, Travel, Spawns and Mounts. Opening, filtering, selecting
entries and changing pages do not change gameplay state. Click a named action
to apply it; the status line reports errors or completion and prevents duplicate
requests while a service operation is pending. Frames start locked and support
the standard right-click lock menu; Escape or the close button dismisses it.

Gear includes all packaged equipment and complete sets. Native item/spell fields
accept hexadecimal runtime form IDs; WoW fields accept decimal entries/spell IDs.
Travel destinations: Riverwood, Whiterun, Solitude, Windhelm, Riften, Markarth,
Dawnstar, Blackreach and QASmoke. Skill XP uses Skyrim's normal advancement;
direct level/skill values keep the existing distinction described above.
The panel previews learned forms only. Console form previews also work before
a form is learned: the active form supplies power and action-page state without
creating a learned shapeshift button.
Native damage percentage affects Skyrim weapon attacks, not WoW service damage.
Spawn ownership is cleared on loading another game; Clear spawned does not search
for or remove unrelated actors. These are intentional testing controls and their
changes persist if you save the game.

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
