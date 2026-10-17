// A Shrine of Kynareth's talent wipe: CMaNGOS's 1.12 price ladder kept in the talent co-save record, and the client's own
// CONFIRM_TALENT_WIPE popup over the host's offer (docs/talents.md).
#include <catch_amalgamated.hpp>
#include "../../skse/src/TalentWipe.h"
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/ui_session.hpp"
#include "wowcore/soft_raster.hpp"
#include "wowcore/wowapi.hpp"
#include "addons/lua_engine.hpp"
extern "C" {
#include <lauxlib.h>
}
#include <cstdlib>
#include <filesystem>
using json=nlohmann::json;

TEST_CASE("talent wipe prices follow resetTalentsCost in septims and game months", "[talents][talent-wipe]") {
    using TalentWipe::Cost;
    // 1, 5, 10, then 5 more each time to the cap of 50, all within a month of the last.
    TalentWipe::State last;
    std::vector<int> ladder;
    for (int i=0;i<12;++i) { last={Cost(last,100.0+i),100.0+i}; ladder.push_back(last.cost); }
    CHECK(ladder==std::vector<int>{1,5,10,15,20,25,30,35,40,45,50,50});
    // From 10 up, each whole month (30 game days) since the last wipe takes 5 off instead, to no less than 10.
    CHECK(Cost({50,10.0},39.9)==50);
    CHECK(Cost({50,10.0},40.0)==45);
    CHECK(Cost({50,10.0},70.0)==40);
    CHECK(Cost({50,10.0},1000.0)==10);
    CHECK(Cost({20,0.0},29.0)==25);
    CHECK(Cost({15,0.0},31.0)==10);
    CHECK(Cost({10,0.0},900.0)==10);
    // The first steps do not look at time, as on the server.
    CHECK(Cost({1,0.0},900.0)==5);
    CHECK(Cost({5,0.0},900.0)==10);
}

TEST_CASE("the talent record keeps the wipe price beside the allocation; older records were never wiped", "[talents][talent-wipe][persistence]") {
    json record={{"class",11},{"talents",{{"762",1}}}};
    CHECK(TalentWipe::Read(record).cost==0);
    CHECK(TalentWipe::Cost(TalentWipe::Read(record),500.0)==1);
    TalentWipe::Write(record,{15,42.5});
    const auto back=TalentWipe::Read(json::parse(record.dump()));
    CHECK(back.cost==15);
    CHECK(back.day==42.5);
    CHECK(record["talents"]==json{{"762",1}});
    for (const json bad : {json("5"),json(-1),json(51),json(2.5),json(nullptr)}) {
        auto damaged=record; damaged["reset_cost"]=bad; INFO(bad.dump()); CHECK(TalentWipe::Read(damaged).cost==0);
    }
    for (const json bad : {json("day"),json(-1.0),json(nullptr),json::array()}) {
        auto damaged=record; damaged["reset_day"]=bad; INFO(bad.dump()); CHECK(TalentWipe::Read(damaged).cost==0);
    }
}

TEST_CASE("a host talent wipe offer drives 1.12's CONFIRM_TALENT_WIPE popup, money frame and master distance", "[client][api][talents][talent-wipe]") {
    const char* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq=std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq,db,textures,1280,720);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L=ui.lua();
    auto run=[&](const char* script) {
        const int result=luaL_dostring(L,script);
        INFO(script); INFO((result ? lua_tostring(L,-1) : "")); REQUIRE(result==0);
    };
    const auto fixtures=std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/";
    for (const char* file : {"elunara.lua","hud_states.lua"}) REQUIRE(luaL_dofile(L,(fixtures+file).c_str())==0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    std::vector<std::string> wipes;
    wowcore::setHostHandler(L,[&](lua_State* state,const std::string& cmd) {
        if (cmd=="talentWipe") wipes.push_back(std::string(lua_tostring(state,2))+":"+lua_tostring(state,3));
        return 0;
    });
    run(R"(
        __WBLogin() __WBSkyrimMenus()
        assert(not CheckTalentMasterDist()) -- the recorded client until the first offer
        local probe=CreateFrame('Frame') probe:RegisterEvent('UI_ERROR_MESSAGE')
        probe:SetScript('OnEvent',function() __wbWipeError=arg1 end)
        __WBTalentWipe(3,5)
        local dialog=StaticPopup_FindVisible('CONFIRM_TALENT_WIPE')
        assert(dialog and dialog:IsVisible())
        local name=dialog:GetName()
        assert(getglobal(name..'Text'):GetText()==CONFIRM_TALENT_WIPE)
        assert(getglobal(name..'Button1'):GetText()==ACCEPT and getglobal(name..'Button2'):GetText()==CANCEL)
        assert(getglobal(name..'MoneyFrame'):IsVisible() and getglobal(name..'MoneyFrame').staticMoney==50000)
        assert(getglobal(name..'MoneyFrameGoldButton'):GetText()=='5')
        assert(CheckTalentMasterDist()==1)
        getglobal(name..'Button1'):Click('LeftButton')
        assert(not dialog:IsVisible())
        assert(CheckTalentMasterDist()==1) -- still at the shrine: a repeated accept finds nothing to unlearn
        __WBUiError('ERR_NOT_ENOUGH_MONEY') -- the host's answer when the player cannot pay
        assert(__wbWipeError==ERR_NOT_ENOUGH_MONEY)
        __WBTalentWipe(4,10)
        __wbWipeDialog=StaticPopup_FindVisible('CONFIRM_TALENT_WIPE')
        assert(__wbWipeDialog and getglobal(__wbWipeDialog:GetName()..'MoneyFrame').staticMoney==100000)
        __WBTalentWipe(nil) -- the player walked away from the shrine
        assert(not CheckTalentMasterDist())
        ConfirmTalentWipe() -- no offer: nothing asked of the host
    )");
    textures.sync(ui.frame(0)); // the popup's own OnUpdate checks the master distance
    textures.sync(ui.frame(1.0f/60));
    run("assert(not __wbWipeDialog:IsVisible() and not StaticPopup_FindVisible('CONFIRM_TALENT_WIPE'))");
    CHECK(wipes==std::vector<std::string>{"3:5"});
    CHECK(ui.engine().lastError().empty());
}
