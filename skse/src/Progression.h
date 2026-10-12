#pragma once

#include <nlohmann/json.hpp>

// Her class spells learned as her Skyrim skills rise (docs/class-progression.md): the service's known_spells answers
// what she knows at her skills; her book shows it (the ranks still to learn greyed, with the skill that teaches them),
// a newly learned rank is announced in the chat, takes its lower rank's places on her bars, and a newly learned spell
// goes to a free slot. Main thread.
namespace Progression
{
	void Tick();    // her skills read each second; a change asks the service again
	void Reset();   // a load or a new game: restore its record, if read; the next answer is published quietly
	constexpr std::uint32_t kRecord = 'PROG', kRecordVersion = 1;
	void Save(SKSE::SerializationInterface* a_intfc);
	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length);   // WoW IDs, not Skyrim forms; Reset restores them
	void Acquire(std::span<const int> a_spells);   // already earned elsewhere (item books/talents); publish with the next answer
	// The service owns talent validation; the allocation is kept separately from acquired spell ranks.
	void Augment(nlohmann::json& a_caster);
	void LearnTalent(int a_talent, int a_rank);
	void RefreshTalents();   // UI startup/ReloadUI asks even when level/allocation did not change
	constexpr std::uint32_t kTalentRecord = 'TLNT', kTalentRecordVersion = 1;
	void LoadTalents(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length);
	bool Ready();   // an answer has been published
	bool Knows(std::uint32_t a_spell);   // she knows it; a load's retained spells are known before the first answer
	void TestBook(bool a_all, std::function<void(bool, std::size_t, std::string)> a_done);   // all progression ranks / skills only
	void ResetTalents(std::function<void(bool, std::size_t, std::string)> a_done);   // every point back; talent spells unlearned
	nlohmann::json SnapshotForTesting();   // allocation and retained class ranks, before a temporary audit
	void RestoreForTesting(const nlohmann::json& a_saved);   // invalidate audit replies; republish quietly on Tick
	std::size_t Count();
}
