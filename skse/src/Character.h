#pragma once

#include "../../wowcore/include/wowcore/character.h"

#include <nlohmann/json.hpp>

// The player's WoW character as the reference realm has the player (data/build_catalog.py writes it from the realm's
// characters and character_inventory: Data/SKSE/Plugins/WoWBridge/character.json): race, sex, looks, level and what
// the player wears. Whatever dresses the player, swings their weapon or tells the service what the player wears reads it here, so another
// character or other gear is another file, not other code. The catalog is loaded at kDataLoaded; Gear updates the
// worn state as their Skyrim equipment changes.
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
		std::uint32_t useItemCooldown = 0, useCategory = 0, useReadyCategory = 0, useCategoryCooldown = 0;   // Player::AddCooldown
		bool operator==(const Item&) const = default;
	};

	struct Sheet
	{
		std::string         name;
		std::uint32_t       level = 0;
		std::uint32_t       classId = 0;
		wowcore::Appearance looks;
		std::vector<Item>   equipment;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> actions;   // action slot (1-120), spell: their bar
	};

	struct EquipmentSet
	{
		std::string name;
		std::vector<std::uint32_t> entries;
		struct Bonus { std::uint32_t spell = 0, threshold = 0, ability = 0; };
		std::vector<Bonus> bonuses;
	};

	bool                       Load();   // false: no character file (logged); nothing dresses or equips the player then
	const Sheet&               Get();
	wowcore::Appearance        Looks();   // snapshot for background loaders
	// WoW: 0 male, 1 female. A change of sex takes that race/sex's packaged look; their
	// own sex keeps their look. Redress immediately.
	bool                       SetSex(std::uint32_t a_sex);
	// The customization window (wowcore/lua/wowapi_customize.lua; the interface's appearance command): Accept sets a
	// look their race and sex can have in 1.12's character creation (false: refused), Cancel keeps theirs. Either answers
	// the offer, which onboarding makes once WoWBridge is chosen and the save keeps until it is answered.
	bool                       SetLooks(const wowcore::Appearance& a_looks);
	// Class changes belong to onboarding; wb identity explicitly bypasses that policy for tests.
	bool                       SetIdentity(std::uint32_t a_class, const wowcore::Appearance& a_looks, bool a_testing = false);
	bool                       SetIdentity(std::uint32_t a_class, std::uint32_t a_race, std::uint32_t a_sex, bool a_testing = false);
	const std::vector<wowcore::CharacterClass>& Classes();
	void                       KeepLooks();
	void                       OfferCustomization(bool a_offer);
	bool                       CustomizationOffered();
	std::string                AppearanceLua();   // their look and each race/sex's packaged one: the interface's __WBAppearance
	void                       ResetSex();   // saves without a record and new games default to female
	void                       PrepareLoad();   // discard the previous save's pending selection
	void                       RestoreIdentity();   // logical class/race before Cast and Forms restore; no surrogate changes
	void                       OnGameLoaded();   // apply the record after Skyrim has restored its player base
	// Druids write v1 (sex) then v3 (full look); v4 adds class. v2 remains unsupported by the save loader.
	constexpr std::uint32_t kRecord = 'LOOK', kRecordVersion = 4;
	void Save(SKSE::SerializationInterface* a_intfc);
	void LoadLooks(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length, std::uint32_t a_version);
	// The WoW items made Skyrim items (gear.json, data/build_esp.py), by item entry.
	const std::map<std::uint32_t, Item>& Gear();
	const std::map<std::string, EquipmentSet>& Sets();   // gear.json console keys, client names and membership
	const std::map<std::uint32_t, std::uint32_t>& Reagents();   // what their spells consume: item entry -> local form id
	// What the player wears: their WoW items and mapped Skyrim looks (Gear). Until it is first set, the realm's
	// equipment (character.json). Everything below answers from it.
	std::uint32_t              SlotOf(std::uint32_t a_type);
	void                       SetWorn(std::vector<Item> a_items);
	std::optional<Item>        InSlot(std::uint32_t a_slot);
	std::vector<wowcore::Worn> Worn();     // what wowcore::dressCharacter dresses the player in
	// Show Helm / Show Cloak (Interface Options, the interface's showGear): a hidden head or back item is left out of
	// Worn, so off their model and paper doll; a change dresses the player again.
	void                       SetShown(bool a_helm, bool a_cloak);
	nlohmann::json             Caster();   // the service's caster: level, the items the player wears, their Skyrim skills
	void                       Train(std::string_view a_skill, float a_use);   // the service's skill name; Skyrim's skill use
	wowcore::MeleeAnims        Melee();    // their stance and swing for the main hand (bare claws in a form)
	void                       SetShapeshifted(bool a_shifted);
	std::string                ActionsLua();   // their bar as the interface's WB.actions
	bool                       HasActions();   // the realm's bar has entries (or a non-Druid's empty bar replaces it)
}
