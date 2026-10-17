// Portable persistence checks; no Skyrim process or shared build outputs.
// Spell.dbc 2893/3137 and CMaNGOS Aura::PeriodicTick use a 2s dispel clock.
#include "AuraPersistence.h"
#include "CastPersistence.h"
#include <cassert>

struct Aura {
    std::string dispel;
    double dispelEvery = 0, nextDispel = 0;
    int dispelCount = 0;
};

int main()
{
    using nlohmann::json;
    Aura original{"Poison", 2, 101.25, 1}, restored;
    const auto timer = json::parse(AuraPersistence::SaveDispel(original, 100).dump());
    assert(CastPersistence::PeriodicDispel(timer));
    AuraPersistence::LoadDispel(restored, timer, 500);
    assert(restored.dispel == "Poison" && restored.dispelCount == 1);
    assert(restored.dispelEvery == 2 && restored.nextDispel == 501.25);
    Aura legacy;
    AuraPersistence::LoadDispel(legacy, json::object(), 500);
    assert(legacy.dispelEvery == 0); // old records never invent periodic effects

    json resources = {{"form","bear"},{"rage",55},{"energy",20},
        {"energy_in",.25},{"decay_in",1.75},{"no_decay_in",8},
        {"rage_auras",json::array({{{"spell",5229},{"next",.75},{"end",8},
            {"every",1},{"per_tick",2},{"max_per_tick",0},{"health_per_rage",0},
            {"crits",json::array()},{"tick",2},{"crit_multiplier",1.5},{"respite",false}}})}};
    json record = {{"form_resources",resources},{"auras",json::array({
        {{"spell",2893},{"remaining",6.25},{"periodic_dispel",timer}}})}};
    assert(CastPersistence::Valid(json::parse(record.dump())));
    assert(CastPersistence::Valid({{"auras",json::array({{{"spell",2893}}})}}));
    auto bad = record; bad["auras"][0]["periodic_dispel"]["every"] = .00001;
    assert(!CastPersistence::Valid(bad));
    bad = record; bad["auras"][0]["periodic_dispel"]["count"] = -1;
    assert(!CastPersistence::Valid(bad));
    bad = record; bad["form_resources"]["rage"] = 101;
    assert(!CastPersistence::Valid(bad));
    bad = record; bad["form_resources"]["rage_auras"][0]["every"] = 0;
    assert(!CastPersistence::Valid(bad));
    bad = record; bad["form_resources"]["rage_auras"][0].erase("crits");
    assert(!CastPersistence::Valid(bad));
}
