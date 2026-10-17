#pragma once

#include "wowcore/character.hpp"

#include <array>
#include <cstdint>
#include <optional>

// Character's co-save LOOK record, shared with the portable tests. v3 adds race after the v2 fields: sex, skin colour, face, hair style, hair colour,
// facial hair, customization still owed (0/1). v1 kept the sex alone, which loads that sex's packaged look.
namespace LookRecord
{
	using Data = std::array<std::uint32_t, 8>;
	constexpr std::uint32_t Size(std::uint32_t a_version) { return a_version == 1 ? 4 : a_version == 2 ? 28 : a_version == 3 ? sizeof(Data) : 0; }
	struct Record
	{
		wowcore::Appearance looks;
		bool                offer = false;
	};
	inline Data Write(const Record& a_record)
	{
		const auto& l = a_record.looks;
		return { l.sex, l.skin, l.face, l.hairStyle, l.hairColor, l.facialHair, a_record.offer, l.race };
	}
	// What a record read as a_version says, for their race and each sex's packaged look; nothing for an unknown version
	// or a v1 sex beyond female. Whether the look is one the player can have is the caller's to ask.
	inline std::optional<Record> Read(std::uint32_t a_version, const Data& a_data, std::uint32_t a_race,
		const wowcore::Appearance& a_male, const wowcore::Appearance& a_female)
	{
		if (a_version == 1) {
			if (a_data[0] > 1) return std::nullopt;
			return Record{ a_data[0] ? a_female : a_male };
		}
		if (a_version != 2 && a_version != 3) return std::nullopt;
		return Record{ { a_version == 3 ? a_data[7] : a_race, a_data[0], a_data[1], a_data[2], a_data[3], a_data[4], a_data[5] }, a_data[6] != 0 };
	}
}
