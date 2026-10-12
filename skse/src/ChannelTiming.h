#pragma once

#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace ChannelTiming
{
    struct Schedule { double duration, period; float radius; bool heal; };

    inline std::optional<Schedule> From(const nlohmann::json& outcome)
    {
        // Hurricane also carries a nonperiodic attack-speed aura. Only its
        // periodic health aura owns cadence; utility effects cannot replace it.
        for (const auto& aura : outcome.value("auras", nlohmann::json::array())) {
            const auto type=aura.value("type",std::string{});
            if ((type!="damage" && type!="heal") || !aura.contains("area")) continue;
            const auto& area=aura["area"];
            if (!area.value("channel",false) && type!="damage") continue;
            const auto duration=aura.value("duration_ms",0), period=aura.value("amplitude_ms",0);
            const auto radius=area.value("radius_yd",0.f);
            if (duration<=0 || period<=0 || radius<=0) continue;
            return Schedule{duration/1000.0,period/1000.0,radius,type=="heal"};
        }
        return std::nullopt;
    }
}
