#include <catch_amalgamated.hpp>
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/ui_session.hpp"
#include "wowcore/soft_raster.hpp"
#include "addons/lua_engine.hpp"
extern "C" {
#include <lauxlib.h>
}
#include <filesystem>
#include <cstdlib>

TEST_CASE("Testing panel browses without mutations and sends explicit validated operations", "[client][api][testing-panel]") {
    const std::filesystem::path data=WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW client absent");
    auto mpq=std::make_shared<wowcore::Mpq>(data); const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures; wowcore::UiSession ui(mpq,db,textures,1280,720);
    std::string error; REQUIRE(ui.start(&error)); auto* L=ui.lua();
    const auto run=[&](const char* code) { int rc=luaL_dostring(L,code); INFO((rc ? lua_tostring(L,-1) : code)); REQUIRE(rc==0); };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    run("__WBFixture('idle')"); REQUIRE(ui.loadInterface(&error));
    run(R"(
      __WBLogin() __WBSkyrimMenus()
      calls={} __WB.host=function(...) if arg[1]=='testCommand' or arg[1]=='menu' then tinsert(calls,arg) end end
      __WBTestCatalog({sets={{key='cenarion',name='Cenarion',count=8}},gear={{entry=873,name='Staff of Jordan'}},
        skills={{key='destruction',value=15},{key='restoration',value=20}},places={{key='riverwood'}}})
      SocialsMicroButton:Click()
      assert(WBTestPanel:IsShown() and not WBTestPanel:IsMovable())
      assert(table.getn(calls)==1 and calls[1][2]=='catalog')
      __WBTestResult({ok=true,message='Ready'})
      WBTestPanelTab2:Click()
      assert(table.getn(calls)==1)
      WBTestPanelValue1:SetText('35') WBTestPanelAction1:Click()
      assert(calls[2][2]=='skill' and calls[2][3]=='destruction' and calls[2][4]=='35')
      WBTestPanelAction2:Click() assert(table.getn(calls)==2)
      __WBTestResult({ok=false,message='Rejected'})
      assert(WBTestPanelStatus:GetText()=='Rejected')
      WBTestPanelValue2:SetText('1000') WBTestPanelAction2:Click()
      assert(calls[3][2]=='xp' and calls[3][3]=='destruction' and calls[3][4]=='1000')
      __WBTestResult({ok=true,message='XP granted'})
      WBTestPanelTab3:Click()
      assert(WBTestPanelRow1:GetText()=='Give set: Cenarion (8 pieces)')
      WBTestPanelRow1:Click() assert(calls[4][2]=='give' and calls[4][3]=='set' and calls[4][4]=='cenarion')
      __WBTestResult({ok=true})
      WBTestPanelSearch:SetText('jordan')
    )");
    for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
    run(R"(
      assert(WBTestPanelRow1:GetText()=='Staff of Jordan  [873]' and not WBTestPanelRow2:IsShown())
      WBTestPanelRow1:Click() assert(WBTestPanelValue1:GetText()=='873' and table.getn(calls)==4)
      WBTestPanelAction2:Click() assert(calls[5][2]=='equip' and calls[5][3]=='873')
      __WBTestResult({ok=true})
      WBTestPanelTab4:Click() WBTestPanelValue1:SetText('0000000F') WBTestPanelValue2:SetText('3') WBTestPanelAction1:Click()
      assert(calls[6][2]=='nativeitem' and calls[6][3]=='0000000F' and calls[6][4]=='3')
      __WBTestResult({ok=true})
      WBTestPanelTab5:Click() WBTestPanelAction1:Click()
      assert(calls[7][2]=='learnwow' and calls[7][3]=='5176')
      __WBTestResult({ok=true})
      WBTestPanelTab6:Click() WBTestPanelRow1:Click() assert(calls[8][2]=='travel' and calls[8][3]=='riverwood')
      __WBTestResult({ok=true})
      WBTestPanelTab7:Click() WBTestPanelValue1:SetText('2') WBTestPanelAction3:Click()
      assert(calls[9][2]=='spawn' and calls[9][3]=='chicken' and calls[9][4]=='2')
      __WBTestResult({ok=true}) WBTestPanelAction4:Click() assert(calls[10][2]=='clearspawn')
      __WBTestResult({ok=true})
      WBTestPanelTab1:Click() WBTestPanelAction8:Click()
      assert(not WBTestPanel:IsShown() and calls[11][1]=='menu' and calls[11][2]=='StatsMenu')
      ToggleFriendsFrame() assert(WBTestPanel:IsShown())
      __WBTestResult({ok=true}) WBTestPanelClose:Click() assert(not WBTestPanel:IsShown())
    )");
    run(R"(
      __WBMountCatalog({{key='swift_frostsaber',name='Swift Frostsaber',spell=23221}, {key='swift_palomino',name='Swift Palomino',spell=23227}})
      __WBTestResult({ok=true}) __WBTestPanelToggle() WBTestPanelTab8:Click()
      assert(table.getn(calls)==12 and calls[12][2]=='catalog')
      WBTestPanelRow1:Click()
      assert(calls[13][2]=='mount' and calls[13][3]=='swift_frostsaber')
      __WBTestResult({ok=true,command='mount'})
      __WBMountState({pending={name='Swift Frostsaber'}})
      assert(not IsMounted() and WBTestPanelStatus:GetText()=='Preparing Swift Frostsaber...')
      __WBMountState({active={name='Swift Frostsaber',key='swift_frostsaber',spell=23221}})
      assert(IsMounted()==1 and WBTestPanelStatus:GetText()=='Mounted: Swift Frostsaber')
      __WB.actions[30]={spell=23221} assert(IsCurrentAction(30)==1)
      assert(IsUsableAction(30)==1 and GetActionTexture(30))
      local seen=false for _,id in ipairs(__WB.spellbook) do if id==23221 then seen=true end end assert(seen)
      WBPreferredMount='swift_palomino'
      assert(not string.find(__WBSavedVariablesText(),'WBPreferredMount',1,true))
      local saved=__WBCharacterVariablesText() WBPreferredMount=nil assert(loadstring(saved))()
      assert(WBPreferredMount=='swift_palomino') WBPreferredMount='swift_frostsaber'
      __WBClassSpells({lines={},spells={},unlearned_spells={},known_spells={},highest_ranks={}})
      __WBSkyrimSpells({})
      local n=0 for _,id in ipairs(__WB.spellbook) do if id==23221 then n=n+1 end end assert(n==1)
      __WBInstallMounts()
      n=0 for _,id in ipairs(__WB.spellbook) do if id==23221 then n=n+1 end end assert(n==1)
      local p=__WB.resolve('player') local mounted
      for n,a in ipairs(p.buffs or {}) do if a.mount then mounted=n assert(a.spell==23221 and a.untilCancelled and not a.expires) end end
      assert(mounted)
      local before=table.getn(p.buffs)
      __WBMountState({active={name='Swift Frostsaber',key='swift_frostsaber',spell=23221}})
      assert(table.getn(p.buffs)==before) -- periodic snapshots cannot duplicate icons
      GameTooltip:SetOwner(UIParent,'ANCHOR_NONE') GameTooltip:SetPlayerBuff(mounted-1)
      assert(GameTooltipTextLeft1:GetText()=='Swift Frostsaber')
      CancelPlayerBuff(mounted-1) assert(calls[14][2]=='mount' and calls[14][3]=='none')
      __WBMountState({}) SlashCmdList.WBMOUNT('')
      assert(calls[15][3]=='swift_frostsaber')
      SlashCmdList.WBMOUNT('swift palomino') assert(calls[16][3]=='swift_palomino')
      __WBMountState({active={name='Swift Frostsaber',key='swift_frostsaber',spell=23221}}) SlashCmdList.WBMOUNT('')
      assert(calls[17][3]=='none')
      __WBMountState({}) assert(not IsMounted() and not IsCurrentAction(30))
      for _,a in ipairs(p.buffs or {}) do assert(not a.mount) end
      __WBTestSkills({{key='destruction',value=41},{key='restoration',value=20}})
      WBTestPanelTab2:Click() assert(WBTestPanelRow1:GetText()=='Destruction  (41)')
      __WBTestSkills({{key='destruction',value=42},{key='restoration',value=20}})
      assert(WBTestPanelRow1:GetText()=='Destruction  (42)' and WBTestPanel:IsShown())
      WBTestPanelTab5:Click() calls={}
      __WBGcd(30) __WBCooldown(29166,120)
      __WB.cooldowns[-42]={GetTime(),60} -- native power cooldown stays authoritative
      local casting={spell=5176} __WB.casting=casting
      __WBAuraAdd('player',774,false,12)
      WBTestPanelAction7:Click()
      assert(table.getn(calls)==1 and calls[1][2]=='cooldowns')
      __WBRefreshCooldowns()
      assert(not __WB.gcd and not __WB.cooldowns[29166] and __WB.cooldowns[-42] and __WB.casting==casting)
      local retained=false for _,a in ipairs(p.buffs) do if a.spell==774 then retained=true end end assert(retained)
      __WB.casting=nil __WB.cooldowns[-42]=nil __WBTestResult({ok=true}) WBTestPanelClose:Click()
    )");
    for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
    CHECK(ui.engine().lastError().empty());
    if (const char* out=std::getenv("WOWBRIDGE_TEST_PANEL_CAPTURE")) {
        run("ToggleFriendsFrame() __WBTestResult({ok=true,message='Testing tools ready'}) WBTestPanelTab3:Click()");
        ImDrawData* draw=nullptr;
        for (int i=0;i<300;++i) { draw=ui.frame(.016f); textures.sync(draw); }
        wowcore::Image im(1280,720); wowcore::rasterize(draw,textures,im);
        REQUIRE(wowcore::writePng(out,im));
    }
}
