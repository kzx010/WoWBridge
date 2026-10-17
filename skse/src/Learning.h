#pragma once

// The items that teach (gear.json books and mounts, data/build_learnables.py): a WoW spell book teaches its druid rank
// (Progression: early, before their skill reaches it, or a rank only a book teaches), a mount's reins teach the mount
// (Mounts). Using one from the WoW bags or reading it in Skyrim's inventory learns it and uses it up; otherwise the
// client's own refusal (wrong class, too low a level, the previous rank missing, already known). Main thread.
namespace Learning
{
	void Init();   // kDataLoaded: the plugin's book and mount item forms; listens for Skyrim's Book Menu
	std::uint32_t EntryOf(const RE::TESBoundObject* a_form);   // its WoW item entry; 0: not a book or mount item
	bool Use(RE::TESBoundObject* a_form, RE::ExtraDataList* a_extra = nullptr);   // false: not one of them
	RE::TESBoundObject* Book(std::uint32_t a_spell);   // the book teaching this rank, if any
	RE::TESBoundObject* MountItem(std::string_view a_key);   // the reins (whistle, horn) teaching this mount
	std::vector<std::uint32_t> Books();   // every rank that has a book
	std::vector<std::string> MountKeys();
}
