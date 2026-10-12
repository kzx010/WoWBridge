#pragma once

#include <nlohmann/json.hpp>

// Her WoW gear as Skyrim items (WoWBridge.esp and gear.json, made by data/build_esp.py): her kit is given once, she
// equips and unequips it in Skyrim's own inventory, and what she has equipped is what she wears - her model, her swing,
// the service's caster (Character::SetWorn) - with each item's intrinsic WoW stats as an equip ability. Without
// the plugin loaded, enabled Skyrim look mappings can still dress her from Skyrim's worn inventory.
namespace Gear
{
	constexpr std::uint32_t kRecord = 'GEAR', kRecordVersion = 1;   // whether her kit has been given

	void Init();           // kDataLoaded: find the plugin's items, listen for equips
	nlohmann::json Modifiers(); // live tempering and native armour enchantments
	void EnchantHit(RE::Actor* a_target); // landed WoW main-hand swing
	bool UseEffect(RE::TESBoundObject* a_form, RE::ExtraDataList* a_extra); // main-thread item-use transformation
	void Reset();          // a new game: not given yet
	void OnGameLoaded();   // kPostLoadGame / kNewGame: give the kit once, then read what she wears
	void Tick();           // every frame: after an equip, what she wears
	bool Ready();          // WoW items or enabled Skyrim look mappings were found
	std::uint32_t EntryOf(const RE::TESBoundObject* a_form);  // resolved form pointer, 0: ordinary Skyrim item

	// Her reagents (Ironwood Seed, Wild Thornroot) in her bags, by WoW item entry: the service's caster.reagents and the
	// interface's item counts. A stack of each comes with her kit.
	std::map<std::uint32_t, int> ReagentCounts();
	void                         Consume(std::uint32_t a_entry, int a_count);

	void Save(SKSE::SerializationInterface* a_intfc);
	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length);
}
