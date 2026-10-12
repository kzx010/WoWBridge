#pragma once

namespace Hooks
{
	// Installs all engine hooks; each call-site patch is verified (opcode + expected target) first.
	bool Install();

	// How many times Skyrim asked for a camera shake (none is played: WoW's camera never shakes).
	std::uint32_t Shakes();

	// True if `a_site` holds `call rel32` whose target is `a_expected` (0 = any target).
	bool VerifyCall(std::uintptr_t a_site, std::uintptr_t a_expected, std::string_view a_name);
}
