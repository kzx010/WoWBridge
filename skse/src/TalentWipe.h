#pragma once
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

// 1.12's talent wipe price (CMaNGOS Player::resetTalentsCost), in gold - septims here: 1, then 5, then 10, then 5 more
// each time up to 50; once a month (30 days, Skyrim's game days) has passed since the last wipe it is instead 5 less per
// month, never under 10. The last price and its day ride in the talent co-save record (TLNT); a record from before them,
// or with them damaged, reads as never wiped.
namespace TalentWipe {
struct State { int cost = 0; double day = 0; };   // the last wipe's price (0: never) and its game day (days passed)

inline int Cost(const State& a_last, double a_today)
{
    if (a_last.cost < 1) return 1;
    if (a_last.cost < 5) return 5;
    if (a_last.cost < 10) return 10;
    const auto months = static_cast<long long>((a_today - a_last.day) / 30.0);   // whole months, as time_t's division
    if (months > 0) return static_cast<int>(std::max(10LL, a_last.cost - 5 * months));
    return std::min(50, a_last.cost + 5);
}

inline State Read(const nlohmann::json& a_record)
{
    const auto cost = a_record.value("reset_cost", nlohmann::json(0)), day = a_record.value("reset_day", nlohmann::json(0.0));
    if (!cost.is_number_integer() || cost.get<long long>() < 0 || cost.get<long long>() > 50 || !day.is_number() ||
        !std::isfinite(day.get<double>()) || day.get<double>() < 0) return {};
    return { cost.get<int>(), day.get<double>() };
}

inline void Write(nlohmann::json& a_record, const State& a_state)
{
    a_record["reset_cost"] = a_state.cost;
    a_record["reset_day"] = a_state.day;
}
}
