#pragma once

#include <cstdint>

namespace ControlTiming
{
    // Frame::Now clamps hitches; Skyrim's active-effect clock does not share
    // that deadline. Keep paralysis alive until the aura explicitly dispels it.
    // Set this once, before casting: active effects retain the spell's Effect*.
    inline constexpr std::uint32_t kParalysisBackstopSeconds = 10000000;

    inline bool Expired(double now, double controlEnd)
    {
        return controlEnd > 0 && now >= controlEnd;
    }
}
