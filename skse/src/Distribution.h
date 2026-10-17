#pragma once

// Where their books, mount items and gear are found and sold (gear.json's distribution, data/build_esp.py; armory.json's,
// data/build_armory.py): at data load each of the plugins' leveled lists joins Skyrim.esm's lists (spell tome treasure,
// court wizards, the College; chests' magic loot, boss chests, humanoid and dragon death items) and vendor factions'
// merchant chests (Solitude's stables, the Khajiit caravans; blacksmiths, Radiant Raiment, the fletcher), as one entry or
// a few (weighting the pick), gated by their level inside the list. The lists and chests are the loaded, winning forms (a
// vendor's chest found through its faction, so other plugins' changes hold), appended to and never replaced; one an NPC
// carries or wears from with the plugins loaded is left out, a vendor faction without a chest is skipped (logged). A
// vendor's stock shows them from its next restock. Main thread.
namespace Distribution
{
	void Init();   // kDataLoaded, once a session: never twice into a list or chest
	RE::TESLevItem* List(std::string_view a_name);   // a plugin list by its editor ID (WoWSpellBooks ...): SelfTest
	// What one roll of a list at this level gives, its lists within rolled in turn (testing: wb lootroll, SelfTest).
	std::vector<RE::TESForm*> Roll(RE::TESLevItem* a_list, std::uint16_t a_level);
	// Their class's spell books roll, the other classes' do not: each class's book lists take their chance none from a
	// global (gear.json class_book_globals), 0 for their class and 100 for the rest. A load or a new character sets them.
	void SetClass(std::uint32_t a_class);
}
