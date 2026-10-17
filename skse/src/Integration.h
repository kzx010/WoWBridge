#pragma once
#include "IntegrationState.h"

namespace SKSE { class SerializationInterface; }

namespace Integration
{
    constexpr bool CanChooseClass(Mode mode, bool offered) { return mode != Mode::WoW || offered; }
    constexpr bool ClassBack(Mode mode, bool creator) { return creator && mode != Mode::WoW; }
    bool CanChooseClass(); // synchronized decision for render/input threads
    void UpdateClassChoice(); // main thread: publish after mode or customization-offer changes
    void Init();
    void Tick();
    bool Active(); // render/input threads: no engine or Lua access
    void NewGame();
    void PrepareLoad();
    void OnGameLoaded();
    bool Start(); // native popup, deferred until the console closes
    std::string PlayerName(); // main thread, Skyrim's name or Dragonborn
    constexpr std::uint32_t kRecord = 'MODE', kRecordVersion = 1;
    void Save(SKSE::SerializationInterface* intfc);
    void Load(SKSE::SerializationInterface* intfc, std::uint32_t length, std::uint32_t version);
}
