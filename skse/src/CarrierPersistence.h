#pragma once
#include <cmath>
#include <optional>

namespace CarrierPersistence {
    // Skyrim can retain a temporary AV modifier while dropping its runtime
    // ActiveEffect on a cold load. Correct only a matching saved contribution;
    // a surviving effect must keep the normal native dispel/replacement path.
    inline std::optional<float> MissingModifier(float saved, float owned, float current, bool surviving) {
        if (!std::isfinite(saved) || !std::isfinite(owned) || !std::isfinite(current)) return {};
        if (surviving || std::abs(owned)<.001f || std::abs(current-(saved-owned))<.01f) return 0.f;
        if (std::abs(current-saved)<.01f) return -owned;
        return {};
    }
}
