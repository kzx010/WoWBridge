#pragma once

#include <nlohmann/json.hpp>

// Their WoW gear as Skyrim items (WoWBridge.esp and gear.json, made by data/build_esp.py; WoWBridgeArmory.esp's, Armory.h):
// their kit is given once, the player equips and unequips it in Skyrim's own inventory, and what the player has equipped is what the player
// wears - their model, their swing, the service's caster (Character::SetWorn) - with each item's intrinsic WoW stats as an
// equip ability and their sets' bonuses as theirs. Without the plugins loaded, enabled Skyrim look mappings can still
// dress the player from Skyrim's worn inventory.
namespace Gear
{
	constexpr std::uint32_t kRecord = 'GEAR', kRecordVersion = 1;   // whether their kit has been given

	void Init();           // kDataLoaded: find the plugin's items, listen for equips
	nlohmann::json Modifiers(); // live tempering, the main hand's native damage and native armour enchantments
	nlohmann::json WeaponDamages(); // each WoW weapon's native base damage by entry (gear.json skyrim, its ESP record)
	void EnchantHit(RE::Actor* a_target); // landed WoW main-hand swing
	bool UseEffect(RE::TESBoundObject* a_form, RE::ExtraDataList* a_extra); // main-thread item-use transformation
	void Reset();          // a new game: not given yet
	bool Given();          // their kit has been given: this save has played with WoWBridge
	void OnGameLoaded();   // kPostLoadGame / kNewGame: give the kit once, then read what the player wears
	void Tick();           // every frame: after an equip, what the player wears
	bool Ready();          // WoW items or enabled Skyrim look mappings were found
	std::uint32_t EntryOf(const RE::TESBoundObject* a_form);  // resolved form pointer, 0: ordinary Skyrim item
	RE::TESBoundObject* FormOf(std::uint32_t a_entry);         // a WoW item's Skyrim item, WoWBridge.esp's or the Armory's

	// Their reagents (Ironwood Seed, Wild Thornroot) in their bags, by WoW item entry: the service's caster.reagents and the
	// interface's item counts. A stack of each comes with their kit.
	std::map<std::uint32_t, int> ReagentCounts();
	void                         Consume(std::uint32_t a_entry, int a_count);

	void Save(SKSE::SerializationInterface* a_intfc);
	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length);
}
