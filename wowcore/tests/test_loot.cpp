#include <catch_amalgamated.hpp>
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/ui_session.hpp"
#include "wowcore/wowapi.hpp"
#include "wowcore/soft_raster.hpp"
extern "C" {
#include <lauxlib.h>
}
#include <cstdlib>
#include <filesystem>

TEST_CASE("Skyrim loot uses the client frame, stable slots, shared item art and acknowledged host transfers", "[client][api][loot]") {
    const char* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq=std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    auto ui=std::make_unique<wowcore::UiSession>(mpq,db,textures,2560,1440);
    std::string err;
    REQUIRE(ui->start(&err));
    auto* L=ui->lua();
    auto run=[&](const char* script) {
        const int result=luaL_dostring(L,script);
        INFO((result ? lua_tostring(L,-1) : ""));
        REQUIRE(result==0);
    };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/hud_states.lua").c_str())==0);
    run("__WBFixture('idle')");
    REQUIRE(ui->loadInterface(&err));
    run(R"(
      assert(GetNumLootItems()==0)
      local ok,msg=pcall(GetLootSlotInfo)
      assert(not ok and string.find(msg,'Usage: GetLootSlotInfo(slot)',1,true))
      __WBLogin() __WBSkyrimMenus()
      calls={} __WB.host=function(...)
        if arg[1]=='lootTake' or arg[1]=='lootClose' or arg[1]=='lootDeposit' then tinsert(calls,arg) end
      end
      __WBLootFixture(false)
    )");
    ui->frame(0.1f);
    run(R"(
      assert(LootFrame:IsShown() and GetNumLootItems()==6)
      assert(GetLootSlotLink(1)==nil and LootSlotIsCoin(1)==1 and not LootSlotIsItem(1))
      local texture,label,count,quality=GetLootSlotInfo(1)
      assert(label=='42 Gold' and count==1 and quality==1)
      assert(texture==__WBBagIcon({form=15}))
      texture,label,count,quality=GetLootSlotInfo(2)
      assert(count==3 and quality==2 and string.find(label,'|cffff2020Steal|r',1,true))
      assert(texture==__WBBagIcon({weapon=1}))
      assert(__WB.lootItems[3].texture==__WB.wowBagItems[873].texture)
      assert(__WB.lootItems[3].quality==4)
      this=LootButton2 LootButton2:GetScript('OnEnter')() this=nil
      assert(GameTooltip:IsShown())
      assert(string.find(GameTooltipTextLeft1:GetText(),'Embers',1,true))
      local warning=false
      for i=1,GameTooltip:NumLines() do
        local line=getglobal('GameTooltipTextLeft'..i)
        if line and string.find(line:GetText() or '', 'theft',1,true) then warning=true end
      end
      assert(warning)
      calls={} LootButton2:Click('LeftButton') LootButton2:Click('RightButton')
      assert(table.getn(calls)==1 and calls[1][1]=='lootTake' and calls[1][2]==101 and calls[1][3]=='embers' and calls[1][4]==3)
      assert(LootSlotIsItem(2)==1) -- no optimistic removal before Skyrim confirms
      local list={} for _,i in pairs(__WB.lootItems) do tinsert(list,i) end
      __WBLoot(101,list,false) -- a refused transfer republishes the unchanged stack
      LootButton2:Click('LeftButton')
      assert(table.getn(calls)==2 and calls[2][4]==3)
      __WB.lootItems[2].count=2 __WBLoot(101,list,false)
      local _,_,updatedCount=GetLootSlotInfo(2)
      assert(updatedCount==2 and LootButton2Count:GetText()=='2')
      __WBLoot(101,{
        {slot=1,key='coin',form=15,name='Gold',count=42},
        {slot=3,key='staff',form=0xfe800,entry=873,name='Staff of Jordan',count=1},
        {slot=6,key='apple',form=0x64b2e,name='Red Apple',count=2,kind='Food',keywords={'VendorItemFood'}}
      },false)
      assert(GetNumLootItems()==6 and not LootSlotIsItem(2) and LootSlotIsItem(3)==1)
      assert(not LootButton2:IsShown() and LootButton3.slot==3)
      LootFrame_PageDown() assert(LootButton3.slot==6)
      this=LootButton3 LootButton3:GetScript('OnEnter')() this=nil
      assert(GameTooltipTextLeft1:GetText()=='Red Apple')
      calls={} __WB.modifiers={shift=1}
      LootButton3:Click('LeftButton') __WB.modifiers={}
      assert(table.getn(calls)==3)
      for _,c in ipairs(calls) do assert(c[1]=='lootTake' and c[2]==101) end
      calls={} CloseLoot() CloseLoot()
      assert(table.getn(calls)==1 and calls[1][1]=='lootClose' and calls[1][2]==101)
      __WBLootClosed(100) assert(LootFrame:IsShown())
      __WBLootClosed(101) assert(not LootFrame:IsShown() and GetNumLootItems()==0)
      assert(table.getn(calls)==1) -- OnHide cannot recursively close an acknowledged session
    )");
    ui->frame(0.1f);
    run(R"(
      __WBLootFixture(false) calls={}
    )");
    ui->frame(0.1f);
    run(R"(
      LootCloseButton:Click('LeftButton')
    )");
    ui->frame(0.1f); // The portable engine dispatches OnHide at its visibility pass.
    run(R"(
      assert(table.getn(calls)==1 and calls[1][1]=='lootClose',
        'close calls='..table.getn(calls)..' closing='..tostring(__WB.lootClosing)..' shown='..tostring(LootFrame:IsShown() or nil))
      __WBLootClosed(101)
      calls={} SetCVar('autoLootDefault',1) calls={}
      __WBLootFixture(false) assert(table.getn(calls)==6)
      __WBLootClosed(101) calls={}
      __WBLootFixture(true) assert(table.getn(calls)==0) -- Shift overrides auto-loot on activation
      __WBLootClosed(101) SetCVar('autoLootDefault',0) calls={}
      __WBLootFixture(true) assert(table.getn(calls)==6)
      __WBLootClosed(101) calls={}
      __WBLoot(102,{},true,false) assert(GetNumLootItems()==0 and LootFrame:IsShown())
      __WBLootClosed(102)
      __WBInventory({{key='potions',identity='potions',form=0x3EADD,name='Potion',kind='Potion',pos=1,count=5}},1)
      __WBLoot(103,{},true,false,true) calls={}
      assert(__WB.lootCanDeposit and LootFrame:IsShown())
      UseContainerItem(0,1) UseContainerItem(0,1)
      assert(table.getn(calls)==1 and calls[1][1]=='lootDeposit' and calls[1][2]==103 and calls[1][3]=='potions' and calls[1][4]==5)
      assert(__WB.bagItems[1].count==5) -- native inventory snapshot owns debit
      __WBLoot(103,{},false,false,true) calls={}
      SplitContainerItem(0,1,2)
      assert(__WB.bagHeld.count==2)
      LootFrame:GetScript('OnReceiveDrag')() LootFrame:GetScript('OnReceiveDrag')()
      local deposits=0
      for _,c in ipairs(calls) do if c[1]=='lootDeposit' then deposits=deposits+1 assert(c[4]==2) end end
      assert(deposits==1 and __WB.bagHeld.count==2) -- no premature cursor cancellation
      __WBLoot(103,{},false,false,true) calls={}
      LootFrame:GetScript('OnReceiveDrag')()
      assert(table.getn(calls)==1 and calls[1][4]==2) -- refusal permits retry
      ClearCursor() __WBLootClosed(103)
      __WBLoot(104,{},true,false,false) calls={}
      SplitContainerItem(0,1,1) calls={}
      LootFrame:GetScript('OnReceiveDrag')()
      assert(table.getn(calls)==0) -- corpses are take-only
      ClearCursor() __WBLootClosed(104)
      calls={} __WBLootDeposit({key='stale',count=1}) assert(table.getn(calls)==0)
      -- Exercise the real options load/save for this addition. The portable API has no
      -- ShowingCloak/ShowingHelm implementation for the unrelated appearance options.
      local buttons,sliders=UIOptionsFrameCheckButtons,UIOptionsFrameSliders
      UIOptionsFrameCheckButtons={WB_AUTO_LOOT_TEXT=buttons.WB_AUTO_LOOT_TEXT}
      UIOptionsFrameSliders={}
      this=UIOptionsFrame UIOptionsFrame_Load()
      assert(UIOptionsFrameCheckButton70:GetChecked()==nil)
      UIOptionsFrameCheckButton70:SetChecked(1) UIOptionsFrame_Save()
      this=nil assert(GetCVar('autoLootDefault')=='1')
      UIOptionsFrameCheckButtons,UIOptionsFrameSliders=buttons,sliders
      SetCVar('autoLootDefault',0)
      __WBLootFixture(false) calls={}
      reload=__WBReloadStateText()
      assert(not string.find(reload,'lootInstalled',1,true))
    )");
    lua_getglobal(L,"reload");
    const std::string retained=lua_tostring(L,-1);
    lua_pop(L,1);
    // Rebuild the real interface with the actual retained state, as the plugin's ReloadUI does.
    ui.reset();
    ui=std::make_unique<wowcore::UiSession>(mpq,db,textures,2560,1440);
    REQUIRE(ui->start(&err));
    L=ui->lua();
    REQUIRE(ui->loadInterface(&err));
    run(retained.c_str());
    run(R"(
      calls={} __WB.host=function(...)
        if arg[1]=='lootTake' or arg[1]=='lootClose' or arg[1]=='lootDeposit' then tinsert(calls,arg) end
      end
      __WBInstallLoot()
      assert(LootFrame:IsShown() and GetNumLootItems()==6 and table.getn(calls)==0)
      assert(GetLootSlotLink(3) and GetLootSlotInfo(2))
      __WBLootClosed(101) assert(not LootFrame:IsShown())
    )");
}
