#pragma once
#include "IntegrationState.h"

namespace Integration
{
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
