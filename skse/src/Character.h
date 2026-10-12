#pragma once

#include "wowcore/character.hpp"

#include <nlohmann/json.hpp>

// The player's WoW character as the reference realm has her (data/build_catalog.py writes it from the realm's
// characters and character_inventory: Data/SKSE/Plugins/WoWBridge/character.json): race, sex, looks, level and what
// she wears. Whatever dresses her, swings her weapon or tells the service what she wears reads it here, so another
// character or other gear is another file, not other code. The catalog is loaded at kDataLoaded; Gear updates the
// worn state as her Skyrim equipment changes.
namespace Character
{
	struct Item
	{
		std::uint32_t slot = 0;           // 0 head ... 15 main hand, 16 off hand, 17 ranged or relic
		std::uint32_t entry = 0;          // item_template; 0 for a Skyrim look (no WoW stats)
		std::uint32_t displayId = 0;
		std::uint32_t inventoryType = 0;
		std::uint32_t itemClass = 0;
		std::uint32_t subclass = 0;
		std::uint32_t sheath = 0;
		std::uint32_t material = 0;
		std::uint32_t delayMs = 0;        // a weapon's swing timer
		std::uint32_t form = 0;           // catalog: WoWBridge.esp local id; worn state: resolved Skyrim form id
		std::uint32_t ability = 0;        // a weapon's stats, an ability added while it is equipped (local id)
		std::uint32_t useSpell = 0, useEntry = 0, useCooldown = 0;
		bool operator==(const Item&) const = default;
	};

	struct Sheet
	{
		std::string         name;
		std::uint32_t       level = 0;
		std::uint32_t       classId = 0;
		wowcore::Appearance looks;
		std::vector<Item>   equipment;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> actions;   // action slot (1-120), spell: her bar
	};

	struct EquipmentSet
	{
		std::string name;
		std::vector<std::uint32_t> entries;
		struct Bonus { std::uint32_t spell = 0, threshold = 0, ability = 0; };
		std::vector<Bonus> bonuses;
	};

	bool                       Load();   // false: no character file (logged); nothing dresses or equips her then
	const Sheet&               Get();
	wowcore::Appearance        Looks();   // snapshot for background loaders
	bool                       SetSex(std::uint32_t a_sex);   // WoW: 0 male, 1 female; redress immediately
	void                       ResetSex();   // saves without a record and new games default to female
	void                       PrepareLoad();   // discard the previous save's pending selection
	void                       OnGameLoaded();   // apply the record after Skyrim has restored its player base
	constexpr std::uint32_t kRecord = 'LOOK', kRecordVersion = 1;
	void Save(SKSE::SerializationInterface* a_intfc);
	void LoadSex(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length);
	// The WoW items made Skyrim items (gear.json, data/build_esp.py), by item entry.
	const std::map<std::uint32_t, Item>& Gear();
	const std::map<std::string, EquipmentSet>& Sets();   // gear.json console keys, client names and membership
	const std::map<std::uint32_t, std::uint32_t>& Reagents();   // what her spells consume: item entry -> local form id
	// What she wears: her WoW items and mapped Skyrim looks (Gear). Until it is first set, the realm's
	// equipment (character.json). Everything below answers from it.
	std::uint32_t              SlotOf(std::uint32_t a_type);
	void                       SetWorn(std::vector<Item> a_items);
	std::optional<Item>        InSlot(std::uint32_t a_slot);
	std::vector<wowcore::Worn> Worn();     // what wowcore::dressCharacter dresses her in
	nlohmann::json             Caster();   // the service's caster: level, the items she wears, her Skyrim skills
	void                       Train(std::string_view a_skill, float a_use);   // the service's skill name; Skyrim's skill use
	wowcore::MeleeAnims        Melee();    // her stance and swing for the main hand (bare claws in a form)
	void                       SetShapeshifted(bool a_shifted);
	std::string                ActionsLua();   // her bar as the interface's WB.actions
}
