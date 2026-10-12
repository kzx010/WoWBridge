// WoWBridge compat: the item-quality enum WoWee's ui_colors.hpp names (values as WoWee's game/inventory.hpp).
#pragma once
#include <cstdint>
namespace wowee::game {
enum class ItemQuality : uint8_t { POOR = 0, COMMON = 1, UNCOMMON = 2, RARE = 3, EPIC = 4, LEGENDARY = 5, ARTIFACT = 6, HEIRLOOM = 7 };
}
