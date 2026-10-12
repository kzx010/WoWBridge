#include <catch_amalgamated.hpp>
#include "wowcore/merchant.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/ui_session.hpp"
#include "wowcore/soft_raster.hpp"
#include "wowcore/wowapi.hpp"
extern "C" {
#include <lauxlib.h>
}
#include <cstdlib>
#include <filesystem>
#include <limits>

TEST_CASE("Skyrim barter prices round like its movie and vendor payment cannot exceed gold", "[merchant]") {
    CHECK(wowcore::barterPrice(17,2.5,true)==43);
    CHECK(wowcore::barterPrice(17,0.25,false)==4);
    CHECK(wowcore::barterPrice(0,0,true)==1);
    CHECK(wowcore::barterPrice(0,1,false)==0);
    CHECK_FALSE(wowcore::barterPrice(-1,1,true));
    CHECK_FALSE(wowcore::barterPrice(1,std::numeric_limits<double>::infinity(),true));
    CHECK_FALSE(wowcore::barterPrice(INT32_MAX,2,true));
    const auto capped=wowcore::barterQuote(40,3,8,75,false);
    REQUIRE(capped); CHECK(capped->total==120); CHECK(capped->paid==75);
    const auto empty=wowcore::barterQuote(40,1,1,0,false);
    REQUIRE(empty); CHECK(empty->paid==0);
    CHECK_FALSE(wowcore::barterQuote(40,3,8,75,true));
    CHECK_FALSE(wowcore::barterQuote(40,9,8,1000,true));
    CHECK_FALSE(wowcore::barterQuote(40,-1,8,1000,true));
    CHECK_FALSE(wowcore::barterQuote(INT32_MAX,2,2,INT32_MAX,false));
}

TEST_CASE("Skyrim MerchantFrame events, pagination, buying, bag sales and buyback use live snapshots", "[client][api][merchant]") {
    const char* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq=std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq,db,textures,2560,1440);
    std::string err;
    REQUIRE(ui.start(&err));
    auto* L=ui.lua();
    auto run=[&](const char* script) {
        const int result=luaL_dostring(L,script);
        INFO((result ? lua_tostring(L,-1) : "")); REQUIRE(result==0);
    };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/hud_states.lua").c_str())==0);
    run("__WBFixture('idle')"); REQUIRE(ui.loadInterface(&err));
    run(R"(
      __WBLogin() __WBSkyrimMenus()
      assert(GetMerchantNumItems()==0)
      local ok,msg=pcall(GetMerchantItemInfo) assert(not ok and string.find(msg,'Usage: GetMerchantItemInfo(index)',1,true))
      __WBBagsFixture()
      events={}
      local f=CreateFrame('Frame')
      for _,e in ipairs({'MERCHANT_SHOW','MERCHANT_UPDATE','MERCHANT_CLOSED'}) do f:RegisterEvent(e) end
      f:SetScript('OnEvent',function() tinsert(events,event) end)
      __WBMerchantFixture()
      assert(events[1]=='MERCHANT_SHOW' and MerchantFrame:IsVisible() and IsBagOpen(0))
      assert(UnitName('NPC')=='Lucan Valerius')
      assert(GetMoney()==1854*10000)
      assert(GetMerchantNumItems()==13)
      local name,icon,price,quantity,stock,usable=GetMerchantItemInfo(1)
      assert(name=='Iron Sword' and price==75*10000 and quantity==1 and stock==4 and usable==1)
      assert(icon=='Interface\\Icons\\INV_Sword_04')
      assert(MerchantItem1Name:GetText()==name and MerchantFrame.wbVendorGold:GetText()=='Merchant gold: 750')
      assert(MerchantNextPageButton:IsShown()) MerchantNextPageButton:Click('LeftButton')
      assert(MerchantFrame.page==2 and MerchantItem1ItemButton:GetID()==11)
      MerchantPrevPageButton:Click('LeftButton')
      GameTooltip:SetOwner(UIParent,'ANCHOR_NONE') GameTooltip:SetMerchantItem(1)
      assert(GameTooltipTextLeft1:GetText()=='Iron Sword' and GameTooltip:NumLines()>3)
      assert(not CanMerchantRepair())
      -- Exercise the untouched classic FrameXML hover handler, including the
      -- native comparison method; direct SetMerchantItem alone missed this.
      local previous=this this=MerchantItem1ItemButton
      MerchantItem1ItemButton:GetScript('OnEnter')()
      this=previous
      assert(GameTooltip:IsVisible() and MerchantFrame.itemHover==1)
      ShoppingTooltip1:SetText('stale comparison') ShoppingTooltip1:Show()
      assert(not ShoppingTooltip1:SetMerchantCompareItem(999,1))
      assert(not ShoppingTooltip1:IsVisible())
    )");
    std::vector<std::string> commands;
    wowcore::setHostHandler(L,[&](lua_State* s,const std::string& name) {
        std::string line=name;
        for (int n=2;n<=lua_gettop(s);++n) { line+='|'; if (lua_tostring(s,n)) line+=lua_tostring(s,n); }
        if (name=="merchant" || name=="bagUse") commands.push_back(line); return 0;
    });
    run("MerchantItem1ItemButton:Click('RightButton')");
    CHECK(commands.back()=="merchant|buy|vendorSword|1|91|1");
    run("MerchantItem1ItemButton:Click('LeftButton') assert(CursorHasItem() and __WB.merchantCursor) PickupContainerItem(0,16) assert(not CursorHasItem())");
    CHECK(commands.back()=="merchant|buy|vendorSword|1|91|1");
    run("MerchantItem1ItemButton:Click('LeftButton') ClearCursor() assert(not CursorHasItem())");
    run("MerchantItem1ItemButton:Click('LeftButton') assert(__WBBagWorldClick(0)==1 and not CursorHasItem())");
    run("UseContainerItem(0,2)");
    CHECK(commands.back()=="merchant|sell|sword|1|91|1");
    run("PickupContainerItem(0,2) PickupMerchantItem(0) assert(__WB.bagHeld.key=='sword')");
    CHECK(commands.back()=="merchant|sell|sword|1|91|1");
    run("ClearCursor()"); // the authoritative sale acknowledgement removes the carried pile in game
    // The live probe must accept a partial payment even when base-container
    // gold remains: four swords at eight septims exceed a 25-septim balance.
    const auto beforeCap=commands.size();
    run(R"(
      __WB.bagItems[2].count=5 __WB.bagItems[2].bagCount=5 __WB.merchant.gold=25
      SplitContainerItem(0,2,4) PickupMerchantItem(0)
      local dialog=StaticPopup_FindVisible('WB_MERCHANT_GOLD_LIMIT')
      assert(dialog and dialog.data.count==4 and dialog.data.key==__WB.bagHeld.key)
      cappedPileKey=dialog.data.key
      assert(__WB.bagItems[2].count==1 and __WB.bagHeld.count==4)
    )");
    CHECK(commands.size()==beforeCap);
    lua_getglobal(L,"cappedPileKey");
    const std::string cappedPileKey=lua_tostring(L,-1);
    lua_pop(L,1);
    run(R"(
      local dialog=StaticPopup_FindVisible('WB_MERCHANT_GOLD_LIMIT')
      getglobal(dialog:GetName()..'Button1'):Click('LeftButton')
      ClearCursor() __WB.bagItems[2].count=1 __WB.bagItems[2].bagCount=1 __WB.merchant.gold=750
    )");
    REQUIRE(commands.size()==beforeCap+1);
    CHECK(commands.back()=="merchant|sell|"+cappedPileKey+"|4|91|1");
    const auto before=commands.size();
    run("__WB.bagItems[3].stolen=true UseContainerItem(0,3)"); // native sales list excludes stolen/non-vendor goods
    CHECK(commands.size()==before);
    run(R"(
      __WB.merchant.gold=0 UseContainerItem(0,2)
      local dialog=StaticPopup_FindVisible('WB_MERCHANT_GOLD_LIMIT') assert(dialog and dialog.data.count==1)
      getglobal(dialog:GetName()..'Button2'):Click('LeftButton')
    )");
    CHECK(commands.size()==before);
    run(R"(
      UseContainerItem(0,2)
      local dialog=StaticPopup_FindVisible('WB_MERCHANT_GOLD_LIMIT') assert(dialog)
      getglobal(dialog:GetName()..'Button1'):Click('LeftButton')
      __WB.merchant.gold=750
    )");
    CHECK(commands.back()=="merchant|sell|sword|1|91|1");
    run(R"(
      __WBMerchantFixture(2)
      assert(events[2]=='MERCHANT_UPDATE' and MerchantFrame:IsVisible())
      assert(GetNumBuybackItems()==1)
      local name,texture,price,quantity=GetBuybackItemInfo(1)
      assert(name=='Iron Sword' and price==150*10000 and quantity==2)
      MerchantFrameTab2:Click('LeftButton')
      assert(MerchantFrame.selectedTab==2 and MerchantItem1Name:GetText()=='Iron Sword','buyback tab: '..tostring(MerchantFrame.selectedTab)..' '..tostring(MerchantItem1Name:GetText()))
      GameTooltip:SetBuybackItem(1) assert(GameTooltipTextLeft1:GetText()=='Iron Sword')
      MerchantItem1ItemButton:Click('RightButton')
    )");
    CHECK(commands.back()=="merchant|buyback|vendorSword|2|91|2");
    run(R"(
      __WBMerchantFixture(1) assert(__WB.merchantRevision==2 and GetNumBuybackItems()==1)
      local text=__WBReloadStateText() assert(loadstring(text))
      assert(loadstring(text))() __WB.merchantInstalled=nil __WBInstallMerchant()
      assert(MerchantFrame:IsVisible() and GetNumBuybackItems()==1)
    )");
    ui.frame(0.1f);
    run(R"(
      CloseMerchant()
      assert(not MerchantFrame:IsVisible() and IsBagOpen(0) and GetMerchantNumItems()==0) -- client's OpenBackpack preserves a backpack already open at rehydration
      assert(events[table.getn(events)]=='MERCHANT_CLOSED')
    )");
    ui.frame(0.1f);
    CHECK(commands.back()=="merchant|close||0|91|2");
    run("__WBMerchantFixture(3) assert(MerchantFrame:IsVisible(), 'reopened visibility')");
    ui.frame(0.1f);
    run("HideUIPanel(MerchantFrame)");
    ui.frame(0.1f); // the renderer dispatches OnHide, including the client's CloseMerchant
    run("assert(not __WB.merchant)");
    CHECK(commands.back()=="merchant|close||0|91|3");
}
