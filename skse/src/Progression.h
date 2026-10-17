#pragma once

#include <nlohmann/json.hpp>

// Their class spells learned as their Skyrim skills rise (docs/class-progression.md): the service's known_spells answers
// what the player knows at their skills; their book shows it (the ranks still to learn greyed, with the skill that teaches them),
// a newly learned rank is announced in the chat, takes its lower rank's places on their bars, and a newly learned spell
// goes to a free slot. Main thread.
namespace Progression
{
	void Tick();    // their skills read each second; a change asks the service again
	void Reset();   // a load or a new game: restore its record, if read; the next answer is published quietly
	constexpr std::uint32_t kRecord = 'PROG', kRecordVersion = 1;
	void Save(SKSE::SerializationInterface* a_intfc);
	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length);   // WoW IDs, not Skyrim forms; Reset restores them
	void Acquire(std::span<const int> a_spells);   // already earned elsewhere (item books/talents); publish with the next answer
	// A spell book read (Learning): its rank is theirs from the next answer, announced as learned and placed as any new
	// spell; kept apart from what their skills taught, so a talent wipe keeps it. False: known or already being learned.
	bool LearnBook(std::uint32_t a_spell);
	bool Taught(std::uint32_t a_spell);   // known, or a book's rank the next answer brings
	constexpr std::uint32_t kBookRecord = 'PBOK', kBookRecordVersion = 1;   // their class, the ranks their books taught
	void LoadBooks(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length);
	// The service owns talent validation; the allocation is kept separately from acquired spell ranks.
	void Augment(nlohmann::json& a_caster);
	void LearnTalent(int a_talent, int a_rank);
	void RefreshTalents();   // UI startup/ReloadUI asks even when level/allocation did not change
	constexpr std::uint32_t kTalentRecord = 'TLNT', kTalentRecordVersion = 1;   // JSON; its wipe price fields are optional
	void LoadTalents(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length);
	// A Shrine of Kynareth is their talent master: praying at one with points spent offers 1.12's talent wipe at its price
	// (docs/talents.md). Init watches activations; the interface's accept pays and unlearns.
	void Init();
	void ConfirmTalentWipe(std::uint32_t a_session, int a_cost);
	bool Ready();   // an answer has been published
	bool Knows(std::uint32_t a_spell);   // the player knows it; a load's retained spells are known before the first answer
	// All progression ranks / skills only (their books' ranks forgotten too: a new character's book).
	void TestBook(bool a_all, std::function<void(bool, std::size_t, std::string)> a_done);
	void ResetTalents(std::function<void(bool, std::size_t, std::string)> a_done);   // every point back; talent spells unlearned
	nlohmann::json SnapshotForTesting();   // allocation, retained class ranks, book ranks and wipe price, before a temporary audit
	void RestoreForTesting(const nlohmann::json& a_saved);   // invalidate audit replies; republish quietly on Tick
	std::size_t Count();
}
