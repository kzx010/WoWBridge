#include "wowcore/dbc.hpp"

#include "wowcore/mpq.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace wowcore {
std::optional<HelmetVisibility> helmetVisibility(const Dbc& displays, const Dbc& visibility,
    uint32_t displayId, uint32_t race, uint32_t sex) {
    if (race < 1 || race > 8 || sex > 1 || displays.cols() < 14 || visibility.cols() != 6) return std::nullopt;
    const auto display = displays.rowOf(displayId);
    if (!display) return std::nullopt;
    HelmetVisibility out;
    out.id = displays.u32(*display, 12 + sex);
    if (!out.id) return out;
    const auto row = visibility.rowOf(out.id);
    if (!row) return std::nullopt;
    // WoWDBDefs via data/dbc.py: ID, HideGeoset[5] = hair, facial hair 1/2/3, ears.
    const uint32_t bit = 1u << race;
    out.hair = (visibility.u32(*row, 1) & bit) != 0;
    out.facialHair = ((visibility.u32(*row, 2) | visibility.u32(*row, 3) | visibility.u32(*row, 4)) & bit) != 0;
    out.ears = (visibility.u32(*row, 5) & bit) != 0;
    return out;
}

namespace {
uint32_t le32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

// Column indices, build 1.12.1.5875 (WoWDBDefs). A _lang column is 8 locale strings + a flags word; [0] is enUS.
namespace spell {
constexpr uint32_t Reagent = 42, ReagentCount = 50, School = 1, DispelType = 4, Attributes = 6, AttributesEx = 7, AttributesExB = 8, ShapeshiftMask = 11,
                   ProcChance = 25, ProcCharges = 26, MaxLevel = 27, BaseLevel = 28, SpellLevel = 29,
                   Effect = 61, EffectDieSides = 64, EffectBaseDice = 67, EffectRealPointsPerLevel = 73,
                   EffectBasePoints = 76, EffectRadiusIndex = 88, EffectAura = 91, EffectAuraPeriod = 94,
                   EffectChainTargets = 100, EffectMiscValue = 106, ManaCostPct = 156, EffectChainAmplitude = 167, CastingTimeIndex = 18,
                   RecoveryTime = 19, CategoryRecoveryTime = 20, DurationIndex = 30, PowerType = 31, ManaCost = 32,
                   RangeIndex = 36, Speed = 37, SpellVisual = 115, SpellIconID = 117, Name = 120, NameSubtext = 129, Description = 138,
                   AuraDescription = 147, StartRecoveryTime = 158, ImplicitTargetA = 82,
                   StackAmount = 39, EffectMultipleValue = 97, EffectPointsPerComboPoint = 112, MaxTargetLevel = 159,
                   MaxAffectedTargets = 163;   // CMaNGOS DBCStructure.h SpellEntry
}
namespace skillLineAbility { constexpr uint32_t SkillLine = 1, Spell = 2; }
namespace skillLine { constexpr uint32_t CategoryID = 1, DisplayName = 3, SpellIconID = 21; }
}  // namespace

std::optional<Dbc> Dbc::load(const Mpq& mpq, std::string_view path) {
    auto bytes = mpq.read(path);
    if (!bytes || bytes->size() < 20 || std::memcmp(bytes->data(), "WDBC", 4) != 0) return std::nullopt;
    Dbc d;
    d.data_ = std::move(*bytes);
    const uint8_t* h = d.data_.data();
    d.rows_ = le32(h + 4);
    d.cols_ = le32(h + 8);
    d.recordSize_ = le32(h + 12);
    d.stringsAt_ = 20 + d.rows_ * d.recordSize_;
    if (d.stringsAt_ + le32(h + 16) > d.data_.size() || d.recordSize_ < d.cols_ * 4) return std::nullopt;
    d.index_.reserve(d.rows_);
    for (uint32_t r = 0; r < d.rows_; ++r) d.index_.emplace(d.u32(r, 0), r);
    return d;
}

uint32_t Dbc::u32(uint32_t row, uint32_t col) const {
    if (row >= rows_ || col >= cols_) return 0;
    return le32(data_.data() + 20 + row * recordSize_ + col * 4);
}

float Dbc::f32(uint32_t row, uint32_t col) const {
    const uint32_t v = u32(row, col);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

const char* Dbc::str(uint32_t row, uint32_t col) const {
    const uint32_t off = u32(row, col);
    if (stringsAt_ + off >= data_.size()) return "";
    return reinterpret_cast<const char*>(data_.data() + stringsAt_ + off);
}

std::optional<uint32_t> Dbc::rowOf(uint32_t id) const {
    const auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    return it->second;
}

std::string ClientDb::itemIcon(uint32_t displayId) const {
    const auto row = itemDisplays_ ? itemDisplays_->rowOf(displayId) : std::nullopt;
    if (!row) return {};
    const std::string icon = itemDisplays_->str(*row, 5);
    return icon.empty() ? std::string() : "Interface\\Icons\\" + icon;
}

ClientDb::ClientDb(const Mpq& mpq) {
    auto t = [&](const char* name) { return Dbc::load(mpq, std::string("DBFilesClient\\") + name); };
    itemDisplays_ = t("ItemDisplayInfo.dbc");
    spell_ = t("Spell.dbc");
    icon_ = t("SpellIcon.dbc");
    cast_ = t("SpellCastTimes.dbc");
    duration_ = t("SpellDuration.dbc");
    range_ = t("SpellRange.dbc");
    skillAbility_ = t("SkillLineAbility.dbc");
    skillLine_ = t("SkillLine.dbc");
    dispel_ = t("SpellDispelType.dbc");
    radius_ = t("SpellRadius.dbc");
    forms_ = t("SpellShapeshiftForm.dbc");
    sounds_ = t("SoundEntries.dbc");
    visuals_ = t("SpellVisual.dbc");
    kits_ = t("SpellVisualKit.dbc");
    effectNames_ = t("SpellVisualEffectName.dbc");
    swings_ = t("WeaponSwingSounds2.dbc");
    impacts_ = t("WeaponImpactSounds.dbc");
    subclasses_ = t("ItemSubClass.dbc");
    creatureDisplays_ = t("CreatureDisplayInfo.dbc");
    creatureModels_ = t("CreatureModelData.dbc");
    creatureSounds_ = t("CreatureSoundData.dbc");
    emotesText_ = t("EmotesText.dbc");
    emotes_ = t("Emotes.dbc");
    emotesTextData_ = t("EmotesTextData.dbc");
    emotesTextSound_ = t("EmotesTextSound.dbc");
    if (emotesText_) for (uint32_t r = 0; r < emotesText_->rows(); ++r) {
        std::string token = emotesText_->str(r, 1);
        for (auto& c : token) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        emoteTokens_.emplace(token, r);
    }
    if (sounds_) {
        // ID, SoundType, Name, File[10], Freq[10], DirectoryBase, Volume, Flags, MinDistance, DistanceCutoff, EAXDef
        for (uint32_t r = 0; r < sounds_->rows(); ++r) {
            SoundInfo info;
            info.id = sounds_->u32(r, 0);
            const std::string dir = sounds_->str(r, 23);
            for (uint32_t f = 0; f < 10; ++f) {
                const std::string file = sounds_->str(r, 3 + f);
                if (!file.empty()) {
                    info.files.push_back(dir.empty() ? file : dir + "\\" + file);
                    info.weights.push_back(std::max<uint32_t>(sounds_->u32(r, 13 + f), 1));
                }
            }
            info.volume = sounds_->f32(r, 24);
            info.minDistance = sounds_->f32(r, 26);
            info.cutoff = sounds_->f32(r, 27);
            info.loop = (sounds_->u32(r, 25) & 0x200) != 0;
            std::string name = sounds_->str(r, 2);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            soundNames_.emplace(std::move(name), info.id);
            soundsById_.emplace(info.id, std::move(info));
        }
    }
    ok_ = spell_ && icon_ && cast_ && duration_ && range_ && skillAbility_ && skillLine_ && dispel_ && radius_ && forms_;
    if (skillAbility_)
        for (uint32_t r = 0; r < skillAbility_->rows(); ++r)
            spellLines_.emplace(skillAbility_->u32(r, skillLineAbility::Spell), skillAbility_->u32(r, skillLineAbility::SkillLine));
}

const SoundInfo* ClientDb::sound(uint32_t id) const {
    const auto it = soundsById_.find(id);
    return it == soundsById_.end() ? nullptr : &it->second;
}

const SoundInfo* ClientDb::sound(std::string_view name) const {
    std::string key(name);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto it = soundNames_.find(key);
    return it == soundNames_.end() ? nullptr : sound(it->second);
}

std::optional<EmoteText> ClientDb::emote(std::string_view token, uint32_t race, uint32_t sex) const {
    std::string key(token);
    for (auto& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto it = emoteTokens_.find(key);
    if (it == emoteTokens_.end()) return std::nullopt;
    // EmotesText: ID, Name, EmoteID, EmoteText[16] (EmotesTextData ids; 2: theirs at a target, 6: theirs alone).
    // Emotes: ID, EmoteSlashCommand, AnimID, EmoteFlags, EmoteSpecProc, ... EmotesTextSound: ID, text, race, sex, sound.
    EmoteText out;
    out.id = emotesText_->u32(it->second, 0);
    const auto text = [&](uint32_t index) {
        const auto row = emotesTextData_ ? emotesTextData_->rowOf(emotesText_->u32(it->second, 3 + index)) : std::nullopt;
        return row ? std::string(emotesTextData_->str(*row, 1)) : std::string();
    };
    out.atTarget = text(2);
    out.alone = text(6);
    if (const auto row = emotes_ ? emotes_->rowOf(emotesText_->u32(it->second, 2)) : std::nullopt) {
        out.anim = emotes_->u32(*row, 2);
        out.specProc = emotes_->u32(*row, 4);
        out.standState = emotes_->u32(*row, 5);
    }
    if (emotesTextSound_) for (uint32_t r = 0; r < emotesTextSound_->rows(); ++r) {
        if (emotesTextSound_->u32(r, 1) == out.id && emotesTextSound_->u32(r, 2) == race && emotesTextSound_->u32(r, 3) == sex) {
            out.sound = emotesTextSound_->u32(r, 4);
            break;
        }
    }
    return out;
}

std::optional<LevelUpEffect> ClientDb::levelUpEffect() const {
    // SpellVisualEffectName 21: "HARDCODED Unit Level Up". This celebration bypasses SpellVisual/Kit.
    const auto effect = effectNames_ ? effectNames_->rowOf(21) : std::nullopt;
    const auto* audio = sound("LEVELUPSOUND");
    if (!effect || !*effectNames_->str(*effect, 2) || !audio || audio->files.empty()) return std::nullopt;
    return LevelUpEffect{21, effectNames_->str(*effect, 2), audio->id};
}

SpellSounds ClientDb::spellSounds(uint32_t spellId) const {
    SpellSounds out;
    const auto sr = spell_ ? spell_->rowOf(spellId) : std::nullopt;
    const auto vr = sr && visuals_ ? visuals_->rowOf(spell_->u32(*sr, spell::SpellVisual)) : std::nullopt;
    if (!vr) return out;
    // SpellVisual: ID, PrecastKit, CastKit, ImpactKit, StateKit, ChannelKit, ... MissileSound (10); a kit's SoundID is 13.
    auto kitSound = [&](uint32_t col) -> uint32_t {
        const auto kr = kits_ ? kits_->rowOf(visuals_->u32(*vr, col)) : std::nullopt;
        return kr ? kits_->u32(*kr, 13) : 0;
    };
    out.precast = kitSound(1);
    out.cast = kitSound(2);
    out.impact = kitSound(3);
    out.state = kitSound(4);
    out.channel = kitSound(5);
    out.missile = visuals_->u32(*vr, 10);
    auto kitAnim = [&](uint32_t col) -> int32_t {
        const auto kr = kits_ ? kits_->rowOf(visuals_->u32(*vr, col)) : std::nullopt;
        return kr ? kits_->i32(*kr, 2) : -1;
    };
    out.precastAnim = kitAnim(1);
    out.castAnim = kitAnim(2);
    out.channelAnim = kitAnim(5);
    return out;
}

SpellVisualKits ClientDb::spellVisualKits(uint32_t spellId) const {
    SpellVisualKits out;
    const auto sr = spell_ ? spell_->rowOf(spellId) : std::nullopt;
    const auto vr = sr && visuals_ ? visuals_->rowOf(spell_->u32(*sr, spell::SpellVisual)) : std::nullopt;
    if (!vr || !kits_ || !effectNames_) return out;
    // SpellVisualEffectName: ID, Name, FileName, AreaEffectSize, Scale.
    auto effect = [&](uint32_t id, int slot, std::vector<KitEffect>& list) {
        const auto er = id ? effectNames_->rowOf(id) : std::nullopt;
        if (er && *effectNames_->str(*er, 2)) list.push_back({ slot, effectNames_->str(*er, 2), effectNames_->f32(*er, 4) > 0 ? effectNames_->f32(*er, 4) : 1.0f });
    };
    // SpellVisualKit: ID, KitType, AnimID, Head, Chest, Base, LeftHand, RightHand, Breath, Special[3], World, SoundID, ...
    auto kit = [&](uint32_t col, std::vector<KitEffect>& list) {
        const auto kr = kits_->rowOf(visuals_->u32(*vr, col));
        if (!kr) return;
        for (int slot = 0; slot < 10; ++slot) effect(kits_->u32(*kr, 3 + slot), slot, list);
    };
    kit(1, out.precast);
    kit(2, out.cast);
    kit(3, out.impact);
    kit(4, out.state);
    kit(5, out.channel);
    // SpellVisual: ..., HasMissile (6), MissileModel (7), ..., MissileSound (10).
    std::vector<KitEffect> missile;
    effect(visuals_->u32(*vr, 7), 0, missile);
    if (!missile.empty()) out.missile = missile[0].model;
    out.missileSound = visuals_->u32(*vr, 10);
    // HasAreaEffect (11), AreaModel (12).
    if (const auto ar = visuals_->u32(*vr, 11) && effectNames_ ? effectNames_->rowOf(visuals_->u32(*vr, 12)) : std::nullopt; ar && *effectNames_->str(*ar, 2)) {
        out.area = effectNames_->str(*ar, 2);
        out.areaSize = effectNames_->f32(*ar, 3);
        out.areaScale = effectNames_->f32(*ar, 4) > 0 ? effectNames_->f32(*ar, 4) : 1.0f;
    }
    return out;
}

uint32_t ClientDb::weaponSwingSound(uint32_t swingType, bool crit) const {
    // ID, SwingType, Crit, SoundID
    for (uint32_t r = 0; swings_ && r < swings_->rows(); ++r)
        if (swings_->u32(r, 1) == swingType && (swings_->u32(r, 2) != 0) == crit) return swings_->u32(r, 3);
    return 0;
}

uint32_t ClientDb::weaponImpactSound(uint32_t subclass, uint32_t parryType, uint32_t index, bool crit) const {
    // ID, WeaponSubClassID, ParrySoundType, ImpactSoundID[10], CritImpactSoundID[10]
    if (index >= 10) return 0;
    for (uint32_t r = 0; impacts_ && r < impacts_->rows(); ++r)
        if (impacts_->u32(r, 1) == subclass && impacts_->u32(r, 2) == parryType) return impacts_->u32(r, (crit ? 13 : 3) + index);
    return 0;
}

std::optional<MeleeSounds> ClientDb::meleeSounds(MeleeSoundSource source, uint32_t id,
        uint32_t parryType, uint32_t impactIndex, bool crit) const {
    if (parryType > 1 || impactIndex >= 10 || !subclasses_ || !swings_ || !impacts_) return std::nullopt;
    if (source != MeleeSoundSource::Unarmed && source != MeleeSoundSource::Weapon &&
        source != MeleeSoundSource::Creature) return std::nullopt;
    // ItemSubClass has a composite (ClassID, SubClassID) key; Dbc::rowOf cannot be used here.
    // The client selects its default unarmed subclass with Flags & 4 (1.12: fist weapons, subclass 13).
    std::optional<uint32_t> subclassRow;
    for (uint32_t r = 0; r < subclasses_->rows(); ++r) {
        if (subclasses_->u32(r, 0) != 2) continue;
        if (source == MeleeSoundSource::Weapon ? subclasses_->u32(r, 1) == id : (subclasses_->u32(r, 4) & 4) != 0) {
            subclassRow = r;
            break;
        }
    }
    if (!subclassRow) return std::nullopt;
    MeleeSounds out;
    // ItemSubClass.WeaponSwingSize -> WeaponSwingSounds2; the unarmed class has size 0 (light).
    out.swing = weaponSwingSound(subclasses_->u32(*subclassRow, 9), crit);
    out.impact = weaponImpactSound(subclasses_->u32(*subclassRow, 1),
        source == MeleeSoundSource::Weapon ? parryType : 0, impactIndex, crit);
    if (source != MeleeSoundSource::Creature) return out;
    const auto display = creatureDisplays_ ? creatureDisplays_->rowOf(id) : std::nullopt;
    if (!display) return std::nullopt;
    uint32_t soundId = creatureDisplays_->u32(*display, 2); // display override before model default
    if (!soundId) {
        const auto model = creatureModels_ ? creatureModels_->rowOf(creatureDisplays_->u32(*display, 1)) : std::nullopt;
        if (!model) return std::nullopt;
        soundId = creatureModels_->u32(*model, 13);
    }
    if (!soundId) return out; // a valid silent display
    const auto soundRow = creatureSounds_ ? creatureSounds_->rowOf(soundId) : std::nullopt;
    if (!soundRow) return std::nullopt;
    // Exertion is separate from $CSS / whoosh. A zero critical vocal stays silent, as in the client.
    out.exertion = creatureSounds_->u32(*soundRow, crit ? 2 : 1);
    for (uint32_t i = 0; i < 4; ++i) out.customAttack[i] = creatureSounds_->u32(*soundRow, 18 + i);
    // CreatureImpactType is the creature's receiving surface: normal, stone, wood, ore. It is NOT a weapon subclass.
    constexpr uint32_t surfaces[] = {0, 8, 7, 9}; // client 1.12.1: WoW.exe 0x6238f0, table 0x80db94
    const auto type = creatureSounds_->u32(*soundRow, 24);
    if (type < 4) out.creatureImpactIndex = surfaces[type];
    return out;
}

std::string ClientDb::iconPath(uint32_t spellIconId) const {
    if (!icon_) return {};
    const auto r = icon_->rowOf(spellIconId);
    return r ? icon_->str(*r, 1) : std::string();
}

std::string ClientDb::formName(uint32_t form) const {
    if (!forms_) return {};
    const auto r = forms_->rowOf(form);
    return r ? forms_->str(*r, 2) : std::string();   // ID, BonusActionBar, Name_lang
}

std::string ClientDb::dispelName(uint32_t dispelType) const {
    if (!dispel_ || dispelType == 0) return {};
    const auto r = dispel_->rowOf(dispelType);
    return r ? dispel_->str(*r, 1) : std::string();
}

const SpellInfo* ClientDb::spell(uint32_t id) const {
    if (const auto it = spells_.find(id); it != spells_.end()) return &it->second;
    if (!spell_) return nullptr;
    const auto r = spell_->rowOf(id);
    if (!r) return nullptr;
    const Dbc& s = *spell_;
    SpellInfo i;
    i.id = id;
    i.name = s.str(*r, spell::Name);
    i.rank = s.str(*r, spell::NameSubtext);
    i.description = s.str(*r, spell::Description);
    i.auraDescription = s.str(*r, spell::AuraDescription);
    i.icon = iconPath(s.u32(*r, spell::SpellIconID));
    i.speed = s.f32(*r, spell::Speed);
    i.attributes = s.u32(*r, spell::Attributes);
    i.attributesEx = s.u32(*r, spell::AttributesEx);
    i.school = s.u32(*r, spell::School);
    i.dispelType = s.u32(*r, spell::DispelType);
    i.recoveryMs = s.u32(*r, spell::RecoveryTime);
    i.categoryRecoveryMs = s.u32(*r, spell::CategoryRecoveryTime);
    i.startRecoveryMs = s.u32(*r, spell::StartRecoveryTime);
    i.powerType = s.u32(*r, spell::PowerType);
    i.manaCost = s.u32(*r, spell::ManaCost);
    i.rangeIndex = s.u32(*r, spell::RangeIndex);
    i.attributesExB = s.u32(*r, spell::AttributesExB);
    i.shapeshiftMask = s.u32(*r, spell::ShapeshiftMask);
    i.shapeshiftExclude = s.u32(*r, spell::ShapeshiftMask + 1);
    i.procChance = s.u32(*r, spell::ProcChance);
    i.procCharges = s.u32(*r, spell::ProcCharges);
    i.maxLevel = s.u32(*r, spell::MaxLevel);
    i.baseLevel = s.u32(*r, spell::BaseLevel);
    i.spellLevel = s.u32(*r, spell::SpellLevel);
    i.manaCostPct = s.u32(*r, spell::ManaCostPct);
    i.stackAmount = s.u32(*r, spell::StackAmount);
    i.maxTargetLevel = s.u32(*r, spell::MaxTargetLevel);
    i.maxTargets = s.u32(*r, spell::MaxAffectedTargets);
    for (uint32_t e = 0; e < 3; ++e) {
        SpellEffect& f = i.effects[e];
        f.effect = s.u32(*r, spell::Effect + e);
        f.aura = s.u32(*r, spell::EffectAura + e);
        f.basePoints = s.i32(*r, spell::EffectBasePoints + e);
        f.dieSides = s.i32(*r, spell::EffectDieSides + e);
        f.baseDice = s.i32(*r, spell::EffectBaseDice + e);
        f.pointsPerLevel = s.f32(*r, spell::EffectRealPointsPerLevel + e);
        f.periodMs = s.u32(*r, spell::EffectAuraPeriod + e);
        f.chainTargets = s.u32(*r, spell::EffectChainTargets + e);
        f.chainAmplitude = s.f32(*r, spell::EffectChainAmplitude + e);
        f.misc = s.i32(*r, spell::EffectMiscValue + e);
        f.multiple = s.f32(*r, spell::EffectMultipleValue + e);
        f.comboPoints = s.f32(*r, spell::EffectPointsPerComboPoint + e);
        if (const auto g = radius_->rowOf(s.u32(*r, spell::EffectRadiusIndex + e))) f.radius = radius_->f32(*g, 1);
    }
    for (uint32_t k = 0; k < 8; ++k)
        if (const uint32_t item = s.u32(*r, spell::Reagent + k)) i.reagents.push_back({item, s.u32(*r, spell::ReagentCount + k)});
    i.implicitTarget = s.u32(*r, spell::ImplicitTargetA);
    for (uint32_t e = 0; e < 3; ++e)   // single enemy, pet, any unit, single friend, party member, friend
        switch (s.u32(*r, spell::ImplicitTargetA + e)) { case 5: case 6: case 21: case 25: case 35: case 57: i.targetsUnit = true; }
    if (const auto c = cast_->rowOf(s.u32(*r, spell::CastingTimeIndex))) i.castTimeMs = cast_->u32(*c, 1);
    if (const auto d = duration_->rowOf(s.u32(*r, spell::DurationIndex))) i.durationMs = duration_->i32(*d, 1);
    if (const auto g = range_->rowOf(i.rangeIndex)) { i.rangeMin = range_->f32(*g, 1); i.rangeMax = range_->f32(*g, 2); }
    return &spells_.emplace(id, std::move(i)).first->second;
}

std::vector<uint32_t> ClientDb::skillLinesOf(uint32_t spellId) const {
    std::vector<uint32_t> out;
    for (auto [it, end] = spellLines_.equal_range(spellId); it != end; ++it) out.push_back(it->second);
    return out;
}

const SkillLineInfo* ClientDb::skillLine(uint32_t id) const {
    if (const auto it = lines_.find(id); it != lines_.end()) return &it->second;
    if (!skillLine_) return nullptr;
    const auto r = skillLine_->rowOf(id);
    if (!r) return nullptr;
    SkillLineInfo i{id, skillLine_->u32(*r, skillLine::CategoryID), skillLine_->str(*r, skillLine::DisplayName),
                    iconPath(skillLine_->u32(*r, skillLine::SpellIconID))};
    return &lines_.emplace(id, std::move(i)).first->second;
}
}  // namespace wowcore
