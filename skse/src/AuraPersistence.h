#pragma once

#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>
#include <string_view>
#include <nlohmann/json.hpp>

namespace AuraPersistence
{
    inline bool IsProwl(std::uint32_t spell)
    {
        return spell == 5215 || spell == 6783 || spell == 9913;
    }

    inline bool IsStealth(std::uint32_t spell)
    {
        return IsProwl(spell) || spell == 20580;
    }

    // Reuse Forms::Apply after restoring the form; old saves contain only the
    // spell identity, not a separate native stealth state.
    inline nlohmann::json StealthOutcome(std::uint32_t spell, std::string_view form, std::uint32_t race)
    {
        if (IsProwl(spell) && form == "cat")
            return {{"spell", spell}, {"auras", nlohmann::json::array({{{"type", "stealth"}, {"prowling", true}}})}};
        if (spell == 20580 && race == 4)
            return {{"spell", spell}, {"auras", nlohmann::json::array({{{"type", "stealth"}, {"shadowmeld", true}}})}};
        return nullptr;
    }

    template <class Changes>
    nlohmann::json SaveChanges(const Changes& changes)
    {
        auto rows = nlohmann::json::array();
        // The optional third field keeps direct actor-value carriers alive
        // across saves. Older records identify their effect through vanilla.
        for (const auto& c : changes) rows.push_back({c.vanilla, c.magnitude, static_cast<std::int32_t>(c.value)});
        return rows;
    }

    template <class Change>
    std::vector<Change> LoadChanges(const nlohmann::json& rows, std::int32_t valueCount)
    {
        std::vector<Change> changes;
        if (!rows.is_array()) return changes;
        for (const auto& row : rows) {
            if (!row.is_array() || row.size() < 2 || !row[0].is_number_integer() || !row[1].is_number()) continue;
            // Validate before integer conversion; malformed unsigned JSON can
            // otherwise wrap into a valid negative sentinel such as kNone.
            const auto vanilla = row[0].template get<double>();
            if (vanilla < 0 || vanilla > std::numeric_limits<std::uint32_t>::max()) continue;
            const auto magnitude = row[1].template get<double>();
            if (!std::isfinite(magnitude) || std::abs(magnitude) > std::numeric_limits<float>::max()) continue;
            Change c;
            c.vanilla = row[0].template get<decltype(c.vanilla)>();
            c.magnitude = static_cast<float>(magnitude);
            if (row.size() >= 3) {
                if (!row[2].is_number_integer()) continue;
                const auto value = row[2].template get<double>();
                if (value < -1 || value >= valueCount) continue;
                c.value = static_cast<decltype(c.value)>(row[2].template get<std::int32_t>());
            }
            changes.push_back(c);
        }
        return changes;
    }
}
