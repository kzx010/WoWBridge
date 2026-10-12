#include <catch_amalgamated.hpp>
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/ui_session.hpp"
#include "wowcore/soft_raster.hpp"
#include "addons/lua_engine.hpp"
#include "core/app_clock.hpp"
#include "ui/widget_tree.hpp"
#include "ui/framexml_takeover.hpp"
extern "C" {
#include <lauxlib.h>
}
#include <cstdlib>
#include <filesystem>
#include <algorithm>

TEST_CASE("Live nameplates use client bindings, follow projections and yield to UI", "[client][api][nameplates]") {
    wowee::core::setAppTimeSeconds(0);
    // A previous session can leave a process-wide diagnostic request for the upstream 3.3.5 API.
    // Do not deliver that unrelated UnitAura probe to this fresh 1.12 session.
    (void)wowee::ui::frameXmlTakeProbeRequest();
    const auto* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env?env:WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq=std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq,db,textures,1280,720);
    std::string error;
    REQUIRE(ui.start(&error));
    auto* L=ui.lua();
    auto run=[&](const char* script) {
        const int result=luaL_dostring(L,script);
        INFO((result?lua_tostring(L,-1):"")); REQUIRE(result==0);
    };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    run("__WBFixture('idle'); emptyPlateApi=ShowNameplates");
    REQUIRE(ui.loadInterface(&error));
    run(R"(
      __WBLogin()
      assert(ShowNameplates==emptyPlateApi and not WBNameplate1)
      assert(GetBindingKey('NAMEPLATES')=='V' and GetBindingKey('FRIENDNAMEPLATES')=='SHIFT-V')
      local function unit(id,reaction,x,y)
        return {guid=id,name='Actor '..id,level=24,health=70,healthMax=100,reaction=reaction,
          attackable=reaction<=4,classification='normal',distance=12,x=x,y=y}
      end
      plateRows={unit(10,2,0.4,0.3),unit(11,4,0.6,0.4),unit(12,5,0.5,0.6)}
      __WBApplyLive({nameplates={epoch=7,units=plateRows}})
      assert(ShowNameplates~=emptyPlateApi and not __WBNameplate(10))
      RunBinding(GetBindingAction('V'),'down')
      assert(__WBNameplate(10) and __WBNameplate(11) and not __WBNameplate(12))
      assert(__WBNameplate(10).name:GetText()=='Actor 10')
      assert(__WBNameplate(10).health:GetValue()==70 and __WBNameplate(10).level:GetText()=='24')
      RunBinding(GetBindingAction('SHIFT-V'),'down')
      assert(not __WBNameplate(10) and __WBNameplate(12) and FRIENDNAMEPLATES_ON and not NAMEPLATES_ON)
      RunBinding(GetBindingAction('CTRL-V'),'down')
      assert(__WBNameplate(10) and __WBNameplate(11) and __WBNameplate(12))
      plateRows[1].classification='elite'
      plateRows[2].level=-1
      __WBApplyLive({target=plateRows[1],nameplates={epoch=7,units=plateRows}})
      assert(__WBNameplate(10).level:GetText()=='24+' and __WBNameplate(10).glow:IsShown())
      assert(UnitClassification('target')=='elite')
      assert(__WBNameplate(11).skull:IsShown() and not __WBNameplate(11).level:IsShown())
      plateRows[1].health=39 plateRows[1].healthMax=150
      __WBApplyLive({nameplates={epoch=7,units=plateRows}})
      assert(__WBNameplate(10).health:GetValue()==39 and select(2,__WBNameplate(10).health:GetMinMaxValues())==150)
      -- Repeated updates reuse the bounded pool rather than accumulating actor widgets.
      savedPlate=__WBNameplate(10)
    )");
    textures.sync(ui.frame(1.0f/60));
    auto& tree=ui.engine().widgets();
    auto* plate=tree.findByName("WBNameplate1");
    REQUIRE(plate); CHECK(plate->effStrata==wowee::ui::FrameStrata::World);
    for (int i=1;i<=3;++i) {
        const auto* bar=tree.findByName("WBNameplate"+std::to_string(i)+"Health");
        REQUIRE(bar);
        CHECK(bar->barColor[0]==(i==3?0:1));
        CHECK(bar->barColor[1]==(i==1?0:1));
        CHECK(bar->barColor[2]==0);
    }
    const auto beforeX=plate->left, beforeY=plate->bottom;
    const auto widgetCount=tree.size();
    run(R"(
      plateRows[1].x=0.45 plateRows[1].y=0.35
      for i=1,10 do __WBApplyLive({nameplates={epoch=7,units=plateRows}}) end
      assert(__WBNameplate(10)==savedPlate)
    )");
    textures.sync(ui.frame(1.0f/60));
    CHECK(tree.size()==widgetCount);
    CHECK(plate->left-beforeX==Catch::Approx(0.05*tree.findByName("WorldFrame")->rectW));
    CHECK(plate->bottom-beforeY==Catch::Approx(-0.05*tree.findByName("WorldFrame")->rectH));
    const float px=plate->left+64,py=plate->bottom+8;
    CHECK(tree.hitTest(px,py)==plate->id);
    // Exercise physical mouse dispatch: the queued command includes actor identity and load epoch.
    const float scale=tree.uiScale();
    ui.engine().dispatchMouse(px*scale,720-py*scale,720,{true,false,false});
    ui.engine().dispatchMouse(px*scale,720-py*scale,720,{});
    run("assert(__WB.log[table.getn(__WB.log)]=='nameplateTarget 10 7')");
    run(R"(
      cover=CreateFrame('Button','PlateCover',UIParent)
      cover:SetFrameStrata('BACKGROUND') cover:SetWidth(128) cover:SetHeight(36)
      cover:SetPoint('BOTTOMLEFT',savedPlate,'BOTTOMLEFT',0,0)
      cover:SetScript('OnClick',function() coverClicked=true end)
    )");
    textures.sync(ui.frame(1.0f/60));
    CHECK(tree.hitTest(px,py)==tree.findByName("PlateCover")->id);
    const auto& order=tree.drawOrder();
    const auto* border=tree.findByName("WBNameplate1Border");
    const auto* cover=tree.findByName("PlateCover");
    CHECK(std::find(order.begin(),order.end(),border)<std::find(order.begin(),order.end(),cover));
    ui.engine().dispatchMouse(px*scale,720-py*scale,720,{true,false,false});
    ui.engine().dispatchMouse(px*scale,720-py*scale,720,{});
    run(R"(
      assert(coverClicked) cover:Hide()
      function targetCommands()
        local n=0 for _,line in ipairs(__WB.log) do if string.find(line,'^nameplateTarget ') then n=n+1 end end
        return n
      end
      commandsBeforeSwap=targetCommands()
    )");
    textures.sync(ui.frame(1.0f/60));
    ui.engine().dispatchMouse(px*scale,720-py*scale,720,{true,false,false});
    run(R"(
      -- A new actor occupies the same recycled button while the mouse is held.
      replacement={guid=99,name='Replacement',level=24,health=100,healthMax=100,reaction=2,
        attackable=true,classification='normal',distance=12,x=0.45,y=0.35}
      __WBApplyLive({nameplates={epoch=7,units={replacement}}})
    )");
    textures.sync(ui.frame(1.0f/60));
    ui.engine().dispatchMouse(px*scale,720-py*scale,720,{});
    run("assert(targetCommands()==commandsBeforeSwap)");
    run(R"(
      plateRows[1].distance=20.001 plateRows[2].x=-0.01 plateRows[3].dead=true
      __WBApplyLive({nameplates={epoch=7,units=plateRows}})
      assert(not __WBNameplate(10) and not __WBNameplate(11) and not __WBNameplate(12))
      -- Missing actors hide recycled buttons; later clicks cannot send their old identities.
      local n=table.getn(__WB.log)
      savedPlate:Click() assert(table.getn(__WB.log)==n)
      plateRows[1].distance=20 plateRows[2].x=0.6 plateRows[3].dead=false
      __WBApplyLive({nameplates={epoch=8,units=plateRows}})
      assert(__WBNameplate(10) and __WBNameplate(10).epoch==8)
      __WBApplyLive({nameplates={epoch=7,units={}}}) -- an older world's delayed publish cannot erase fresh rows
      assert(__WBNameplate(10) and __WBNameplate(10).epoch==8)
      RunBinding('FRIENDNAMEPLATES','down')
      assert(__WBNameplate(12) and not __WBNameplate(10))
      -- Even a login replay in the current session retains the friendly switch.
      __WBLogin() assert(__WBNameplate(12) and not __WBNameplate(10))
      nameplateReload=__WBReloadStateText()
    )");
    lua_getglobal(L,"nameplateReload"); const std::string saved=lua_tostring(L,-1); lua_pop(L,1);
    wowcore::UiSession fresh(mpq,db,textures,2560,1440);
    REQUIRE(fresh.start(&error)); REQUIRE(fresh.loadInterface(&error));
    REQUIRE(luaL_dostring(fresh.lua(),saved.c_str())==0);
    const int result=luaL_dostring(fresh.lua(),R"(
      __WBLogin()
      assert(FRIENDNAMEPLATES_ON and not NAMEPLATES_ON)
      assert(__WBNameplate(12) and not __WBNameplate(10))
      assert(__WBNameplate(12).name:GetText()=='Actor 12')
      RunBinding('NAMEPLATES','down') assert(__WBNameplate(10) and not __WBNameplate(12))
    )");
    INFO((result?lua_tostring(fresh.lua(),-1):"")); REQUIRE(result==0);
    textures.sync(fresh.frame(1.0f/60));
    const auto* reloaded=fresh.engine().widgets().findByName("WBNameplate1");
    REQUIRE(reloaded); CHECK(reloaded->visible);
    CHECK((reloaded->left+reloaded->rectW/2)*fresh.engine().widgets().uiScale()==Catch::Approx(0.45*2560));
    // Changing only UI scale keeps the actor's pixel anchor fixed on the next frame.
    fresh.engine().widgets().setUserScale(0.8f);
    textures.sync(fresh.frame(1.0f/60));
    CHECK((reloaded->left+reloaded->rectW/2)*fresh.engine().widgets().uiScale()==Catch::Approx(0.45*2560));
    INFO(ui.engine().lastError());
    CHECK(ui.engine().lastError().empty());
    INFO(fresh.engine().lastError());
    CHECK(fresh.engine().lastError().empty());
    REQUIRE(mpq->exists("Interface\\Tooltips\\Nameplate-Border.blp"));
    REQUIRE(mpq->exists("Interface\\Tooltips\\Nameplate-Glow.blp"));
}
