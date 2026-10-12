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
}
