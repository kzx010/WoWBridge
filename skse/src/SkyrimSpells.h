#pragma once

// The Skyrim spells she has learned (tomes, trainers; a new game's Flames and Healing) in her WoW spellbook's "Skyrim"
// tab, each as the spell -(its form id), draggable to her bars. Cast from there as WoW casts: a cast bar for its charge
// time (a concentration spell is channelled), then Skyrim's own spell goes off - its magicka, its effects, its skill.
// Her shouts and powers use Skyrim's voice control and native availability, with their own book lists and cooldowns.
namespace SkyrimSpells
{
	void Tick();    // her learned spells into her book as they change; a cast or channel in progress
	void Reset();   // a load or a new game
	bool Use(std::int64_t a_id);   // a book or bar spell below 0 is hers to cast (true); anything else is not
	bool UseByName(std::string_view a_name);
	bool Casting();
	void Stop();    // interrupt the native caster as well as the client cast/channel
	// Last successful greater-power uses, in Skyrim hours; Skyrim's own cast-power list enforces availability.
	constexpr std::uint32_t kRecord = 'SKPW', kRecordVersion = 1;
	void Save(SKSE::SerializationInterface* a_intfc);
	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length);
}
