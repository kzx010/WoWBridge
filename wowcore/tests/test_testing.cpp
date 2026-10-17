#include <catch_amalgamated.hpp>
#include "wowcore/dbc.hpp"
#include "wowcore/item_preview.hpp"
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
    const char* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
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
      __WBMountCatalog({{key='swift_frostsaber',name='Swift Frostsaber',spell=23221,learned=true}, {key='swift_palomino',name='Swift Palomino',spell=23227,learned=true}})
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
    run(R"(
      __WBTestPanelToggle() calls={}
      __WB.skyrimSpells={[-4261416960]={name='Light Plugin Spell'}} -- 0xFE001000, a light plugin's form ID
      WBTestPanelTab5:Click() WBTestPanelSearch:SetText('light plugin')
    )");
    for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
    run(R"(
      assert(WBTestPanelRow1:GetText()=='Skyrim: Light Plugin Spell [FE001000]')
      WBTestPanelAction1:Click() local sent=table.getn(calls) assert(sent==1)
      WBTestPanelAction1:Click() assert(table.getn(calls)==sent) -- a pending command blocks another
      __WB.testPanelPending=GetTime()-20 WBTestPanelAction1:Click()
      assert(table.getn(calls)==sent+1) -- a reply that never came no longer locks the panel
      __WBTestResult({ok=true,pending=true}) assert(type(__WB.testPanelPending)=='number')
      __WBTestResult({ok=true}) WBTestPanelClose:Click()
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

TEST_CASE("The Gear page searches every WoW item and the Armory page gives random ones", "[client][api][testing-panel][armory]") {
    const char* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
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
      -- Console.cpp's catalog: WoWBridge.esp's sets and items, the Armory's sets (by ItemSet.dbc ID) and, once a
      -- session, its items as {entry, name, Skyrim level, quality}.
      __WBTestCatalog({sets={{key='cenarion',name='Cenarion',count=8},{key='211',name='Vestments of Transcendence',count=8,level=48}},
        gear={{entry=873,name='Staff of Jordan'}},armory={{12640,'Lionheart Helm',37,4},{2059,'Sentry Cloak',11,3},{6123,"Novice's Robe",1,1}}})
      __WBTestCatalog({sets={{key='cenarion',name='Cenarion',count=8}},gear={{entry=873,name='Staff of Jordan'}}})
      assert(table.getn(__WB.testPanelArmory)==3) -- a later catalog leaves them
      __WBTestPanelToggle() __WBTestResult({ok=true}) calls={}
      WBTestPanelTab3:Click() WBTestPanelSearch:SetText('lionheart')
    )");
    for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
    run(R"(
      assert(WBTestPanelRow1:GetText()=='Lionheart Helm  [12640]  Epic 37' and not WBTestPanelRow2:IsShown())
      WBTestPanelRow1:Click() assert(WBTestPanelValue1:GetText()=='12640' and WBTestPanelRow1.marker:IsShown())
      WBTestPanelAction2:Click() assert(calls[1][2]=='equip' and calls[1][3]=='12640')
      __WBTestResult({ok=true}) WBTestPanelValue1:SetText("Novice's Robe") WBTestPanelAction1:Click()
      assert(calls[2][2]=='give' and calls[2][3]=="Novice's Robe") -- by name
      __WBTestResult({ok=true}) WBTestPanelSearch:SetText('cloak')
    )");
    for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
    run(R"(
      assert(WBTestPanelRow1:GetText()=='Sentry Cloak  [2059]  Rare 11')
      WBTestPanelTab14:Click()
      assert(WBTestPanelValue1:GetText()==tostring(math.min(52,UnitLevel('player'))) and WBTestPanelValue2:GetText()=='2')
      WBTestPanelValue1:SetText('30') WBTestPanelValue2:SetText('3')
      local function sent(button,command,arg,value)
        __WBTestResult({ok=true}) local before=table.getn(calls) button:Click()
        assert(table.getn(calls)==before+1,command..' not sent from '..button:GetName())
        local c=calls[table.getn(calls)] assert(c[2]==command and c[3]==arg and c[4]==(value or ''),command..' '..tostring(c[3])..' '..tostring(c[4]))
      end
      sent(WBTestPanelAction1,'random','30','3') sent(WBTestPanelAction2,'randomequip','30','3')
      sent(WBTestPanelAction3,'randomset','30') sent(WBTestPanelAction5,'lootroll','100')
      for i,op in ipairs({'on','off','status'}) do
        local button=getglobal('WBTestPanelAction'..(i+6))
        assert(button:GetText()=='NPC shoulders '..op)
        sent(button,'npcshoulders',op)
      end
      for i,op in ipairs({'on','off','status'}) do
        local button=getglobal('WBTestPanelRow'..i)
        assert(button:GetText()=='NPC weapons '..op)
        sent(button,'npcweapons',op)
      end
      for i,op in ipairs({'on','off','status'}) do
        local button=getglobal('WBTestPanelRow'..(i+3))
        assert(button:GetText()=='Player Skyrim weapons '..op)
        sent(button,'playerweapons',op)
      end
      for i,op in ipairs({'on','off','status'}) do
        local button=getglobal('WBTestPanelRow'..(i+6))
        assert(button:GetText()=='NPC helms '..op)
        sent(button,'npchelms',op)
      end
      WBTestPanelSearch:SetText('epic')
    )");
    for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
    run(R"(
      WBTestPanelRow1:Click() assert(WBTestPanelValue2:GetText()=='4')
      __WBTestResult({ok=true}) WBTestPanelAction4:Click()
      local c=calls[table.getn(calls)] assert(c[1]=='menu' and c[2]=='InventoryMenu')
    )");
    CHECK(ui.engine().lastError().empty());
}

TEST_CASE("Flying mounts the client lacks are General spells with 1.12 mount tooltips", "[client][api][testing-panel][mount]") {
    const char* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW client absent");
    auto mpq=std::make_shared<wowcore::Mpq>(data); const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures; wowcore::UiSession ui(mpq,db,textures,1280,720);
    std::string error; REQUIRE(ui.start(&error)); auto* L=ui.lua();
    const auto run=[&](const char* code) { int rc=luaL_dostring(L,code); INFO((rc ? lua_tostring(L,-1) : code)); REQUIRE(rc==0); };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    run("__WBFixture('idle')"); REQUIRE(ui.loadInterface(&error));
    run(R"(
      __WBLogin() __WBSkyrimMenus()
      calls={} __WBHost=function(...) if arg[1]=='testCommand' or arg[1]=='useAction' then tinsert(calls,arg) end end
      -- data/mounts.json rows (Mounts.cpp sends the catalog as is)
      local saber={key='swift_frostsaber',spell=23221,name='Swift Frostsaber',display=14331,speed_percent=100,cast_ms=3000,
        icon='Interface\\Icons\\Ability_Mount_PinkTiger',learned=true}
      local gryphon={key='swift_golden_gryphon',spell=32235,name='Swift Golden Gryphon',display=6852,speed_percent=100,
        cast_ms=3000,icon='Interface\\Icons\\Ability_Hunter_Pet_Owl',flying=true,flight_speed_percent=280,learned=true,
        description='Summons and dismisses a rideable Gryphon.  This is a very fast mount that can fly outdoors.'}
      local rider={key='swift_green_wind_rider',spell=32295,name='Swift Green Wind Rider',display=11011,speed_percent=100,
        cast_ms=3000,icon='Interface\\Icons\\Ability_Hunter_Pet_Bat',flying=true,flight_speed_percent=280,learned=true,
        description='Summons and dismisses a rideable Wind Rider.  This is a very fast mount that can fly outdoors.'}
      assert(not __WBSpellInfo(32235) and not __WBSpellInfo(32295)) -- not 1.12 spells
      __WBMountCatalog({saber,gryphon,rider})
      -- The synthetic spells behave as the client's swift mounts do.
      local real,fake=__WB.spellInfo(23221),__WB.spellInfo(32295)
      for _,k in ipairs({'rank','passive','castTime','rangeMin','rangeMax','rangeIndex','cost','costPct','powerType','duration',
        'recovery','categoryRecovery','gcd','targetsUnit','school','attributes','attributesEx','attributesExB','spellLevel',
        'baseLevel','maxLevel','procCharges','procChance','stances','stancesNot'}) do
        assert(real[k]==fake[k],k..': '..tostring(real[k])..' ~= '..tostring(fake[k]))
      end
      assert(fake.id==32295 and fake.name=='Swift Green Wind Rider' and fake.icon==rider.icon)
      local name,_,offset,count=GetSpellTabInfo(1) assert(name=='General')
      local at={} for i=offset+1,offset+count do at[GetSpellName(i,'spell')]=i end
      assert(at['Swift Frostsaber'] and at['Swift Golden Gryphon'] and at['Swift Green Wind Rider'])
      assert(GetSpellTexture(at['Swift Golden Gryphon'],'spell')=='Interface\\Icons\\Ability_Hunter_Pet_Owl')
      local _,rank=GetSpellName(at['Swift Green Wind Rider'],'spell') assert(rank=='')
      local function lines() local t={} for i=1,GameTooltip:NumLines() do t[i]=getglobal('GameTooltipTextLeft'..i):GetText() end return t end
      GameTooltip:SetOwner(UIParent,'ANCHOR_NONE') GameTooltip:SetSpell(at['Swift Frostsaber'],'spell')
      local ground=lines()
      GameTooltip:SetSpell(at['Swift Green Wind Rider'],'spell')
      local t=lines()
      assert(table.getn(t)==3 and table.getn(ground)==3)
      assert(t[1]=='Swift Green Wind Rider' and t[2]=='3 sec cast' and t[2]==ground[2] and t[3]==rider.description)
      -- Drag to an action bar, use, highlight, and the mounted buff.
      PickupSpell(at['Swift Golden Gryphon'],'spell') assert(CursorHasSpell()) PlaceAction(30) assert(not CursorHasSpell())
      assert(__WB.actions[30].spell==32235 and GetActionTexture(30)==gryphon.icon and IsUsableAction(30)==1)
      GameTooltip:SetAction(30) t=lines() assert(t[1]=='Swift Golden Gryphon' and t[2]=='3 sec cast' and t[3]==gryphon.description)
      UseAction(30) assert(calls[1][1]=='useAction' and calls[1][3]==32235)
      __WBMountState({pending=gryphon}) assert(IsCurrentAction(30)==1 and not IsMounted())
      __WBMountState({active=gryphon}) assert(IsCurrentAction(30)==1 and IsMounted()==1)
      local p=__WB.resolve('player') local mounted
      for n,a in ipairs(p.buffs) do if a.mount then mounted=n assert(a.spell==32235 and a.texture==gryphon.icon) end end
      GameTooltip:SetPlayerBuff(mounted-1) t=lines()
      assert(t[1]=='Swift Golden Gryphon' and t[2]=='Increases ground movement speed by 100% and flight speed by 280%. Right-click to dismount.')
      __WBMountState({}) assert(not IsCurrentAction(30))
      -- /mount keys and the saved choice
      SlashCmdList.WBMOUNT('swift green wind rider') assert(calls[2][2]=='mount' and calls[2][3]=='swift_green_wind_rider')
      WBPreferredMount='swift_golden_gryphon' SlashCmdList.WBMOUNT('') assert(calls[3][3]=='swift_golden_gryphon')
      -- The class and Skyrim books keep them; a catalog republish never duplicates them.
      __WBClassSpells({lines={},spells={},unlearned_spells={},known_spells={},highest_ranks={}})
      __WBSkyrimSpells({}) __WBMountCatalog({saber,gryphon,rider}) __WBInstallMounts()
      local n=0 for _,id in ipairs(__WB.spellbook) do if id==32235 or id==32295 then n=n+1 end end assert(n==2)
      assert(__WB.actions[30].spell==32235 and __WB.spellInfo(32235).name=='Swift Golden Gryphon')
      -- The Mounts testing page offers them with their flight speed.
      __WBTestPanelToggle() __WBTestResult({ok=true}) WBTestPanelTab8:Click()
      assert(WBTestPanelRow2:GetText()=='Swift Golden Gryphon  (+280% flight speed)')
      assert(WBTestPanelRow3:GetText()=='Swift Green Wind Rider  (+280% flight speed)')
      WBTestPanelRow3:Click() local last=calls[table.getn(calls)]
      assert(last[2]=='mount' and last[3]=='swift_green_wind_rider')
      -- Its routes for learning: forget every mount (a new game), restore them, give reins (all, or one).
      for i,op in ipairs({{'mounts','forget'},{'mounts','restore'},{'givereins','all'}}) do
        __WBTestResult({ok=true}) getglobal('WBTestPanelAction'..(i+1)):Click()
        last=calls[table.getn(calls)] assert(last[2]==op[1] and last[3]==op[2])
      end
      __WBTestResult({ok=true}) __WBTestCatalog({reins={{key='swift_frostsaber',name='Reins of the Swift Frostsaber'}}})
      assert(WBTestPanelRow4:GetText()=='Give Reins of the Swift Frostsaber')
      WBTestPanelRow4:Click() last=calls[table.getn(calls)] assert(last[2]=='givereins' and last[3]=='swift_frostsaber')
      -- A mount the player has not learned stays on the page, marked.
      rider.learned=false __WBMountCatalog({saber,gryphon,rider}) __WBTestResult({ok=true}) WBTestPanelTab8:Click()
      assert(WBTestPanelRow3:GetText()=='Swift Green Wind Rider  (+280% flight speed) - not learned')
      local n=0 for _,id in ipairs(__WB.spellbook) do if id==32295 then n=n+1 end end assert(n==0)
      rider.learned=true
      -- A catalog without them withdraws their spell info.
      __WBMountCatalog({saber}) assert(not __WB.spellInfo(32295))
    )");
    CHECK(ui.engine().lastError().empty());
}

TEST_CASE("Every populated testing page fits its text and keeps controls apart", "[client][api][testing-panel][layout]") {
    const char* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW client absent");
    auto mpq=std::make_shared<wowcore::Mpq>(data); const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures; wowcore::UiSession ui(mpq,db,textures,1280,720);
    std::string error; REQUIRE(ui.start(&error)); auto* L=ui.lua();
    const auto run=[&](const char* code) { int rc=luaL_dostring(L,code); INFO((rc ? lua_tostring(L,-1) : code)); REQUIRE(rc==0); };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    run("__WBFixture('idle')"); REQUIRE(ui.loadInterface(&error));
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/hud_states.lua").c_str())==0);
    run(R"(
      __WBLogin() __WBSkyrimMenus() __WBTestingFixture()
      calls={} __WB.host=function(...) if arg[1]=='testCommand' then tinsert(calls,arg) end end
      __WBTestPanelToggle() __WBTestResult({ok=true,message='Testing tools ready'})
      function __checkTestingLayout()
        local nodes={}
        local function fits(f,w,h,wrapped)
          local name=f:GetName() or f:GetText()
          assert(f:GetStringHeight()<=h+.1,name..' text too tall: '..f:GetStringHeight()..' > '..h..' width='..f:GetStringWidth()..' box='..w..' text='..f:GetText())
          if not wrapped then assert(f:GetStringWidth()<=w+.1,name..' text too wide: '..f:GetStringWidth()..' > '..w) end
        end
        local function add(f,wrapped)
          if not f:IsVisible() then return end
          local x,y,w,h=f:GetRect()
          assert(x and w>0 and h>0,f:GetName()..' has no rect')
          local px,py,pw,ph=WBTestPanel:GetRect()
          assert(x>=px and y>=py and x+w<=px+pw+.1 and y+h<=py+ph+.1,f:GetName()..' outside panel')
          tinsert(nodes,{f=f,x=x,y=y,w=w,h=h})
          if f:GetObjectType()=='FontString' then fits(f,w,h,wrapped)
          elseif f:GetObjectType()=='Button' and f:GetText() then
            local fs=f:GetFontString() local fx,fy,fw,fh=fs:GetRect()
            fits(fs,math.min(fw,w-12),math.min(fh,h),false)
            assert(fx>=x and fy>=y-.1 and fx+fw<=x+w+.1 and fy+fh<=y+h+.1,f:GetName()..' label outside button')
          elseif f:GetObjectType()=='EditBox' then fits(f,w-12,h,false) end
        end
        for _,f in ipairs({WBTestPanelTitle,WBTestPanelDescription,WBTestPanelValue1Label,WBTestPanelValue2Label,
          WBTestPanelValue1,WBTestPanelValue2,WBTestPanelSearchLabel,WBTestPanelSearch,WBTestPanelPrev,WBTestPanelNext,
          WBTestPanelCount,WBTestPanelRefresh,WBTestPanelStatus,WBTestPanelClose}) do
          add(f,f==WBTestPanelDescription or f==WBTestPanelStatus)
        end
        for i=1,9 do add(getglobal('WBTestPanelAction'..i)) end
        for i=1,15 do add(getglobal('WBTestPanelTab'..i)) end
        for i=1,11 do
          local f=getglobal('WBTestPanelRow'..i) add(f)
          if f:IsVisible() and f.marker:IsVisible() then
            local x,y,w,h=f.marker:GetRect() local fs=f:GetFontString()
            fits(f.marker,w,h,false) assert(x+w<=fs:GetLeft(),'selection marker overlaps label')
          end
        end
        for i,a in ipairs(nodes) do for j=i+1,table.getn(nodes) do local b=nodes[j]
          local overlapX=math.min(a.x+a.w,b.x+b.w)-math.max(a.x,b.x)
          local overlapY=math.min(a.y+a.h,b.y+b.h)-math.max(a.y,b.y)
          assert(overlapX<=.1 or overlapY<=.1,a.f:GetName()..' overlaps '..b.f:GetName())
        end end
      end
    )");
    for (int page=1;page<=15;++page) {
        INFO("testing page " << page);
        run(("getglobal('WBTestPanelTab"+std::to_string(page)+"'):Click()").c_str());
        for (int batch=0;batch<3;++batch) {
            for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
            run("__checkTestingLayout() WBTestPanelNext:Click()");
        }
        run("WBTestPanelSearch:SetText('no such catalog entry')");
        for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
        run("__checkTestingLayout() assert(WBTestPanelCount:GetText()=='No matching entries')");
    }
    run(R"(
      assert(table.getn(calls)==1) -- browsing, filtering and paging only requested the initial catalog
      WBTestPanelTab2:Click() WBTestPanelSearch:SetText('destruction')
    )");
    for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
    run(R"(
      assert(WBTestPanelRow1.marker:IsShown())
      WBTestPanelSearch:SetText('restoration')
    )");
    for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
    run(R"(
      WBTestPanelRow1:Click()
      assert(WBTestPanelRow1.marker:IsShown() and WBTestPanelAction1:GetText()=='Set Restoration')
      WBTestPanelAction9:Click() assert(WBTestPanelValue1:GetText()=='100')
      WBTestPanelAction1:Click() assert(calls[2][2]=='skill' and calls[2][3]=='restoration' and calls[2][4]=='100')
      __WBTestResult({ok=true}) WBTestPanelTab3:Click() WBTestPanelSearch:SetText('jordan')
    )");
    for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
    run(R"(
      WBTestPanelRow1:Click()
      assert(WBTestPanelRow1.marker:IsShown())
      WBTestPanelValue1:SetText('1168')
    )");
    for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
    run(R"(
      assert(not WBTestPanelRow1.marker:IsShown())
      WBTestPanelTab7:Click() WBTestPanelValue1:SetText('5')
      for i,key in ipairs({'bandit','bear','skeever','mudcrab'}) do
        __WBTestResult({ok=true}) getglobal('WBTestPanelAction'..(i+5)):Click()
        local c=calls[table.getn(calls)] assert(c[2]=='spawn' and c[3]==key and c[4]=='5')
      end
      __WBTestResult({ok=true}) WBTestPanelRow1:Click()
      local c=calls[table.getn(calls)] assert(c[2]=='spawn' and c[3]=='townsperson' and c[4]=='5')
      WBTestPanelTab9:Click()
      for i,op in ipairs({{'time','12'},{'weather','clear'},{'bounty','clear'},{'god',''},
        {'target','heal'},{'target','kill'},{'target','revive'}}) do
        __WBTestResult({ok=true}) getglobal('WBTestPanelAction'..i):Click()
        local c=calls[table.getn(calls)] assert(c[2]==op[1] and c[3]==op[2])
      end
      __WBTestResult({ok=true}) WBTestPanelRow4:Click()
      c=calls[table.getn(calls)] assert(c[2]=='time' and c[3]=='0')
      __WBTestResult({ok=true,message='level 60: setlevel skips attribute choices. Earn skill XP (player.advskill SKILL XP), then click the XP bar or use Shift+Tab > Skills for Skyrim Health/Magicka/Stamina choices'})
    )");
    for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
    run("__checkTestingLayout()");
    run(R"(
      local function text(f,v) f:SetText(v) f:GetScript('OnTextChanged')() end
      WBTestPanelTab10:Click()
      assert(WBTestPanelValue1:GetText()=='druid' and WBTestPanelValue2:GetText()=='0')
      assert(WBTestPanelRow1:IsShown()) -- initial page defaults must populate the list
      text(WBTestPanelSearch,'wrath rank 2')
      assert(string.find(WBTestPanelRow1.row.text,'Unknown; Destruction 23',1,true),WBTestPanelRow1.row.text)
      WBTestPanelRow1:Click() assert(WBTestPanelRow1.marker:IsShown())
      local function sent(button,command,arg,value)
        __WBTestResult({ok=true}) local before=table.getn(calls) button:Click()
        assert(table.getn(calls)==before+1,command..' not sent from '..button:GetName())
        local c=calls[table.getn(calls)] assert(c[1]=='testCommand' and c[2]==command and c[3]==arg and c[4]==(value or ''),command..' '..tostring(c[2])..' '..tostring(c[3])..' '..tostring(c[4]))
      end
      sent(WBTestPanelAction1,'learnwow','5177','druid')
      sent(WBTestPanelAction2,'learnchain','5176','druid')
      text(WBTestPanelValue2,'1') sent(WBTestPanelAction3,'learntab','1','druid')
      sent(WBTestPanelAction4,'givebook','5177') sent(WBTestPanelAction5,'givebook','chain','5176')
      sent(WBTestPanelAction6,'givebook','all')
      -- An authoritative class publication changes the known marker without a new catalog.
      __WBProgressionFixture(true)
    )");
    for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
    run(R"(
      assert(string.find(WBTestPanelRow1.row.text,'Known; Destruction 23',1,true))
      assert(WBTestPanelRow1.marker:IsShown())
      WBTestPanelAction9:Click() assert(WBTestPanelValue2:GetText()=='2')
      WBTestPanelAction9:Click() assert(WBTestPanelValue2:GetText()=='3')
      WBTestPanelAction9:Click() assert(WBTestPanelValue2:GetText()=='0')
      local function text(f,v) f:SetText(v) f:GetScript('OnTextChanged')() end
      local function sent(button,command,arg,value)
        __WBTestResult({ok=true}) local before=table.getn(calls) button:Click()
        assert(table.getn(calls)==before+1,command..' not sent from '..button:GetName())
        local c=calls[table.getn(calls)] assert(c[1]=='testCommand' and c[2]==command and c[3]==arg and c[4]==(value or ''))
      end
      text(WBTestPanelSearch,'') WBTestPanelAction8:Click()
      assert(WBTestPanelValue1:GetText()=='mage')
      text(WBTestPanelValue2,'0')
      assert(string.find(WBTestPanelRow1.row.text,'Fire / Fireball Rank 1 - Unknown',1,true))
      WBTestPanelRow1:Click() sent(WBTestPanelAction1,'learnwow','133','mage')
      WBTestPanelAction7:Click() assert(WBTestPanelValue1:GetText()=='druid')
      sent(WBTestPanelRefresh,'catalog','')
      WBTestPanelTab11:Click()
      local ops={{'merchant','spawn'},{'merchant','open'},{'merchant','remove'},
        {'gold','100'},{'gold','1000'},{'gold','10000'},
        {'lootchest','spawn'},{'lootchest','open'},{'lootchest','remove'}}
      for i,op in ipairs(ops) do sent(getglobal('WBTestPanelAction'..i),op[1],op[2]) end
      local routes={{'passive beast','scenario','beast'},{'guard and townsperson','scenario','observers'},
        {'spawn shrine','scenario','shrine'},{'wipe price','talents','price'},{'allocated talents','talents','reset'},
        {'total talent points','talentpoints','0'},{'varied items','scenario','inventory'},
        {'seeds and resurrection','scenario','reagents'},{'improve / enchant','scenario','crafting'},{'scenario actors','clearspawn',''},
        {'10000 HP dummy','spawn','dummy','1'},{'wolf corpse','scenario','corpse'},{'two still wolves','scenario','cc'},
        {'Refill health','heal',''},{'Clear cooldowns','cooldowns',''},{'Reset forms','reset',''}}
      for _,op in ipairs(routes) do text(WBTestPanelSearch,op[1])
        assert(WBTestPanelRow1:IsShown()) sent(WBTestPanelRow1,op[2],op[3],op[4]) end
      WBTestPanelTab12:Click()
      assert(table.getn(__WB.testPanelCatalog.emotes)>100) -- all 1.12 tokens, not a handpicked subset
      text(WBTestPanelSearch,'dance') WBTestPanelRow1:Click()
      assert(WBTestPanelValue1:GetText()=='DANCE' and WBTestPanelRow1.marker:IsShown())
      sent(WBTestPanelAction1,'emote','DANCE') sent(WBTestPanelAction2,'emote','stand')
      sent(WBTestPanelAction4,'say','') sent(WBTestPanelAction5,'say','long')
      WBTestPanelTab13:Click()
      for i,attr in ipairs({'health','magicka','stamina'}) do sent(getglobal('WBTestPanelAction'..i),'levelup',attr,'1') end
      text(WBTestPanelSearch,'Attribute: Stamina') WBTestPanelRow1:Click()
      assert(WBTestPanelRow1.marker:IsShown() and WBTestPanelValue1:GetText()=='stamina')
      text(WBTestPanelSearch,'Levels: 10') WBTestPanelRow1:Click()
      sent(WBTestPanelAction4,'levelup','stamina','10') sent(WBTestPanelAction6,'levelup','cancel')
      WBTestPanelTab3:Click() sent(WBTestPanelAction7,'showhelm','toggle') sent(WBTestPanelAction8,'showcloak','toggle')
      WBTestPanelTab10:Click() WBTestPanelAction7:Click()
      assert(WBTestPanelValue1:GetText()=='mage') -- Previous wraps at the first class
      WBTestPanelAction8:Click() assert(WBTestPanelValue1:GetText()=='druid')
      -- The remaining Features routes open their real UI or native host menu.
      WBTestPanelTab11:Click() text(WBTestPanelSearch,'open inventory')
      __WBTestResult({ok=true}) __WB.host=function(...) tinsert(calls,arg) end
      WBTestPanelRow1:Click() local c=calls[table.getn(calls)] assert(c[1]=='menu' and c[2]=='InventoryMenu' and not WBTestPanel:IsShown())
      WBTestPanel:Show() text(WBTestPanelSearch,'open talents') WBTestPanelRow1:Click() assert(TalentFrame:IsShown()) HideUIPanel(TalentFrame)
      WBTestPanel:Show() text(WBTestPanelSearch,'open world map') WBTestPanelRow1:Click() c=calls[table.getn(calls)] assert(c[1]=='menu' and c[2]=='MapMenu')
      WBTestPanel:Show() text(WBTestPanelSearch,'open macros') WBTestPanelRow1:Click() assert(MacroFrame and MacroFrame:IsShown()) HideUIPanel(MacroFrame)
      WBTestPanel:Show() WBTestPanelTab13:Click() WBTestPanelAction5:Click()
      c=calls[table.getn(calls)] assert(c[1]=='menu' and c[2]=='StatsMenu')
      __WBTestResult({ok=true})
    )");
    CHECK(ui.engine().lastError().empty());
}

TEST_CASE("Paladin testing routes browse without casting and send explicit actions", "[client][api][testing-panel][paladin]") {
    const char* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW client absent");
    auto mpq=std::make_shared<wowcore::Mpq>(data); const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures; wowcore::UiSession ui(mpq,db,textures,1280,720);
    std::string error; REQUIRE(ui.start(&error)); auto* L=ui.lua();
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    REQUIRE(luaL_dostring(L,"__WBFixture('idle')")==0); REQUIRE(ui.loadInterface(&error));
    const auto run=[&](const char* code) { const int rc=luaL_dostring(L,code); INFO((rc ? lua_tostring(L,-1) : code)); REQUIRE(rc==0); };
    run(R"(
      __WBLogin() calls={}
      __WB.host=function(...) if arg[1]=='testCommand' then tinsert(calls,arg) end end
      __WBTestPanelToggle() __WBTestResult({ok=true}) WBTestPanelTab15:Click()
      assert(table.getn(calls)==1 and calls[1][2]=='catalog')
      assert(WBTestPanelRow1:GetText()=='Holy Light')
      for i=1,9 do assert(string.len(getglobal('WBTestPanelAction'..i):GetText() or '')<=32) end
      for i=1,11 do assert(string.len(getglobal('WBTestPanelRow'..i):GetText() or '')<=32) end
      WBTestPanelRow1:Click()
      assert(calls[2][2]=='paladin' and calls[2][3]=='holy_light')
      __WBTestResult({ok=true}) WBTestPanelValue1:SetText('37') WBTestPanelAction3:Click()
      assert(calls[3][2]=='paladin' and calls[3][3]=='hit' and calls[3][4]=='37')
      __WBTestResult({ok=true}) WBTestPanelSearch:SetText('Consecration')
    )");
    for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
    run(R"(
      assert(table.getn(calls)==3 and WBTestPanelRow1:GetText()=='Consecration')
      WBTestPanelRow1:Click() assert(calls[4][2]=='paladin' and calls[4][3]=='consecration')
      __WBTestResult({ok=true}) WBTestPanelAction1:Click()
      assert(calls[5][2]=='paladin' and calls[5][3]=='status')
    )");
    // Every new behavior has a searchable, explicit route; browsing must never cast or hit a follower.
    for (const auto& [label, action] : std::vector<std::pair<std::string, std::string>>{
        {"Devotion Aura","devotion"}, {"Retribution Aura","retribution"}, {"Concentration Aura","concentration"},
        {"Native fear resistance probe","fear_probe"}, {"Native paralysis (stun) probe","paralysis_probe"},
        {"Native stagger resist probe","stagger_probe"}, {"Hostile interruption probe","interrupt_probe"},
        {"Fire Resistance Aura","fire_aura"}, {"Frost Resistance Aura","frost_aura"}, {"Shadow Resistance Aura","shadow_aura"},
        {"Sanctity Aura","sanctity"}, {"Blessing of Sanctuary","sanctuary"}, {"Blessing of Sacrifice","sacrifice"},
        {"Blessing of Freedom","freedom"}, {"Holy Shock: heal or strike","holy_shock"},
        {"Party hit: first follower","party_hit"}, {"Party block: first follower","party_block"},
        {"Freedom: native snare probe","snare"}, {"Vindication / Vengeance swings","attack"}, {"Lay on Hands","lay_hands"},
        {"Divine Intervention (kills you)","intervention"}, {"Divine Favor","divine_favor"},
        {"Blessing of Might","might"}, {"Greater Blessing of Might","greater_might"},
        {"Greater Blessing: party","greater_might"},
        {"Blessing of Salvation","salvation"}, {"Greater Blessing of Salvation","greater_salvation"},
        {"Native spell hit on you","spell_hit"}, {"Follower weapon hit","follower_hit"}, {"Follower spell hit","follower_spell"},
        {"Racial: Perception (Human)","perception"}, {"Racial: Stoneform (Dwarf)","stoneform"},
        {"Racial: Find Treasure (Dwarf)","find_treasure"}, {"Bleeding probe (Stoneform)","bleed"}}) {
        INFO(label); REQUIRE(label.size() <= 32);
        run(("__WBTestResult({ok=true}) routeCalls=table.getn(calls) WBTestPanelSearch:SetText('"+label+"')").c_str());
        for (int i=0;i<3;++i) textures.sync(ui.frame(.016f));
        run(("assert(table.getn(calls)==routeCalls and WBTestPanelRow1:GetText()=='"+label+"') "
             "WBTestPanelRow1:Click() assert(table.getn(calls)==routeCalls+1) "
             "assert(calls[routeCalls+1][2]=='paladin' and calls[routeCalls+1][3]=='"+action+"')").c_str());
    }
    // Native party snapshots must preserve service-owned WoW entries and announce them on the party token.
    run(R"(
      __WBParty({{guid=777,name='Blessed follower',health=100,healthMax=100,auras={}}})
      __WBAuraAdd(777,10293,false,0) __WBAuraAdd(777,20914,false,300)
      __WBParty({{guid=777,name='Blessed follower',health=100,healthMax=100,auras={}}})
      local seen={} for _,a in ipairs(__WB.units[777].buffs) do seen[a.spell]=true end
      assert(seen[10293] and seen[20914] and UnitBuff('party1',1))
      __WBAuraRemove(777,10293)
      seen={} for _,a in ipairs(__WB.units[777].buffs) do seen[a.spell]=true end
      assert(not seen[10293] and seen[20914])
    )");
    CHECK(ui.engine().lastError().empty());
}

// The explicit Skyrim fixture uses these same entries/displays and the Human variant on every Skyrim race.
TEST_CASE("NPC helm fixtures load both Human variants and preserve the DBC hair policy", "[client][dbc][npc-helms]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW client absent");
    wowcore::Mpq mpq(data);
    const auto displays = wowcore::Dbc::load(mpq, "DBFilesClient\\ItemDisplayInfo.dbc");
    const auto visibility = wowcore::Dbc::load(mpq, "DBFilesClient\\HelmetGeosetVisData.dbc");
    REQUIRE(displays); REQUIRE(visibility); REQUIRE(visibility->cols() == 6);
    struct Helm { uint32_t entry, display; const char* model; bool hair, beard; };
    for (const auto& helm : { Helm{7922,16093,"Helm_Plate_B_01Stormwind",true,true},
            Helm{7931,16110,"Helm_Mail_A_02",true,false}, Helm{7048,15283,"Helm_Cloth_A_01",true,false},
            Helm{5624,15905,"Helm_Circlet_C_01",false,false}, Helm{9394,21301,"Helm_Plate_A_02IronforgeGuard",true,false} }) {
        for (uint32_t sex = 0; sex < 2; ++sex) {
            INFO("entry=" << helm.entry << " sex=" << sex);
            const auto vis = wowcore::helmetVisibility(*displays, *visibility, helm.display, 1, sex);
            REQUIRE(vis); CHECK(vis->hair == helm.hair); CHECK(vis->facialHair == helm.beard);
            wowcore::Appearance who; who.race = 1; who.sex = sex;
            const auto item = wowcore::loadItemPreview(mpq, helm.entry, {{helm.entry,{helm.display,1}}}, who);
            INFO(item.detail);
            REQUIRE(item.status == wowcore::ItemPreviewStatus::Ready); REQUIRE(item.parts.size() == 1);
            const auto& part = item.parts[0];
            CHECK(part.model.name == std::string("Item\\ObjectComponents\\Head\\") + helm.model + (sex ? "_HuF.m2" : "_HuM.m2"));
            CHECK_FALSE(part.model.vertices.empty()); CHECK_FALSE(part.model.indices.empty());
            CHECK(part.batchTextures.size() == part.model.batches.size()); CHECK_FALSE(part.images.empty());
        }
    }
    CHECK_FALSE(wowcore::helmetVisibility(*displays, *visibility, 0xFFFFFFFF, 1, 0));
    CHECK_FALSE(wowcore::helmetVisibility(*displays, *visibility, 16093, 0, 0));
    CHECK_FALSE(wowcore::helmetVisibility(*displays, *visibility, 16093, 32, 0));
    CHECK_FALSE(wowcore::helmetVisibility(*displays, *visibility, 16093, 1, 2));
    // Visibility ID 0 is a valid open look, not a missing-row failure.
    const auto open = wowcore::helmetVisibility(*displays, *visibility, 28852, 1, 1);
    REQUIRE(open); CHECK(open->id == 0); CHECK_FALSE(open->hair); CHECK_FALSE(open->facialHair); CHECK_FALSE(open->ears);
}
