#pragma once
#include "wowcore/character.hpp"

#include <array>
#include <optional>

namespace wowcore {
struct CharacterClass {
    uint32_t id = 0;
    std::string name, token;
    std::vector<uint32_t> races;
};
// ChrClasses / ChrRaces / CharBaseInfo, restricted to the implemented classes.
std::vector<CharacterClass> characterClasses(const Mpq& mpq);
bool eligibleCharacter(uint32_t classId, uint32_t race);

// LOOK v4 appends class to v3. Druids precede v3 with v1 for sex-only readers.
struct CharacterIdentity {
    Appearance looks;
    bool offer = false;
    uint32_t classId = 11;
};
using IdentityData = std::array<uint32_t, 9>;
constexpr uint32_t identityVersion(uint32_t classId) { return classId == 11 ? 3 : 4; }
constexpr uint32_t identitySize(uint32_t version) {
    return version == 1 ? 4 : version == 3 ? 32 : version == 4 ? 36 : 0;
}
inline IdentityData writeIdentity(const CharacterIdentity& who) {
    const auto& l = who.looks;
    return {l.sex,l.skin,l.face,l.hairStyle,l.hairColor,l.facialHair,who.offer,l.race,who.classId};
}
template <class Write>
bool writeIdentityRecords(const CharacterIdentity& who, Write write) {
    const auto data = writeIdentity(who);
    bool ok = true;
    // Loaders visit records in order: legacy builds keep the sex; newer ones replace it with the full look.
    if (who.classId == 11) ok = write(1u, data, identitySize(1));
    const auto version = identityVersion(who.classId);
    return write(version, data, identitySize(version)) && ok;
}
inline std::optional<CharacterIdentity> readIdentity(uint32_t version, const IdentityData& data,
    const Appearance& male, const Appearance& female) {
    if (!identitySize(version) || data[0] > 1) return std::nullopt;
    CharacterIdentity who;
    if (version == 1) who.looks = data[0] ? female : male;
    else {
        who.looks = {version >= 3 ? data[7] : 4,data[0],data[1],data[2],data[3],data[4],data[5]};
        who.offer = data[6] != 0;
        who.classId = version == 4 ? data[8] : 11;
    }
    return eligibleCharacter(who.classId, who.looks.race) ? std::optional(who) : std::nullopt;
}
}
