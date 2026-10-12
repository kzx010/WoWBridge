// WoWBridge compat: the coin split WoWee's ui_colors.hpp uses (as WoWee's game/item_text.hpp).
#pragma once
#include <cstdint>
namespace wowee::game {
inline constexpr uint64_t COPPER_PER_SILVER = 100, COPPER_PER_GOLD = 10000;
struct CoinAmount { uint32_t gold = 0, silver = 0, copper = 0; };
inline CoinAmount splitCopper(uint64_t amount) {
    return {static_cast<uint32_t>(amount / COPPER_PER_GOLD), static_cast<uint32_t>((amount / COPPER_PER_SILVER) % 100),
            static_cast<uint32_t>(amount % COPPER_PER_SILVER)};
}
inline const char* itemBindText(uint32_t bindType) {
    switch (bindType) {
        case 1: return "Binds when picked up";
        case 2: return "Binds when equipped";
        case 3: return "Binds when used";
        case 4: return "Quest Item";
        default: return nullptr;
    }
}
}  // namespace wowee::game
