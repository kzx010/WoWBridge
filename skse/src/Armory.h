#pragma once

#include "Character.h"

#include <nlohmann/json.hpp>

// WoWBridgeArmory.esp: every other 1.12 item a player could own (data/build_armory.py), beside WoWBridge.esp's 95.
// armory.json is read once at data load: each item's form (by the plugin and its local ID, never a load index), what
// dresses the player in it, its equip ability (shared by every item with the same bonus, a copy for each worn at once), its
// set, its Skyrim level and quality; its tooltip row stays text until one of their stacks shows it. Without the plugin
// the Armory is empty and everything else works as before.
namespace Armory
{
	struct Item
	{
		std::uint32_t       entry = 0;
		RE::TESBoundObject* form = nullptr;
		Character::Item     look;            // what dresses the player and swings it, as Character::Gear's rows
		std::int32_t        ability = -1;    // its bonus (Abilities), -1: none
		std::uint32_t       set = 0;         // ItemSet.dbc
		std::uint16_t       level = 0;       // Skyrim's, as its leveled lists have it
		std::uint8_t        quality = 0;
		std::string         name;
	};
	struct Set
	{
		std::string                                 name;
		std::vector<std::uint32_t>                  entries;   // every member, those only WoW has too (its tooltip lists them)
		std::vector<std::string>                    names;
		std::vector<Character::EquipmentSet::Bonus> bonuses;   // ability: the Armory's local ID, 0 where Skyrim has none
		std::uint16_t                               level = 0;
		std::uint8_t                                quality = 0;
	};

	constexpr std::string_view kPlugin = "WoWBridgeArmory.esp";

	void Init();    // kDataLoaded (Gear::Init): read armory.json, resolve its forms
	bool Ready();   // the plugin is loaded and its items found
	const Item* Find(std::uint32_t a_entry);
	const Item* Find(const RE::TESBoundObject* a_form);
	const std::vector<Item>& Items();   // by entry
	const std::vector<std::vector<RE::SpellItem*>>& Abilities();   // each bonus's copies, one per item worn with it at once
	const std::map<std::uint32_t, Set>& Sets();
	nlohmann::json Row(std::uint32_t a_entry);   // their bags' tooltip data: wow, skyrim, set, quality, display (Bags)
	const nlohmann::json& Distribution();       // where its leveled lists join Skyrim's (Distribution)
	// Testing: an item of this quality at the highest Skyrim level up to this one (the lowest above where none is), at
	// random; the items whose name holds this text (or this entry).
	const Item* Random(std::uint32_t a_level, std::uint32_t a_quality);
	std::vector<const Item*> Search(std::string_view a_text, std::size_t a_limit);
}
