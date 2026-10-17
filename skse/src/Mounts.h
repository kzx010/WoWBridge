#pragma once
#include <nlohmann/json.hpp>
// Main-thread, epic ground mounts (active state is session-only). Skyrim's player controller
// retains collision/terrain/jumping; only its visible model and speed are changed.
namespace Mounts {
    void Init();
    void Tick();
    void Reset();
    bool Use(std::uint32_t spell);
    bool Select(const std::string& key, std::string& message);
    void Dismount();
    bool Pending();
    nlohmann::json Snapshot();
    // The mounts the player has learned, from their reins (Learning.cpp): only these are in their spellbook and summon. A new
    // game has none; a save that played before mounts were learned keeps every mount (Migrate). Keys, saved as JSON.
    constexpr std::uint32_t kRecord='MNTS', kRecordVersion=1;
    void Save(SKSE::SerializationInterface* a_intfc);
    void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length);
    void PrepareLoad();   // kPreLoadGame / kNewGame: none until a record or Migrate
    void Migrate(bool a_played);   // after the records: a save without one that had their kit keeps every mount
    bool Learned(std::string_view a_key);
    bool Learn(std::string_view a_key);   // false: no such mount or already learned; announced as the client does
    std::vector<std::string> LearnedKeys();
    void SetLearned(std::vector<std::string> a_keys);   // testing: every mount, a new game's none, or a saved set
    std::vector<std::string> Keys();   // the catalog's
}
