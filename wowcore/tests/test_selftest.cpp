#include <catch_amalgamated.hpp>
#include "../../skse/src/SelfTestEvidence.h"
#include "../../skse/src/ControlTiming.h"
#include "../../skse/src/ChannelTiming.h"

TEST_CASE("native level-choice evidence accepts the observed prompt and rejects incomplete movies", "[selftest][level-choice]")
{
    const std::array<std::string, 3> labels{"Health", "Magicka", "Stamina"};
    const std::string body = "You leveled up!  Choose an attribute to advance:";
    REQUIRE(SelfTest::Evidence::LevelChoices(true, body, labels));
    REQUIRE(SelfTest::Evidence::LevelChoices(true, "Attribut auswählen", {"Gesundheit", "Magicka", "Ausdauer"}));
    REQUIRE_FALSE(SelfTest::Evidence::LevelChoices(false, body, labels));
    REQUIRE_FALSE(SelfTest::Evidence::LevelChoices(true, "", labels));
    REQUIRE_FALSE(SelfTest::Evidence::LevelChoices(true, body, {"Health", "", "Stamina"}));
    REQUIRE_FALSE(SelfTest::Evidence::LevelChoices(true, body, {"Health", "Health", "Stamina"}));
}

TEST_CASE("talent healing evidence requires the actual released health budget", "[selftest][talents]")
{
    using SelfTest::Evidence::HealingObserved;
    REQUIRE(HealingObserved(50, 112, 62, 200));
    REQUIRE(HealingObserved(150, 200, 62, 200));  // capped overhealing is valid
    REQUIRE_FALSE(HealingObserved(50, 50, 62, 200));  // no native heal
    REQUIRE_FALSE(HealingObserved(50, 52, 62, 200));  // incidental native regeneration
    REQUIRE_FALSE(HealingObserved(200, 200, 62, 200));  // full-health probe proves nothing
    REQUIRE_FALSE(HealingObserved(50, 112, 61250, 200));  // placeholder scaling regression
    REQUIRE_FALSE(HealingObserved(50, 50, 0, 200));
}

TEST_CASE("Reflection evidence requires a started cast and actual mana restoration", "[selftest][talents]")
{
    using SelfTest::Evidence::ReflectionObserved;
    REQUIRE(ReflectionObserved(true, .897f, 0, 2.4f, 1200, 1202.4f));
    REQUIRE_FALSE(ReflectionObserved(false, .897f, 0, 2.4f, 1200, 1202.4f));  // unknown Healing Touch
    REQUIRE_FALSE(ReflectionObserved(true, .897f, 0, 0, 1200, 1202.4f));
    REQUIRE_FALSE(ReflectionObserved(true, .897f, 0, 2.4f, 1500, 1500));  // capped mana
    REQUIRE_FALSE(ReflectionObserved(true, 0, 0, 2.4f, 1200, 1202.4f));
}

TEST_CASE("Natural Weapons evidence checks the released native damage", "[selftest][talents]")
{
    using SelfTest::Evidence::DamageObserved;
    REQUIRE(DamageObserved(99955, 99847, 108));
    REQUIRE_FALSE(DamageObserved(99955, 99955, 108));
    REQUIRE_FALSE(DamageObserved(99955, 99857, 108)); // untalented 98 damage
    REQUIRE_FALSE(DamageObserved(99955, 99954, 108));
}

TEST_CASE("Starfire evidence checks the native paralysis carrier and status", "[selftest][talents]")
{
    using SelfTest::Evidence::StunObserved;
    REQUIRE(StunObserved(true, true, 2.400777766)); // October 9 live duration
    REQUIRE_FALSE(StunObserved(false, true, 2.4));
    REQUIRE_FALSE(StunObserved(true, false, 2.4));
    REQUIRE_FALSE(StunObserved(true, true, 0));
    REQUIRE_FALSE(StunObserved(true, true, 4));
}

TEST_CASE("Moonglow evidence requires payment for the intended spell", "[selftest][talents]")
{
    using SelfTest::Evidence::ManaPaymentObserved;
    REQUIRE(ManaPaymentObserved(9889, 9889, 91, 91));
    REQUIRE_FALSE(ManaPaymentObserved(9889, 0, 91, -1)); // BAD_TARGETS: no cast
    REQUIRE_FALSE(ManaPaymentObserved(9889, 9876, 91, 91));
    REQUIRE_FALSE(ManaPaymentObserved(9889, 9889, 91, 100));
    REQUIRE_FALSE(ManaPaymentObserved(9889, 9889, 0, 0));
}

TEST_CASE("Rejuvenation evidence waits for an actual native tick", "[selftest][talents]")
{
    using SelfTest::Evidence::HealingObserved;
    REQUIRE_FALSE(HealingObserved(536, 536, 0, 636)); // old poll at 2.505 sec
    REQUIRE(HealingObserved(536, 552, 16, 636)); // first 3-sec tick
    REQUIRE(HealingObserved(536, 568, 32, 636)); // delayed poll after two ticks
    REQUIRE_FALSE(HealingObserved(536, 536, 16, 636));
    REQUIRE_FALSE(HealingObserved(536, 550, 16, 636, .5)); // untalented tick
}

TEST_CASE("Brutal Impact evidence checks extended control without a positive paralysis value", "[selftest][talents][feral]")
{
    using SelfTest::Evidence::StunObserved;
    REQUIRE(StunObserved(true, true, 2.65, 2, 3)); // max-rank Pounce
    REQUIRE(StunObserved(true, true, .8, 0, 1)); // Bash still controls after its untalented 4 seconds
    REQUIRE_FALSE(StunObserved(false, true, 2.65, 2, 3)); // ledger alone proves nothing
    REQUIRE_FALSE(StunObserved(true, false, .8, 0, 1));
    REQUIRE_FALSE(StunObserved(true, true, 0, 0, 1)); // untalented Bash has expired
    REQUIRE_FALSE(StunObserved(true, true, 3.65, 2, 3)); // bleed must not extend stun
    REQUIRE_FALSE(StunObserved(false, false, .9866668283939362, 0, 1)); // October 10 Bash: ledger survives native control
}

TEST_CASE("paralysis survives native clock advance until each rank's bridge control deadline", "[selftest][talents][feral]")
{
    for (int rank : {1, 2}) {
        const double pounceEnd = 2 + .5 * rank, bashEnd = 4 + .5 * rank;
        // Engine time can advance past the old ceil(duration)+1 backstop
        // while Frame::Now is still in the talent extension (clamped hitches).
        const double nativeElapsed = 8;
        REQUIRE(nativeElapsed < ControlTiming::kParalysisBackstopSeconds);
        REQUIRE_FALSE(ControlTiming::Expired(2.1, pounceEnd));
        REQUIRE_FALSE(ControlTiming::Expired(4.1, bashEnd));
        REQUIRE(ControlTiming::Expired(pounceEnd, pounceEnd));
        REQUIRE(ControlTiming::Expired(bashEnd, bashEnd));
        REQUIRE(ControlTiming::Expired(pounceEnd + .4, pounceEnd));
        REQUIRE_FALSE(ControlTiming::Expired(pounceEnd + .4, 18)); // bleeding continues
    }
    REQUIRE_FALSE(ControlTiming::Expired(100, 0)); // no control attached
}

TEST_CASE("Feral Charge evidence checks landing and subsequent melee reach separately", "[selftest][talents][feral]")
{
    using SelfTest::Evidence::ChargeObserved;
    REQUIRE(ChargeObserved(true, true, 3.65, 0, 512, 2, 2.2f, 16979, 5, 95)); // physics after landing
    REQUIRE_FALSE(ChargeObserved(true, true, 3.65, 0, 0, 2, 2, 16979, 5, 95)); // no charge
    REQUIRE_FALSE(ChargeObserved(true, true, 3.65, 0, 512, 3, 3, 16979, 5, 95)); // wrong destination
    REQUIRE_FALSE(ChargeObserved(true, true, 3.65, 0, 512, 2, 5.1f, 16979, 5, 95)); // outside melee
    REQUIRE_FALSE(ChargeObserved(false, true, 3.65, 0, 512, 2, 2, 16979, 5, 95));
    REQUIRE_FALSE(ChargeObserved(true, false, 3.65, 0, 512, 2, 2, 16979, 5, 95));
    REQUIRE_FALSE(ChargeObserved(true, true, 0, 0, 512, 2, 2, 16979, 5, 95));
    REQUIRE_FALSE(ChargeObserved(true, true, 1.65, 0, 512, 2, 2, 16979, 5, 95)); // untalented/short root
    REQUIRE_FALSE(ChargeObserved(true, true, 3.65, 100, 512, 2, 2, 16979, 5, 95));
    REQUIRE_FALSE(ChargeObserved(true, true, 3.65, 0, 512, 2, 2, 5487, 5, 95));
    REQUIRE_FALSE(ChargeObserved(true, true, 3.65, 0, 512, 2, 2, 16979, 0, 100));
}

TEST_CASE("Heart evidence measures talent gains against the same form baseline", "[selftest][talents][feral]")
{
    using SelfTest::Evidence::StatGainObserved;
    REQUIRE(StatGainObserved(true, 1500, 1585.5f, 345, 288, 1.5f)); // live intellect
    REQUIRE(StatGainObserved(true, 300, 312, 74, 62, 1)); // live Cat CarryWeight
    REQUIRE(StatGainObserved(true, 0, 6, 74, 62, .5f)); // native weapon modifiers
    REQUIRE(StatGainObserved(true, 100, 237, 229, 191, 1.5f, 80)); // separate Bear form health
    REQUIRE_FALSE(StatGainObserved(true, 100, 180, 229, 191, 1.5f, 80)); // form health alone
    REQUIRE_FALSE(StatGainObserved(false, 100, 112, 74, 62, 1)); // profile still pending
    REQUIRE_FALSE(StatGainObserved(true, 100, 100, 74, 62, 1)); // missing native talent
}

TEST_CASE("Hurricane utility effects cannot accelerate periodic channel damage", "[selftest][channel]")
{
    using json=nlohmann::json;
    const json damage={{"type","damage"},{"amplitude_ms",1000},{"duration_ms",10000},
        {"area",{{"channel",true},{"radius_yd",8.0}}}};
    const json slow={{"type","aura_138"},{"amount",-25},{"amplitude_ms",0},{"duration_ms",10000},
        {"area",{{"channel",true},{"radius_yd",8.0}}}};
    for (const auto& auras : {json::array({damage,slow}),json::array({slow,damage})}) {
        const auto schedule=ChannelTiming::From({{"auras",auras}});
        REQUIRE(schedule);
        REQUIRE(schedule->period==1.0);
        REQUIRE(schedule->duration==10.0);
        REQUIRE(schedule->duration/schedule->period==10);
        REQUIRE(schedule->radius==8.0f);
        REQUIRE_FALSE(schedule->heal);
    }
    auto heal=damage; heal["type"]="heal"; heal["amplitude_ms"]=2000; heal["area"]["radius_yd"]=30.0;
    const auto tranquility=ChannelTiming::From({{"auras",json::array({slow,heal})}});
    REQUIRE(tranquility);
    REQUIRE(tranquility->heal);
    REQUIRE(tranquility->period==2.0);
    REQUIRE(tranquility->duration/tranquility->period==5);
    REQUIRE_FALSE(ChannelTiming::From({{"auras",json::array({slow})}}));
    heal["amplitude_ms"]=0;
    REQUIRE_FALSE(ChannelTiming::From({{"auras",json::array({heal})}}));
}

#include "../../skse/src/ColdPersistence.h"
TEST_CASE("cold persistence only loads its own disposable saves", "[selftest][persistence]") {
    REQUIRE(ColdPersistence::SafeSaveName("WBColdPersistenceTest_1791799000_453"));
    for (const auto invalid : {"", "personal", "WBColdPersistenceTest_", "WBColdPersistenceTest_../old", "../WBColdPersistenceTest_12", "WBColdPersistenceTest_12.ess", "WBColdPersistenceTest_12\\save"})
        CHECK_FALSE(ColdPersistence::SafeSaveName(invalid));
    CHECK_FALSE(ColdPersistence::SafeSaveName("WBColdPersistenceTest_"+std::string(96,'1')));
    CHECK(ColdPersistence::DefaultUi(true,false,"wow_bags"));
    CHECK_FALSE(ColdPersistence::DefaultUi(true,false,"wow_cold_persistence_verify"));
    CHECK_FALSE(ColdPersistence::DefaultUi(true,true,"wow_bags"));
    CHECK_FALSE(ColdPersistence::DefaultUi(false,false,""));
}
