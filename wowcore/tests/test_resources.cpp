#include <catch_amalgamated.hpp>
#include "../../skse/src/AuraPersistence.h"
#include "addons/lua_engine.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/soft_raster.hpp"
#include "wowcore/ui_session.hpp"
extern "C" {
#include <lauxlib.h>
}
#include <cstdlib>
#include <filesystem>

namespace {
std::filesystem::path clientData()
{
    const auto* env = std::getenv("WOWBRIDGE_WOW_DATA");
    std::filesystem::path path = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(path / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    return path;
}
struct ResourceUi
{
    std::shared_ptr<wowcore::Mpq> mpq = std::make_shared<wowcore::Mpq>(clientData());
    wowcore::ClientDb db{*mpq};
    wowcore::SoftTextures textures;
    wowcore::UiSession ui{mpq, db, textures, 1280, 720};
    ResourceUi()
    {
        std::string error;
        REQUIRE(ui.start(&error));
        REQUIRE(luaL_dofile(ui.lua(), (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
        run("__WBFixture('idle')");
        REQUIRE(ui.loadInterface(&error));
        run("__WBLogin()");
    }
    void run(const char* script)
    {
        const auto rc = luaL_dostring(ui.lua(), script);
        INFO((rc ? lua_tostring(ui.lua(), -1) : ""));
        REQUIRE(rc == 0);
    }
};
enum class Value { None=-1, Speed=30, Armor=39, AttackSpeed=89 };
struct Change { std::uint32_t vanilla=0; float magnitude=0; Value value=Value::None; };
}

TEST_CASE("form switching follows live hidden magicka and refreshes the original client buttons", "[client][api][resources]")
{
    ResourceUi test;
    test.run(R"(
        __WBDruidForms(9634,4,true)
        __WB.skyrimCosts={[9634]=30,[768]=40,[9913]=0}
        __WBApplyLive({player={powerType=0,power=80,powerMax=100,skyrim={},nativeSheet={magicka=80,magickaMax=100}}})
        __WBForm(3)
        __WB.actions[1]={spell=9634} __WB.actions[2]={spell=9913} __WB.actions[3]={spell=768}
        __WBApplyLive({player={power=100,nativeSheet={magicka=5,magickaMax=100}}})
        assert(not select(4,GetShapeshiftFormInfo(1)), 'Bear button used pre-shift mana')
        assert(math.abs(ShapeshiftButton1Icon:GetVertexColor()-0.4)<0.001, 'client form icon did not gray out')
        local usable,oom=IsUsableAction(1)
        assert(not usable and oom, 'Bear action ignored hidden magicka')
        assert(select(4,GetShapeshiftFormInfo(3)) and IsUsableAction(3), 'leaving the active form must be free')
        __resourceEvents=0
        local probe=CreateFrame('Frame') probe:RegisterEvent('SPELL_UPDATE_USABLE')
        probe:SetScript('OnEvent',function() __resourceEvents=__resourceEvents+1 end)
        __WBApplyLive({player={power=100,nativeSheet={magicka=35,magickaMax=100}}})
        assert(__resourceEvents==1, 'hidden mana regeneration did not refresh the form bar')
        assert(select(4,GetShapeshiftFormInfo(1)) and IsUsableAction(1))
        assert(ShapeshiftButton1Icon:GetVertexColor()==1, 'client form icon stayed gray after magicka recovered')
        __WBApplyLive({player={power=100,nativeSheet={magicka=35,magickaMax=100}}})
        assert(__resourceEvents==1, 'unchanged snapshots should not refresh availability')
        __WBForm(1)
        usable,oom=IsUsableAction(2) -- Prowl automatically enters Cat, which costs 40
        assert(not usable and oom, 'automatic Cat shift ignored live magicka')
        __WBApplyLive({player={nativeSheet={magicka=45,magickaMax=100}}})
        assert(IsUsableAction(2))
        __WBForm(0)
        assert(UnitPowerType('player')==0 and UnitMana('player')==45, 'leaving a form restored stale mana')
    )");
}

TEST_CASE("percentage mana costs agree between action availability and tooltips", "[client][api][resources]")
{
    ResourceUi test;
    test.run(R"(
        __WB.actions[1]={spell=22812} -- Barkskin: 15% of the recorded druid's base mana
        __WBApplyLive({player={powerType=0,power=185}})
        local info=__WB.spellInfo(22812)
        assert(info.costPct==15 and __WB.resolve('player').baseMana==1244)
        local usable,oom=IsUsableAction(1)
        assert(not usable and oom, 'percentage cost was ignored by the action button')
        GameTooltip:SetOwner(UIParent,'ANCHOR_NONE') assert(GameTooltip:SetAction(1))
        local text=''
        for i=1,GameTooltip:NumLines() do text=text..(getglobal('GameTooltipTextLeft'..i):GetText() or '') end
        assert(string.find(text,'186 Mana',1,true))
        __WBApplyLive({player={power=186}}) assert(IsUsableAction(1))
        __WB.skyrimCosts={[22812]=17}
        __WBApplyLive({player={power=16,skyrim={}}})
        usable,oom=IsUsableAction(1) assert(not usable and oom)
        __WBApplyLive({player={power=17}}) assert(IsUsableAction(1))
    )");
}

TEST_CASE("saved aura carriers retain explicit actor values and vanilla effect identities", "[auras][persistence]")
{
    const std::vector<Change> original{{0,60,Value::Speed},{0,-30,Value::Speed},
        {0,-0.5f,Value::AttackSpeed},{0x7A0FD,20,Value::None}};
    const auto saved = AuraPersistence::SaveChanges(original);
    const auto loaded = AuraPersistence::LoadChanges<Change>(nlohmann::json::parse(saved.dump()),164);
    REQUIRE(loaded.size()==original.size());
    for (std::size_t i=0; i<original.size(); ++i) {
        CHECK(loaded[i].vanilla==original[i].vanilla);
        CHECK(loaded[i].magnitude==original[i].magnitude);
        CHECK(loaded[i].value==original[i].value);
    }
}

TEST_CASE("legacy two-field aura records retain their vanilla carrier effects", "[auras][persistence]")
{
    const auto old = nlohmann::json::parse("[[499965,20],[300951,-40]]");
    const auto loaded = AuraPersistence::LoadChanges<Change>(old,164);
    REQUIRE(loaded.size()==2);
    CHECK(loaded[0].vanilla==499965);
    CHECK(loaded[1].magnitude==-40);
    CHECK(loaded[0].value==Value::None);
}

TEST_CASE("damaged aura carrier rows are skipped without losing valid neighbors", "[auras][persistence]")
{
    using nlohmann::json;
    const auto bad = json::parse(R"([[0,60,30],null,{},[],[0],[0,"bad"],[0,1,null],
        [-1,2],[4294967296,2],[0,1,164],[0,1,-2],[0,1,4294967295],[0,1,1.5],
        [0,1e100],[0,1,18446744073709551615],[499965,20]])");
    const auto loaded = AuraPersistence::LoadChanges<Change>(bad,164);
    REQUIRE(loaded.size()==2);
    CHECK(loaded[0].value==Value::Speed);
    CHECK(loaded[1].vanilla==499965);
    CHECK(loaded[1].value==Value::None);
    CHECK(AuraPersistence::LoadChanges<Change>(nullptr,164).empty());
    CHECK(AuraPersistence::LoadChanges<Change>(json::object(),164).empty());
}

TEST_CASE("legacy saved Prowl restores each rank through the normal stealth outcome", "[auras][persistence][forms]")
{
    for (const auto rank : {5215u, 6783u, 9913u}) {
        INFO(rank);
        // The persisted aura never had a separate stealth flag.
        const auto saved = nlohmann::json::parse(nlohmann::json{{"spell",rank}}.dump());
        const auto spell = saved.value("spell",0u);
        CHECK(AuraPersistence::IsStealth(spell));
        const auto outcome = AuraPersistence::StealthOutcome(spell,"cat",4);
        REQUIRE(outcome.is_object());
        CHECK(outcome["spell"]==rank);
        REQUIRE(outcome["auras"].size()==1);
        CHECK(outcome["auras"][0]["type"]=="stealth");
        CHECK(outcome["auras"][0].value("prowling",false));
        CHECK_FALSE(outcome["auras"][0].value("shadowmeld",false));
        CHECK(AuraPersistence::StealthOutcome(spell,"cat",6).is_object());
    }
}

TEST_CASE("saved stealth restores only in compatible form and racial contexts", "[auras][persistence][forms]")
{
    for (const auto form : {"none","bear","dire_bear","aquatic","travel","moonkin"})
        for (const auto rank : {5215u,6783u,9913u})
            CHECK(AuraPersistence::StealthOutcome(rank,form,4).is_null());
    CHECK(AuraPersistence::IsStealth(20580));
    const auto meld = AuraPersistence::StealthOutcome(20580,"none",4);
    REQUIRE(meld.is_object());
    CHECK(meld["spell"]==20580);
    CHECK(meld["auras"][0]["type"]=="stealth");
    CHECK(meld["auras"][0]["shadowmeld"]==true);
    CHECK_FALSE(meld["auras"][0].value("prowling",false));
    CHECK(AuraPersistence::StealthOutcome(20580,"cat",4).is_object());
    for (const auto race : {0u,1u,2u,3u,5u,6u,7u,8u})
        CHECK(AuraPersistence::StealthOutcome(20580,"none",race).is_null());
    for (const auto spell : {0u,768u,1850u,5214u,5216u,6782u,6784u,9912u,9914u}) {
        CHECK_FALSE(AuraPersistence::IsStealth(spell));
        CHECK(AuraPersistence::StealthOutcome(spell,"cat",4).is_null());
    }
}
