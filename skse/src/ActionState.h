#pragma once
#include <cmath>
#include <cstdint>
#include <string>

namespace ActionState {
    // Native spells, shouts and powers use negative 32-bit FormIDs. WoW spells
    // use positive IDs. Lua numbers represent every ID in this range exactly.
    inline void Append(std::string& text, double slot, double spell) {
        constexpr double maxId=4294967295.0;
        if (!std::isfinite(slot) || slot<1 || slot>120 || std::trunc(slot)!=slot ||
            !std::isfinite(spell) || spell < -maxId || spell > maxId || std::trunc(spell)!=spell) return;
        text+=" ["+std::to_string(static_cast<int>(slot))+"] = { spell = "+
            std::to_string(static_cast<std::int64_t>(spell))+" },";
    }
}
