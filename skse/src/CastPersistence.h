#pragma once
#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>

// Validate the whole record before Restore changes any native or UI state.
// Optional fields preserve older aura records; unknown fields remain readable.
namespace CastPersistence {
using json=nlohmann::json;
inline bool Number(const json& v,double low=-1e12,double high=1e12) {
    if (!v.is_number()) return false;
    const auto n=v.get<double>(); return std::isfinite(n) && n>=low && n<=high;
}
inline bool UInt(const json& v) { return v.is_number_integer() && Number(v,0,UINT32_MAX); }
inline bool Int(const json& v) { return v.is_number_integer() && Number(v,INT32_MIN,INT32_MAX); }
inline bool Bool(const json& v) { return v.is_boolean(); }
inline bool String(const json& v) { return v.is_string(); }
template<class Predicate>
bool Fields(const json& v,std::initializer_list<const char*> keys,Predicate check) {
    for (const auto* key : keys) if (v.contains(key) && !check(v[key])) return false;
    return true;
}
inline bool Tooltip(const json& v) {
    if (v.is_null()) return true;
    if (!v.is_object() || !v.contains("effects") || !v["effects"].is_array()) return false;
    for (const auto& range : v["effects"])
        if (!range.is_array() || range.size()<2 || !Number(range[0]) || !Number(range[1])) return false;
    for (const auto* key : {"duration_ms","cast_ms","range_max","cooldown_ms"})
        if (!v.contains(key) || !Number(v[key])) return false;
    return Fields(v,{"proc_chance","health_factor"},[](const json& x) { return Number(x); }) && Fields(v,{"minimum_health"},Bool);
}
inline bool Outcome(const json& v,bool tracking=false) {
    if (!v.is_object() || !Fields(v,{"spell"},UInt) || !Fields(v,{"school"},Int) ||
        !Fields(v,{"name"},String) || !Fields(v,{"breaks_prowl"},Bool) ||
        !Fields(v,{"threat_multiplier"},[](const json& x) { return Number(x); }) || !Fields(v,{"tooltip"},Tooltip)) return false;
    if (v.contains("auras")) {
        if (!v["auras"].is_array()) return false;
        for (const auto& a : v["auras"]) {
            if (!a.is_object() || !Fields(a,{"type","form","power","action"},String) ||
                !Fields(a,{"duration_ms","skyrim_health_bonus"},Int) ||
                !Fields(a,{"display_id","aura","mask"},UInt) ||
                !Fields(a,{"water_breathing","clears_prowl","ends_on_form_loss"},Bool) ||
                !Fields(a,{"armor_percent","movement_speed_percent","swim_speed_percent","dodge_bonus_percent","stealth_level_bonus"},[](const json& x) { return Number(x); })) return false;
            const auto type=a.value("type",std::string{});
            if (tracking ? (type!="track_creatures" && type!="track_resources" && type!="track_stealthed") : type!="shapeshift") return false;
            if (a.contains("power_set")) {
                if (!a["power_set"].is_object()) return false;
                for (const auto& value : a["power_set"].items()) if (!Number(value.value(),0,100)) return false;
            }
            if (a.contains("leader_of_the_pack") && !a["leader_of_the_pack"].is_null()) {
                const auto& leader=a["leader_of_the_pack"];
                if (!leader.is_object() || !Fields(leader,{"radius_yd","crit_percent"},[](const json& x) { return Number(x); })) return false;
            }
            // Form and tracking outcomes have no periodic ticks or direct
            // native carriers. Those belong to the saved aura's own fields.
            for (const auto* key : {"ticks","skyrim"}) if (a.contains(key) && (!a[key].is_array() || !a[key].empty())) return false;
        }
    }
    if (v.contains("events")) {
        if (!v["events"].is_array()) return false;
        for (const auto& e : v["events"])
            if (!e.is_object() || !Fields(e,{"type","power"},String) || !Fields(e,{"spell"},UInt) ||
                !Fields(e,{"school_lockout_ms"},Int) || !Fields(e,{"amount"},[](const json& x) { return Number(x); })) return false;
    }
    return !v.contains("form_tracking") || (!tracking && Outcome(v["form_tracking"],true));
}
inline bool Valid(const json& v) {
    if (!v.is_object() || !Fields(v,{"saved_at","mana"},[](const json& x) { return Number(x); })) return false;
    if (v.contains("cooldowns")) {
        if (!v["cooldowns"].is_object()) return false;
        for (const auto& [key,left] : v["cooldowns"].items()) {
            std::uint32_t id=0; const auto [end,error]=std::from_chars(key.data(),key.data()+key.size(),id);
            if (error!=std::errc{} || end!=key.data()+key.size() || !id || !Number(left)) return false;
        }
    }
    if (v.contains("auras")) {
        if (!v["auras"].is_array()) return false;
        for (const auto& a : v["auras"]) {
            if (!a.is_object() || !a.contains("spell") || !UInt(a["spell"]) || a["spell"].get<std::uint32_t>()==0 ||
                !Fields(a,{"tracking_aura","tracking_mask"},UInt) || !Fields(a,{"school","shield"},Int) ||
                !Fields(a,{"name"},String) || !Fields(a,{"harmful","ends_with_form","respite","steady"},Bool) ||
                !Fields(a,{"remaining","wow_per_tick","threat_multiplier"},[](const json& x) { return Number(x); }) ||
                !Fields(a,{"tooltip"},Tooltip)) return false;
            if (a.contains("form_outcome") && !a["form_outcome"].is_null() && !Outcome(a["form_outcome"])) return false;
            if (a.contains("ticks")) {
                if (!a["ticks"].is_array()) return false;
                for (const auto& tick : a["ticks"])
                    if (!tick.is_array() || tick.size()<3 || !Number(tick[0]) || !Int(tick[1]) || !String(tick[2])) return false;
            }
            if (a.contains("skyrim") && !a["skyrim"].is_array()) return false;
        }
    }
    return true;
}
}
