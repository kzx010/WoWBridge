#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace wowcore {
// Skyrim BarterMenu.as: apply the engine's Speech/perk multipliers to the
// ItemCard instance value, minimum one when buying, then round half upwards.
inline std::optional<std::int32_t> barterPrice(double value, double multiplier, bool buying) {
    if (!std::isfinite(value) || !std::isfinite(multiplier) || value < 0 || multiplier < 0) return {};
    const double price = std::floor((buying ? std::max(1.0, value * multiplier) : value * multiplier) + 0.5);
    if (price > std::numeric_limits<std::int32_t>::max()) return {};
    return static_cast<std::int32_t>(price);
}
struct BarterQuote { std::int32_t total, paid; };
inline std::optional<BarterQuote> barterQuote(std::int32_t price, std::int32_t count,
    std::int32_t available, std::int32_t gold, bool buying) {
    if (price < 0 || count <= 0 || count > available || gold < 0) return {};
    const auto total = std::int64_t(price) * count;
    if (total > std::numeric_limits<std::int32_t>::max() || (buying && total > gold)) return {};
    return BarterQuote{static_cast<std::int32_t>(total), static_cast<std::int32_t>(buying ? total : std::min(total, std::int64_t(gold)))};
}
}
