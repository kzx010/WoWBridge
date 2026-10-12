#include <catch_amalgamated.hpp>
#include "../../skse/src/CastPersistence.h"
#include <limits>
using json=nlohmann::json;

TEST_CASE("cast records validate before any state is restored", "[persistence][cast]") {
    const json aura={{"spell",1126},{"name","Mark of the Wild"},{"remaining",30.0}};
    const json good={{"saved_at",1791800000.0},{"cooldowns",{{"17402",30.0}}},{"auras",json::array({aura})}};
    REQUIRE(CastPersistence::Valid(good));
    CHECK(CastPersistence::Valid(json::object()));
    auto legacy=good; legacy["mana"]=5284; CHECK(CastPersistence::Valid(legacy));
    for (const json bad : {json(nullptr),json::array(),json("broken"),json(3),json(false)}) CHECK_FALSE(CastPersistence::Valid(bad));
    for (const char* key : {"","not-a-spell","1junk","-1","0","4294967296","18446744073709551615"}) {
        auto record=good; record["cooldowns"][key]=30; INFO(key); CHECK_FALSE(CastPersistence::Valid(record));
    }
    for (const json bad : {json(nullptr),json("later"),json::array(),json::object(),json(true),json(1e300)}) {
        auto record=good; record["saved_at"]=bad; CHECK_FALSE(CastPersistence::Valid(record));
        record=good; record["cooldowns"]["17402"]=bad; CHECK_FALSE(CastPersistence::Valid(record));
        record=good; record["auras"][0]["remaining"]=bad; CHECK_FALSE(CastPersistence::Valid(record));
    }
    for (const char* key : {"auras","cooldowns"}) {
        auto record=good; record[key]=false; CHECK_FALSE(CastPersistence::Valid(record));
    }
    for (const json bad : {json(-1),json(4294967296ULL),json(1.5),json("1126")}) {
        auto record=good; record["auras"][0]["spell"]=bad; CHECK_FALSE(CastPersistence::Valid(record));
    }
    for (const json bad : {json::array({1}),json::array({1,"amount","heal"}),json::array({1,2147483648ULL,"heal"}),json::array({1,20,false})}) {
        auto record=good; record["auras"][0]["ticks"]=json::array({bad}); CHECK_FALSE(CastPersistence::Valid(record));
    }
    auto record=good; record["auras"][0]["ticks"]=json::array({json::array({1.0,20,"heal"})}); CHECK(CastPersistence::Valid(record));
    record["auras"][0]["remaining"]=std::numeric_limits<double>::infinity(); CHECK_FALSE(CastPersistence::Valid(record));
    record=good; record["auras"][0]["shield"]=4294967296ULL; CHECK_FALSE(CastPersistence::Valid(record));
    record=good; record["auras"][0]["harmful"]="false"; CHECK_FALSE(CastPersistence::Valid(record));
}

TEST_CASE("cast records retain legacy carriers and validate nested tooltip and form data", "[persistence][cast]") {
    json form={{"spell",768},{"auras",json::array({{{"type","shapeshift"},{"form","cat"},{"power","energy"},{"duration_ms",-1},{"display_id",892},{"power_set",{{"energy",0}}},{"leader_of_the_pack",nullptr}}})},{"events",json::array()}};
    json aura={{"spell",768},{"remaining",1e12},{"form_outcome",form},{"skyrim",json::array({json::array({499965,20})})}};
    json record={{"auras",json::array({aura})}};
    REQUIRE(CastPersistence::Valid(record));
    auto bad=record; bad["auras"][0]["form_outcome"]["auras"][0]["form"]=3; CHECK_FALSE(CastPersistence::Valid(bad));
    bad=record; bad["auras"][0]["form_outcome"]["auras"][0]["power_set"]["energy"]="full"; CHECK_FALSE(CastPersistence::Valid(bad));
    bad=record; bad["auras"][0]["form_outcome"]["auras"][0]["leader_of_the_pack"]={{"radius_yd",false}}; CHECK_FALSE(CastPersistence::Valid(bad));
    form["form_tracking"]={{"spell",5225},{"auras",json::array({{{"type","track_creatures"},{"aura",44},{"mask",128},{"duration_ms",-1}}})}};
    record["auras"][0]["form_outcome"]=form; CHECK(CastPersistence::Valid(record));
    json tooltip={{"effects",json::array({json::array({12,20})})},{"duration_ms",30000},{"cast_ms",0},{"range_max",30},{"cooldown_ms",0}};
    tooltip["minimum_health"]=false; tooltip["health_factor"]=0.0;
    record["auras"][0]["tooltip"]=tooltip; REQUIRE(CastPersistence::Valid(record));
    record["auras"][0]["tooltip"]["minimum_health"]=true; CHECK(CastPersistence::Valid(record));
    auto wrongMinimum=record; wrongMinimum["auras"][0]["tooltip"]["minimum_health"]="true"; CHECK_FALSE(CastPersistence::Valid(wrongMinimum));
    for (const char* key : {"effects","duration_ms","cast_ms","range_max","cooldown_ms"}) {
        auto b=record; b["auras"][0]["tooltip"].erase(key); INFO(key); CHECK_FALSE(CastPersistence::Valid(b));
    }
    bad=record; bad["auras"][0]["tooltip"]["effects"]=json::array({json::array({12})}); CHECK_FALSE(CastPersistence::Valid(bad));
    record["future_field"]="ignored"; CHECK(CastPersistence::Valid(record));
}
