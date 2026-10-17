#pragma once

#include <nlohmann/json.hpp>

// Nearby NPCs answer their /emotes and greetings with a native idle and a glance, never a crime or a fight. Main thread.
namespace Reactions
{
    // a_target: their WoW target (may be null or the player). Returns how many NPCs reacted.
    int OnEmote(RE::PlayerCharacter* a_player, RE::Actor* a_target, const std::string& a_token);
    int OnChat(RE::PlayerCharacter* a_player, RE::Actor* a_target, const std::string& a_text);
    void Tick();   // releases the glances
    nlohmann::json Status();
}
