#pragma once

// Main-thread carriage publisher. False preserves Skyrim's ordinary dialogue.
namespace Taxi
{
    bool Open(RE::Actor* a_driver);
    void Tick();
    void Reset();
    void Command(std::string a_action, std::uint32_t a_destination,
        std::uint32_t a_session, int a_cost);
}
