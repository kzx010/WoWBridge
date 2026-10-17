#pragma once

#include <nlohmann/json.hpp>
#include <optional>

// The player's native biped clones, at the last rendered WoW pose. No replacement meshes or materials.
namespace PlayerWeapons
{
	void Tick();                         // main thread, after Avatar::Tick
	void Reset();                        // release clones before load, on disable, or when the player's body is shown
	bool KeepsVisible(const RE::BSGeometry* a_geometry);  // Surrogate's geometry-cull exception
	bool Enabled();
	bool FrontViewEnabled();             // self-test-only camera override switch
	std::optional<std::uint32_t> ReadyStance();  // native main hand mapped through wowcore::meleeAnims
	std::uint32_t NativeSlots();          // main thread: suppress legacy mapped-M2 proxies for native weapons
	bool Command(std::string_view a_arg, std::string_view a_value, std::string& a_message);
	void RecordReferenceReview(std::string_view a_kind, bool a_drawn, const nlohmann::json& a_native, const nlohmann::json& a_wow);
	nlohmann::json WowItems();           // attached WoW reference items from the last rendered attachment pose
	nlohmann::json Status();             // main thread; current clone and attachment positions
}
