#pragma once
// Main-thread names for native compass markers; never decides HUD eligibility.
namespace CompassLocations {
    struct Location { RE::FormID id; std::string name; RE::NiPoint3 position; int type; };
    std::vector<Location> Snapshot(RE::PlayerCharacter* player);
    void Reset();
}
