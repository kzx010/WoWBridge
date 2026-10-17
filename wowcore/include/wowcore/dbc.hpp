// WDBC tables from the 1.12 archives, and the typed lookups the WoW API needs over them.
// Column indices are build 1.12.1.5875's, from WoWDBDefs (reference/WoWDBDefs/definitions/<Table>.dbd).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wowcore {
class Mpq;

class Dbc {
public:
    static std::optional<Dbc> load(const Mpq& mpq, std::string_view path);   // e.g. "DBFilesClient\\Spell.dbc"
    uint32_t rows() const { return rows_; }
    uint32_t cols() const { return cols_; }
    uint32_t u32(uint32_t row, uint32_t col) const;
    int32_t i32(uint32_t row, uint32_t col) const { return static_cast<int32_t>(u32(row, col)); }
    float f32(uint32_t row, uint32_t col) const;
    const char* str(uint32_t row, uint32_t col) const;                       // "" when out of range
    std::optional<uint32_t> rowOf(uint32_t id) const;

private:
    std::vector<uint8_t> data_;
    uint32_t rows_ = 0, cols_ = 0, recordSize_ = 0, stringsAt_ = 0;
    std::unordered_map<uint32_t, uint32_t> index_;
};

struct HelmetVisibility {
    uint32_t id = 0;
    bool hair = false, facialHair = false, ears = false;  // true means hidden
};
// ItemDisplayInfo.HelmetGeosetVisID[sex] -> HelmetGeosetVisData.HideGeoset[5], build 1.12.1.5875.
// The masks use the race ID as the bit index (Human = 1 << 1). No visibility ID means nothing hidden.
std::optional<HelmetVisibility> helmetVisibility(const Dbc& displays, const Dbc& visibility,
    uint32_t displayId, uint32_t race, uint32_t sex);

struct SpellEffect {
    uint32_t effect = 0, aura = 0, chainTargets = 0, periodMs = 0;
    int32_t basePoints = 0, dieSides = 0, baseDice = 0, misc = 0;
    float pointsPerLevel = 0, radius = 0, chainAmplitude = 1;
    float multiple = 0, comboPoints = 0;   // EffectMultipleValue ($e), EffectPointsPerComboPoint ($b)
};

struct SpellInfo {
    uint32_t id = 0;
    SpellEffect effects[3];
    uint32_t spellLevel = 0, baseLevel = 0, maxLevel = 0, attributesExB = 0, shapeshiftMask = 0, manaCostPct = 0;
    uint32_t procCharges = 0, procChance = 0;
    uint32_t stackAmount = 0, maxTargetLevel = 0, maxTargets = 0;   // $u, $v, $i
    std::vector<std::pair<uint32_t, uint32_t>> reagents;   // item, count
    std::string name, rank, description, auraDescription, icon;   // icon: full texture path
    uint32_t attributes = 0, attributesEx = 0, shapeshiftExclude = 0;
    uint32_t castTimeMs = 0, recoveryMs = 0, categoryRecoveryMs = 0, startRecoveryMs = 0;
    int32_t durationMs = 0;
    uint32_t powerType = 0, manaCost = 0, rangeIndex = 0;
    float rangeMin = 0, rangeMax = 0;
    float speed = 0;                                                  // yards a second a missile flies (0: none)
    uint32_t school = 0, dispelType = 0;
    bool targetsUnit = false;                                         // cast at a chosen unit (not self, area or ground)
    uint32_t implicitTarget = 0;                                      // the first effect's ImplicitTargetA
    bool passive() const { return attributes & 0x40; }               // SPELL_ATTR_PASSIVE
};

struct SkillLineInfo {
    uint32_t id = 0, category = 0;
    std::string name, icon;
};

// The client tables the API reads, loaded once.
// SoundEntries: a named sound, one of up to ten files picked by weight, from a directory, at a volume.
struct SoundInfo {
    uint32_t id = 0;
    std::vector<std::string> files;     // full MPQ paths
    std::vector<uint32_t> weights;
    float volume = 1.0f;
    float minDistance = 0.0f, cutoff = 0.0f;   // yards (positional sounds)
    bool loop = false;                         // Flags 0x200: precast and channel loops
};

// The sounds of a spell's visual (SpellVisual -> SpellVisualKit.SoundID): SoundEntries IDs, 0 for none; and the
// animations its kits play on the caster (SpellVisualKit.AnimID, AnimationData ids; -1 for none): ReadySpell* while
// casting, SpellCast* when it goes off, ChannelCast* while channelling.
struct SpellSounds {
    uint32_t precast = 0, cast = 0, impact = 0, state = 0, channel = 0, missile = 0;
    int32_t precastAnim = -1, castAnim = -1, channelAnim = -1;
};

enum class MeleeSoundSource { Unarmed, Weapon, Creature };
struct MeleeSounds {
    uint32_t swing = 0, impact = 0, exertion = 0;   // SoundEntries ids; zero means silence
    uint32_t customAttack[4] = {};                 // creature M2 $AH0..$AH3 overrides
    uint32_t creatureImpactIndex = 0;             // material when this creature is HIT, not its claws
};

// The models a spell's visual puts on: each kit's effects by slot (SpellVisualKit: 0 head, 1 chest, 2 base, 3 left
// hand, 4 right hand, 5 breath, 6-8 special, 9 world) with SpellVisualEffectName's file and scale, and the missile.
struct KitEffect {
    int slot = 0;
    std::string model;
    float scale = 1.0f;
};
struct SpellVisualKits {
    std::vector<KitEffect> precast, cast, impact, state, channel;
    std::string missile;
    uint32_t missileSound = 0;
    // The area effect (SpellVisual.AreaModel): a model stood where a ground spell lands, its size AreaEffectSize yards
    // (0: as made).
    std::string area;
    float areaScale = 1.0f, areaSize = 0.0f;
};

// The player's hardcoded level celebration (not Spell 24312, an NPC buff also named "Level Up").
// No SpellVisualKit references effect 21 in 1.12; the client attaches this effect at the unit's base directly.
struct LevelUpEffect {
    uint32_t effectId = 0;
    std::string model;                 // DBC path; loadModel accepts .mdl and .mdx as .m2
    uint32_t soundId = 0;              // SoundEntries LEVELUPSOUND; resolve with ClientDb::sound
};

// A text emote (EmotesText, by its slash token: "DANCE"): what the player sees in chat, alone or at a target, the animation
// it plays (Emotes) and the race/sex's voice (EmotesTextSound). specProc: 0 once, 1 a stand state (sit, sleep, kneel),
// 2 a loop until the player moves (dance).
struct EmoteText {
    uint32_t id = 0, anim = 0, specProc = 0, standState = 0, sound = 0;   // standState: 1 sit, 3 sleep, 8 kneel
    std::string alone, atTarget;   // "You burst into dance.", "You dance with %s."
};

class ClientDb {
public:
    explicit ClientDb(const Mpq& mpq);
    const SpellInfo* spell(uint32_t id) const;
    std::vector<uint32_t> skillLinesOf(uint32_t spellId) const;     // SkillLineAbility
    const SkillLineInfo* skillLine(uint32_t id) const;
    std::string iconPath(uint32_t spellIconId) const;
    std::string itemIcon(uint32_t displayId) const; // ItemDisplayInfo.InventoryIcon, column 5
    std::string dispelName(uint32_t dispelType) const;               // SpellDispelType: "Magic", "Curse", ...
    std::string formName(uint32_t form) const;                       // SpellShapeshiftForm: "Cat Form", ...
    const SoundInfo* sound(std::string_view name) const;             // SoundEntries by name, any case
    const SoundInfo* sound(uint32_t id) const;
    SpellSounds spellSounds(uint32_t spellId) const;
    SpellVisualKits spellVisualKits(uint32_t spellId) const;
    std::optional<LevelUpEffect> levelUpEffect() const;
    // Melee sounds (SoundEntries ids, 0: none). A swing's whoosh by size (WeaponSwingSounds2: 0 light, 1 medium, 2 heavy;
    // the critical one); what it hits by weapon (WeaponImpactSounds: item subclass, parry sound type - 0 wood, 1 metal)
    // and by what it hits: 0 flesh, 1 chain, 2 plate, 3 metal shield, 4 wooden shield, 5/6 a parry by metal/wood,
    // 7 wood, 8 stone, 9 ore.
    uint32_t weaponSwingSound(uint32_t swingType, bool crit) const;
    uint32_t weaponImpactSound(uint32_t subclass, uint32_t parryType, uint32_t index, bool crit) const;
    // id: item subclass for Weapon, CreatureDisplayInfo id for Creature, ignored for Unarmed.
    // parryType and impactIndex describe the weapon material and the surface being hit, respectively.
    // A form is Creature regardless of the equipped weapon. No match returns nullopt; no sound returns id 0.
    std::optional<MeleeSounds> meleeSounds(MeleeSoundSource source, uint32_t id = 0,
        uint32_t parryType = 0, uint32_t impactIndex = 0, bool crit = false) const;
    std::optional<EmoteText> emote(std::string_view token, uint32_t race, uint32_t sex) const;
    bool ok() const { return ok_; }

private:
    std::optional<Dbc> spell_, icon_, cast_, duration_, range_, skillAbility_, skillLine_, dispel_, radius_, forms_, sounds_,
        visuals_, kits_, effectNames_, swings_, impacts_, subclasses_, creatureDisplays_, creatureModels_, creatureSounds_, itemDisplays_,
        emotesText_, emotes_, emotesTextData_, emotesTextSound_;
    std::unordered_map<std::string, uint32_t> emoteTokens_;   // upper-case token -> EmotesText row
    std::unordered_map<uint32_t, SoundInfo> soundsById_;
    std::unordered_map<std::string, uint32_t> soundNames_;   // lower case -> id (the first of a name)
    mutable std::unordered_map<uint32_t, SpellInfo> spells_;
    mutable std::unordered_map<uint32_t, SkillLineInfo> lines_;
    std::unordered_multimap<uint32_t, uint32_t> spellLines_;
    bool ok_ = false;
};
}  // namespace wowcore
