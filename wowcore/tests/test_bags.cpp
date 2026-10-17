#include <catch_amalgamated.hpp>
#include "wowcore/bags.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/ui_session.hpp"
#include "wowcore/wowapi.hpp"
#include "wowcore/soft_raster.hpp"
#include "addons/lua_engine.hpp"
extern "C" {
#include <lauxlib.h>
}
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include "pipeline/blp_loader.hpp"

TEST_CASE("bag places preserve gaps, swap across pages and round-trip sparse saves", "[bags]") {
    wowcore::BagPlaces p;
    std::vector<std::string> items;
    for (int n=1;n<=179;++n) items.push_back("stack"+std::to_string(n));
    p.reconcile(items);
    CHECK(p.pages()==3);
    CHECK(p.at(80)=="stack80");
    CHECK(p.at(81)=="stack81");
    items.erase(items.begin()+1);
    p.reconcile(items);
    CHECK(p.at(2).empty());
    CHECK(p.at(3)=="stack3");
    items.push_back("new");
    p.reconcile(items);
    CHECK(p.at(2)=="new");
    REQUIRE(p.move("stack1",165,"stack165"));
    CHECK(p.at(1)=="stack165");
    CHECK(p.at(165)=="stack1");
    CHECK_FALSE(p.move("stack1",2,"stale"));
    REQUIRE(p.move("stack1",240,"")); // spare page
    CHECK(p.at(165).empty());
    wowcore::BagPlaces loaded;
    REQUIRE(loaded.restore(p.places));
    loaded.reconcile(items);
    CHECK(loaded.places==p.places);
    CHECK(loaded.at(240)=="stack1");
    CHECK_FALSE(loaded.restore({{"a",0}}));
    CHECK_FALSE(loaded.restore({{"a",1},{"b",1}}));
    CHECK_FALSE(loaded.restore({{"a",1000001}}));
    CHECK(loaded.places==p.places);
}

TEST_CASE("bag piles split, merge, swap, cancel and reconcile Skyrim totals", "[bags]") {
    wowcore::BagPiles p;
    p.reconcile({{"arrows",20},{"sword",1}});
    REQUIRE(p.grab("arrows",6,"six"));
    CHECK(p.at(1)=="arrows");
    CHECK(p.piles.at("arrows").count==14);
    CHECK(p.piles.at("six").pos==0);
    CHECK(p.total("arrows")==20);
    REQUIRE(p.place("six",3,""));
    CHECK(p.piles.at("six").count==6);
    REQUIRE(p.grab("six",2,"two"));
    REQUIRE(p.place("two",2,"sword")); // partial swap parks the displaced pile and releases the cursor
    CHECK(p.held.empty());
    CHECK(p.piles.at("sword").pos==4); // original place still contains the remainder
    CHECK(p.total("arrows")==20);
    REQUIRE(p.grab("two",2,"unused"));
    REQUIRE(p.place("two",1,"arrows"));
    CHECK(p.piles.at("arrows").count==16);
    CHECK_FALSE(p.piles.contains("two"));
    REQUIRE(p.grab("six",2,"back"));
    p.cancel();
    CHECK(p.piles.at("six").count==4);
    CHECK_FALSE(p.piles.contains("back"));
    p.reconcile({{"arrows",25},{"sword",1}}); // acquisition joins earliest pile
    CHECK(p.piles.at("arrows").count==21);
    CHECK(p.piles.at("six").count==4);
    p.reconcile({{"arrows",22},{"sword",1}}); // external consumption takes latest pile first
    CHECK(p.piles.at("six").count==1);
    p.debit("arrows",2); // selected-pile use/drop
    p.reconcile({{"arrows",20},{"sword",1}});
    CHECK(p.piles.at("arrows").count==19);
    CHECK(p.piles.at("six").count==1);
    REQUIRE(p.grab("arrows",19,"unused"));
    CHECK_FALSE(p.place("arrows",4,"stale"));
    CHECK_FALSE(p.grab("six",1,"unused"));
    CHECK_FALSE(p.place("arrows",81,""));
    p.cancel();
    wowcore::BagPiles loaded;
    REQUIRE(loaded.restore(p.piles));
    loaded.reconcile({{"arrows",20},{"sword",1}});
    CHECK(loaded.piles==p.piles);
    CHECK_FALSE(loaded.restore({{"bad",{"arrows",0,1}}}));
    CHECK_FALSE(loaded.restore({{"bad",{"arrows",1,0}}}));
    CHECK_FALSE(loaded.restore({{"a",{"arrows",1,1}},{"b",{"sword",1,1}}}));
    REQUIRE(loaded.restore({{"old",{"arrows",37,0}}},true)); // v1 migration
    loaded.reconcile({{"arrows",20}});
    CHECK(loaded.piles.at("old").count==20);
    CHECK(loaded.piles.at("old").pos==37);
    loaded.rename("arrows","tempered");
    loaded.reconcile({{"tempered",20}});
    CHECK(loaded.piles.at("old").identity=="tempered");
    REQUIRE(loaded.grab("old",20,"unused"));
    loaded.reconcile({{"tempered",22}}); // acquiring while holding does not grow the held quantity
    CHECK(loaded.piles.at("old").count==20);
    CHECK(loaded.total("tempered")==22);
    loaded.reconcile({});
    CHECK(loaded.held.empty());
    CHECK(loaded.piles.empty());
}

TEST_CASE("confirmed equipment replacement returns the old identity to the source place", "[bags][equipment]") {
    wowcore::BagPiles piles;
    std::vector<std::pair<std::string,int32_t>> live;
    for (int n=1;n<=80;++n) live.emplace_back("item"+std::to_string(n),1);
    piles.reconcile(live); // full first page, including the hidden equipped reservation
    REQUIRE(piles.returnEquipment(80,"item80","item1"));
    CHECK(piles.at(80)=="item1");
    CHECK(piles.at(1)=="item80");
    CHECK(piles.pages()==1);
    CHECK(piles.total("item1")==1);
    CHECK(piles.total("item80")==1);
    CHECK_FALSE(piles.returnEquipment(80,"item80","item2")); // another item now occupies the source
    CHECK_FALSE(piles.returnEquipment(80,"item1","missing"));
    CHECK_FALSE(piles.returnEquipment(80,"item1","item1"));
    piles.rename("item80","tempered-instance");
    REQUIRE(piles.returnEquipment(80,"item1","tempered-instance"));
    CHECK(piles.at(80)=="item80"); // stable pile key; changed native identity retained
    CHECK(piles.piles.at("item80").identity=="tempered-instance");
    REQUIRE(piles.grab("item1",1,"unused"));
    CHECK_FALSE(piles.returnEquipment(80,"tempered-instance","item1")); // cannot move another held pile
    CHECK(piles.held=="item1");
}

TEST_CASE("character pickup reserves a copy until a validated drop or cancellation", "[bags][character]") {
    wowcore::BagPiles piles;
    piles.reconcile({{"dual", 3}, {"other", 1}});
    REQUIRE(piles.grabEquipped("dual", 17, "left"));
    CHECK(piles.equippedSlot == 17);
    CHECK(piles.total("dual") == 3);
    CHECK_FALSE(piles.place("left", 2, "stale"));
    CHECK(piles.equippedSlot == 17);
    CHECK_FALSE(piles.grabEquipped("other", 16, "other-copy"));
    CHECK(piles.equippedSlot == 17);
    piles.cancel();
    CHECK_FALSE(piles.equippedSlot);
    CHECK(piles.piles.at("dual").count == 3);
    REQUIRE(piles.grabEquipped("dual", 16, "right"));
    REQUIRE(piles.place("right", 2, "other"));
    CHECK_FALSE(piles.equippedSlot);
    CHECK(piles.held.empty());
    CHECK(piles.piles.at("other").pos == 3); // remainder still occupies the character copy's origin
    REQUIRE(piles.grabEquipped("right", 16, "unused"));
    piles.debit("right", 1);
    CHECK_FALSE(piles.equippedSlot);
    REQUIRE(piles.grabEquipped("dual", 0, "ammo")); // slot zero is still a deferred equipment drag
    CHECK(piles.equippedSlot == 0);
    piles.reconcile({});
    CHECK_FALSE(piles.equippedSlot);
}

TEST_CASE("a split opens an extra page only when every existing place is occupied", "[bags]") {
    wowcore::BagPiles p;
    std::vector<std::pair<std::string,int32_t>> live;
    for (int n=1;n<=80;++n) live.emplace_back("item"+std::to_string(n),10);
    p.reconcile(live);
    CHECK(p.pages()==1);
    REQUIRE(p.grab("item1",3,"partial"));
    CHECK(p.pages()==2);
    p.cancel();
    CHECK(p.pages()==1);
    p.debit("item2",10);
    REQUIRE(p.grab("item1",3,"partial"));
    CHECK(p.pages()==1);
    REQUIRE(p.place("partial",2,""));
    CHECK(p.pages()==1);
    CHECK(p.piles.at("partial").count==3);
    CHECK(p.total("item1")==10);
}

TEST_CASE("Skyrim bags use client frames, APIs, controls, tooltips and host commands", "[client][api][bags]") {
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
        INFO((result ? lua_tostring(L,-1) : ""));
        REQUIRE(result==0);
    };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/hud_states.lua").c_str())==0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run(R"(
      __WBLogin()
      assert(GetBindingAction('B')=='TOGGLEBACKPACK')
      __WBSkyrimMenus()
      assert(GetBindingAction('B')=='OPENALLBAGS' and GetBindingAction('SHIFT-B')=='')
      LoadBindings(0)
      assert(GetBindingAction('B')=='OPENALLBAGS' and GetBindingAction('SHIFT-B')=='')
      assert(GetContainerNumSlots(1)==0)
      assert(select('#',GetContainerItemInfo(1,1))==0)
      local ok,msg=pcall(GetBagName) assert(not ok and string.find(msg,'Usage: GetBagName(index)',1,true))
      __WBBagsFixture()
      for bag=0,4 do assert(GetContainerNumSlots(bag)==16) end
      RunBinding(GetBindingAction('B'),'down')
      for bag=0,4 do assert(IsBagOpen(bag)) end
      RunBinding(GetBindingAction('B'),'down')
      for bag=0,4 do assert(not IsBagOpen(bag)) end
      CharacterBag0Slot:Click('LeftButton') assert(IsBagOpen(1))
      OpenBackpack()
      MainMenuBarBackpackButton:Click('LeftButton') assert(not IsBagOpen(0))
      OpenAllBags(1)
      StackSplitFrame:Hide() CoinPickupFrame:Hide()
      ContainerFrame1MoneyFrameGoldButton:Click('LeftButton')
      assert(not StackSplitFrame:IsShown() and not CoinPickupFrame:IsShown())
      assert(not ContainerFrame1MoneyFrameGoldButton:IsMouseEnabled())
      for bag=0,4 do assert(IsBagOpen(bag)) end
      assert(ContainerFrame1:GetID()==0 and ContainerFrame1.wbPages:IsShown() and ContainerFrame1.wbPages.label:GetText()=='1 / 3')
      for n=2,5 do local f=getglobal('ContainerFrame'..n) assert(not (f.wbPages and f.wbPages:IsShown())) end
      assert(GetInventoryItemTexture('player',20)=='Interface\\Icons\\INV_Misc_Bag_08')
      local texture,count,locked,quality=GetContainerItemInfo(0,1)
      assert(count==1 and quality==4 and string.find(texture,'INV_Staff',1,true))
      GameTooltip:SetOwner(UIParent,'ANCHOR_NONE')
      local cooldown,repair=GameTooltip:SetBagItem(0,1)
      assert(cooldown==nil and repair==0)
      assert(GameTooltipTextLeft1:GetText()=='Staff of Jordan')
      assert(GameTooltip:NumLines()>5)
      GameTooltip:SetBagItem(0,2)
      assert(GameTooltipTextLeft1:GetText()=='Iron Sword')
      assert(GameTooltipTextLeft3:GetText()=='7 Damage')
      assert(GameTooltipTextLeft4:GetText()=='Weight: 9')
      assert(GameTooltipTextLeft5:GetText()=='Value: 25')
      local r,g,b=GameTooltipTextLeft1:GetTextColor() assert(r==1 and g==1 and b==1)
      GameTooltip:SetBagItem(0,3)
      r,g,b=GameTooltipTextLeft1:GetTextColor() assert(g==1 and r<0.2 and b==0)
      local effect, hint
      for n=2,GameTooltip:NumLines() do
        local line=getglobal('GameTooltipTextLeft'..n) local text=line:GetText() r,g,b=line:GetTextColor()
        if r==0 and g==1 and b==0 and string.sub(text,1,1)~='<' then effect=true end
        if text=='<Right Click to Equip>' or text=='<Right Click to Unequip>' then hint=true end
      end
      assert(effect and hint)   -- its enchantment in green, then what the player can do with it
      assert(getglobal('GameTooltipTextLeft'..GameTooltip:NumLines()):GetText()=='<Tab, then Items: Sell>')
      local link=GetContainerItemLink(0,3) assert(string.find(link,'|cff1eff00|Hskyrim:',1,true)==1)
      assert(GameTooltip:SetHyperlink(link)==1)
      assert(select('#',GetContainerItemInfo(3,16))==0) -- deliberate hole
      assert(select('#',GetContainerItemLink(3,16))==0)
      local start,duration,enabled=GetContainerItemCooldown(3,16) assert(start==0 and duration==0 and enabled==0)
      UseContainerItem(0,1) assert(string.find(__WB.log[table.getn(__WB.log)],'bagUse staff',1,true))
      PickupContainerItem(0,2) assert(CursorHasItem() and not CursorHasSpell())
      assert(__WB.cursorTexture()==GetContainerItemInfo(0,2))
      local _,_,locked=GetContainerItemInfo(0,2) assert(locked==1)
      -- Flip with the actual control while holding a stack, then swap into another bag.
      ContainerFrame1.wbPages.next:Click('LeftButton') assert(__WB.bagPage==2)
      assert(ContainerFrame1.wbPages.label:GetText()=='2 / 3')
      local displaced=__WB.bagItems[97].key
      PickupContainerItem(1,1)
      assert(__WB.bagItems[97].key=='sword' and __WB.bagItems[2].key==displaced and not CursorHasItem())
      assert(not __WB.bagCursor and not __WB.cursorTexture())
      __WBBagPage(1) ClearCursor()
      assert(not CursorHasItem() and not __WB.bagCursor)
      __WBBagPage(4) assert(__WB.bagPage==3)   -- no page past their last
      __WBBagPage(1) PickupContainerItem(0,1) PickupContainerItem(3,16)   -- into the hole
      assert(__WB.bagItems[64].key=='staff' and __WB.bagPages==3)
      assert(select('#',GetContainerItemInfo(0,1))==0)
      -- A host removal clears a held cursor, preserves sparse positions and updates all visible buttons.
      PickupContainerItem(3,16)
      __WBInventory({{key='sword',form=0x12eb7,name='Iron Sword',pos=97,count=4,kind='Weapon',weapon=1,damage=7,weight=9,value=25}},2)
      assert(not CursorHasItem() and __WB.bagItems[97].count==4 and not __WB.bagItems[64])
      assert(GetContainerItemInfo(1,1)=='Interface\\Icons\\INV_Sword_04')
      -- New Skyrim acquisition into a gap, zero inventory, and the previous save's page clamped.
      __WBInventory({},99) assert(__WB.bagPage==1 and __WB.bagPages==1)   -- emptied pages go
      assert(not (ContainerFrame1.wbPages and ContainerFrame1.wbPages:IsShown()))
      assert(select('#',GetContainerItemInfo(0,1))==0)
      assert(GetContainerNumSlots(0)==16)
      -- New exports resolve an uncaptured WoW item's own icon/stats and spell descriptions.
      __WBInventory({{key='wow',form=0xfe800,name='Test Staff',pos=1,count=1,entry=123456,display=20298,quality=3,
        wow={class=2,subclass=10,InventoryType=17,Quality=3,dmg_min1=100,dmg_max1=150,delay=3000,
          stat_type1=5,stat_value1=20,RequiredLevel=35,spellid_1=9395,spelltrigger_1=1}}},1)
      assert(GetContainerItemInfo(0,1)==__WBItemIcon(20298))
      GameTooltip:SetBagItem(0,1) assert(GameTooltipTextLeft2:GetText()=='Two-Hand')
      assert(GameTooltipTextLeft5:GetText()=='+20 Intellect')
      -- Live armour changes the character pane as well as bags, using WoW cloth/stats and set bonuses with Skyrim's
      -- own armour for the copy (its live rating, not WoW's 176).
      local set={id=205,name='Cenarion Raiment',items={{entry=16833,name='Cenarion Vestments'},
        {entry=16834,name='Cenarion Helm'}, {entry=16835,name='Cenarion Leggings'}},bonuses={{spell=23557,threshold=3}}}
      local chest={key='cenarion',form=0xfe810,name='Cenarion Vestments',pos=1,count=1,entry=16833,display=31797,
        quality=4,kind='Armor',armor=20,worn=true,inventorySlots={5},set=set,
        wow={class=4,subclass=2,InventoryType=20,armor=176,stat_type1=5,stat_value1=20,AllowableClass=1024,RequiredLevel=60}}
      __WBInventory({chest,{key='helm',form=0xfe811,name='Cenarion Helm',pos=2,count=1,entry=16834,worn=true,inventorySlots={1}},
        {key='legs',form=0xfe812,name='Cenarion Leggings',pos=3,count=1,entry=16835,worn=true,inventorySlots={7}}},1)
      assert(GetInventoryItemTexture('player',5)==__WBItemIcon(31797))
      assert(GetInventoryItemLink('player',5) and not GetContainerItemLink(0,1))
      GameTooltip:SetInventoryItem('player',5)
      assert(GameTooltipTextLeft1:GetText()=='Cenarion Vestments')
      local armor,classes,bonus
      for n=1,GameTooltip:NumLines() do
        local text=getglobal('GameTooltipTextLeft'..n):GetText()
        if text=='20 Armor' then armor=true end
        assert(text~='176 Armor',text)
        if text=='Classes: Druid' then classes=true end
        if string.find(text,'(3) Set:',1,true)==1 then
          local r,g,b=getglobal('GameTooltipTextLeft'..n):GetTextColor() bonus=(r==0 and g==1 and b==0)
        end
      end
      assert(armor and classes and bonus)
      chest.worn=false chest.inventorySlots={}
      __WBInventory({chest},1)
      assert(GetInventoryItemTexture('player',5)==nil and GetInventoryItemLink('player',5)==nil)
      GameTooltip:SetBagItem(0,1)
      local inactive
      for n=1,GameTooltip:NumLines() do if string.find(getglobal('GameTooltipTextLeft'..n):GetText(),'(3) Set:',1,true)==1 then
        local r,g,b=getglobal('GameTooltipTextLeft'..n):GetTextColor() inactive=(r==0.5 and g==0.5 and b==0.5)
      end end
      assert(inactive)
      -- Thunderfury's chance-on-hit text is WoW data; its damage is Skyrim's 20, spread over WoW's two ranges.
      __WBInventory({{key='thunderfury',form=0xfe813,name='Thunderfury',pos=1,count=1,entry=19019,display=30606,quality=5,
        kind='Weapon',worn=true,inventorySlots={16},damage=20,
        wow={class=2,subclass=7,InventoryType=13,dmg_min1=44,dmg_max1=115,dmg_min2=16,dmg_max2=30,dmg_type2=3,
          delay=1900,spellid_1=21992,spelltrigger_1=2}}},1)
      GameTooltip:SetInventoryItem('player',16)
      local damage,nature,proc
      for n=1,GameTooltip:NumLines() do local text=getglobal('GameTooltipTextLeft'..n):GetText()
        if text=='9 - 22 Damage' then damage=getglobal('GameTooltipTextRight'..n):GetText()=='Speed 1.90' end
        if text=='3 - 6 Nature Damage' then nature=true end
        if string.find(text,'Chance on hit:',1,true)==1 then proc=true end
      end
      assert(damage and nature and proc)
      -- Crafted instances keep client stat text, add Skyrim quality/effects, and
      -- resolve distinct links even when their base WoW entry is identical.
      local crafted={key='crafted',form=0xfe814,entry=16833,instance=42,name='Renamed Vestments',
        pos=1,count=1,kind='Armor',quality=4,enchanted=true,enchantment=0xff000123,
        worn=true,inventorySlots={5},armor=48,temper='Legendary',effects={'Increases your health by 50 points.'},
        wow={class=4,subclass=2,InventoryType=20,armor=176}}
      local plain={key='plain',form=0xfe814,entry=16833,instance=43,name='Cenarion Vestments',
        pos=2,count=1,kind='Armor',quality=4,wow=crafted.wow}
      __WBInventory({crafted,plain},1)
      local function checkCrafted()
        local quality,effect,armor
        for n=1,GameTooltip:NumLines() do
          local f=getglobal('GameTooltipTextLeft'..n) local text=f:GetText()
          if text=='Improved: Legendary' then quality=true end
          if text=='Increases your health by 50 points.' then
            local r,g,b=f:GetTextColor() effect=r==0 and g==1 and b==0
          end
          if text=='48 Armor' then armor=true end
        end
        assert(quality and effect and armor)
      end
      assert(not GetContainerItemLink(0,1))
      GameTooltip:SetInventoryItem('player',5) checkCrafted()
      local craftedLink=GetInventoryItemLink('player',5)
      assert(craftedLink~=GetContainerItemLink(0,2))
      GameTooltip:SetHyperlink(craftedLink) checkCrafted()
      GameTooltip:SetBagItem(0,2)
      for n=1,GameTooltip:NumLines() do
        assert(getglobal('GameTooltipTextLeft'..n):GetText()~='Improved: Legendary')
      end
      -- A save can already have a worn Staff of Jordan before the probe gives
      -- another. Bags expose only the spare, while the pile retains worn=true.
      local staff={key='staff-spare',identity='staff',form=0xfe800,entry=873,name='Staff of Jordan',
        pos=1,count=2,kind='Weapon',weapon=8,worn=true,inventorySlots={16}}
      __WBInventory({staff},1,'',0,__WB.bagRevision)
      local _,n=GetContainerItemInfo(0,1)
      assert(n==1 and staff.count==2 and staff.worn and not staff.bagEquipped)
      GameTooltip:SetBagItem(0,1)
      local equip
      for n=1,GameTooltip:NumLines() do
        equip=equip or getglobal('GameTooltipTextLeft'..n):GetText()=='<Right Click to Equip>'
      end
      assert(equip)
      UseContainerItem(0,1)
      assert(string.find(__WB.log[table.getn(__WB.log)],'bagUse staff-spare',1,true))
      PickupContainerItem(0,1)
      assert(__WB.bagHeld.count==1 and __WB.bagHeld.key~=staff.key)
      assert(GetInventoryItemLink('player',16) and staff.count==1)
      ClearCursor() assert(staff.count==2)
      -- Once Skyrim acknowledges unequipping, a whole-pile bag move keeps the
      -- saved key. Character pickup would select only one copy with a new key.
      staff.worn=false staff.inventorySlots={}
      __WBInventory({staff},1,'',0,__WB.bagRevision)
      assert(not GetInventoryItemLink('player',16))
      PickupContainerItem(0,1) PickupContainerItem(4,16)
      assert(__WB.bagItems[80].key=='staff-spare' and __WB.bagItems[80].count==2 and not CursorHasItem())
      PickupContainerItem(4,16) PickupContainerItem(0,1)
      assert(__WB.bagItems[1].key=='staff-spare' and __WB.bagItems[1].count==2 and not CursorHasItem())
      -- Modifier snapshots drive the character sheet APIs and preserve native resources.
      local health=UnitHealth('player')
      __WBApplyLive({player={armor={48,48,0,0,0},damage={123,145,0,0,0,0,1},
        stats={{100,100,0,0}},resistances={[2]={45,45,0,0}}}})
      assert(UnitArmor('player')==48 and UnitDamage('player')==123)
      assert(UnitStat('player',1)==100 and UnitResistance('player',2)==45)
      assert(UnitHealth('player')==health)
      -- Reload state rebuilds closures/frame controls and retains logical places, clearing held items.
      local text=__WBReloadStateText() assert(not string.find(text,'["bagsInstalled"]',1,true))
      assert(not string.find(text,'["bagCursor"]',1,true))
      __WBInventory({{key='wow',form=0xfe800,name='Test Staff',pos=160,count=1,entry=123456,display=20298,quality=3}},2)
      GameTooltip:SetBagItem(4,16) assert(GameTooltipTextLeft1:GetText()=='Test Staff')
    )");
    // Drive the actual client split dialog against the portable model and delayed host snapshots.
    struct CursorCommand { std::string action,key,expected,split; int count; uint32_t to,revision; };
    std::vector<CursorCommand> pending;
    wowcore::BagPiles model;
    model.reconcile({{"arrows",12},{"sword",1}});
    wowcore::setHostHandler(L,[&](lua_State* state,const std::string& name) {
        if (name=="bagCursor") pending.push_back({lua_tostring(state,2),lua_tostring(state,3),
            lua_tostring(state,6),lua_tostring(state,7),static_cast<int>(lua_tointeger(state,4)),
            static_cast<uint32_t>(lua_tointeger(state,5)),static_cast<uint32_t>(lua_tointeger(state,8))});
        return 0;
    });
    auto publish=[&](uint32_t revision) {
        std::string list="__WBInventory({";
        for (const auto& [key,pile] : model.piles) {
            list+="{key='"+key+"',identity='"+pile.identity+"',name='"+pile.identity+"',kind='Ammo',form=42,pos="+
                std::to_string(pile.pos)+",count="+std::to_string(pile.count)+"},";
        }
        list+="},1,'"+model.held+"',"+std::to_string(model.origin)+","+std::to_string(revision)+")";
        run(list.c_str());
    };
    auto flush=[&]() {
        for (const auto& c : pending) {
            if (c.action=="grab") REQUIRE(model.grab(c.key,c.count,c.split));
            else if (c.action=="place") REQUIRE(model.place(c.key,c.to,c.expected));
            else if (c.action=="cancel") model.cancel();
            else if (c.action=="drop") model.debit(c.key,model.piles.at(c.key).count);
            else FAIL("unknown cursor command");
            publish(c.revision);
        }
        pending.clear();
    };
    run("__WBResetBags() OpenAllBags(1)");
    publish(0);
    run(R"(
      local button
      for n=1,16 do local b=getglobal('ContainerFrame1Item'..n) if b:GetID()==1 then button=b end end
      assert(button)
      __WB.modifiers={shift=1} button:Click('LeftButton') __WB.modifiers={}
      assert(StackSplitFrame:IsShown() and StackSplitFrame.maxStack==12)
    )");
    textures.sync(ui.frame(0));
    const auto& tree=ui.engine().widgets();
    const auto* splitFrame=tree.findByName("StackSplitFrame");
    REQUIRE(splitFrame);
    CHECK(splitFrame->rectW==172);
    CHECK(splitFrame->rectH==96);
    bool splitBackground=false;
    for (auto id : splitFrame->children) {
        const auto* region=tree.get(id);
        if (region && region->texturePath=="Interface\\MoneyFrame\\UI-MoneyFrame") {
            splitBackground=true;
            CHECK(region->left==splitFrame->left);
            CHECK(region->bottom==splitFrame->bottom);
            CHECK(region->rectW==splitFrame->rectW);
            CHECK(region->rectH==splitFrame->rectH);
        }
    }
    REQUIRE(splitBackground);
    for (const char* name : {"StackSplitLeftButton","StackSplitRightButton","StackSplitText"}) {
        const auto* region=tree.findByName(name);
        REQUIRE(region);
        CHECK(region->left>=splitFrame->left);
        CHECK(region->left+region->rectW<=splitFrame->left+splitFrame->rectW);
        CHECK(region->bottom>=splitFrame->bottom);
        CHECK(region->bottom+region->rectH<=splitFrame->bottom+splitFrame->rectH);
    }
    run(R"(
      StackSplitFrameRight_Click() StackSplitFrameRight_Click() StackSplitFrameOkay_Click()
      assert(not StackSplitFrame:IsShown() and __WB.bagHeld.count==3)
      assert(__WB.bagItems[1].count==9 and __WB.bagHeld.pos==0)
      GameTooltip:SetBagItem(0,1)
      local split,drop
      for n=1,GameTooltip:NumLines() do local line=getglobal('GameTooltipTextLeft'..n):GetText()
        split=split or line=='<Shift Click to Split>' drop=drop or line=='<Drag Out to Drop>'
      end
      assert(split and drop)
      PickupContainerItem(0,3)
      assert(__WB.bagItems[3].count==3 and not CursorHasItem())
      __wbSplitKey=__WB.bagItems[3].key
    )");
    flush(); // the earlier grab snapshot must not undo the later optimistic placement
    CHECK(model.total("arrows")==12);
    run(R"(
      assert(__WB.bagItems[1].count==9 and __WB.bagItems[3].count==3 and not CursorHasItem())
      SplitContainerItem(0,3,2) PickupContainerItem(0,2)
      assert(not CursorHasItem() and not __WB.bagHeld and __WB.bagItems[3].count==1)
      assert(__WB.bagItems[4].identity=='sword')
      PickupContainerItem(0,2) PickupContainerItem(0,1)
      assert(__WB.bagItems[1].count==11 and not __WB.bagItems[2] and not CursorHasItem())
      SplitContainerItem(0,1,0) SplitContainerItem(0,1,12) SplitContainerItem(0,1,-1) SplitContainerItem(0,1,1.5)
      assert(not CursorHasItem())
      SplitContainerItem(0,1,2) ToggleGameMenu()
      assert(not CursorHasItem() and __WB.bagItems[1].count==11)
      SplitContainerItem(0,1,2) UseContainerItem(0,1)
      assert(not CursorHasItem() and __WB.bagItems[1].count==11)
      SplitContainerItem(0,1,2) assert(__WBBagWorldClick(1)==1)
      assert(not CursorHasItem() and __WB.bagItems[1].count==11)
      SplitContainerItem(0,1,2) __WB.setCursor({spell=774})
      assert(not __WB.bagHeld and CursorHasSpell() and __WB.bagItems[1].count==11) ClearCursor()
      SplitContainerItem(0,1,2)
      local state=__WBReloadStateText()
      assert(not CursorHasItem() and __WB.bagItems[1].count==11)
      assert(not string.find(state,'["bagHeld"]',1,true))
    )");
    flush();
    CHECK(model.total("arrows")==12);
    run(R"(
      assert(__WB.bagItems[1].count==11 and __WB.bagItems[3].count==1)
      SplitContainerItem(0,1,2) assert(__WBBagWorldClick(0)==1)
    )");
    flush();
    CHECK(model.total("arrows")==10);
    run("assert(not CursorHasItem() and __WB.bagItems[1].count==9 and __WB.bagItems[3].count==1)");
    // Every reviewed icon is a real vanilla texture, so missing art cannot silently use the question mark.
    run(R"(
      __WBBagIconPaths={}
      local function visit(t)
        for _,v in pairs(t) do if type(v)=='table' then visit(v) else tinsert(__WBBagIconPaths,'Interface\\Icons\\'..v..'.blp') end end
      end
      visit(__WBBagIcons)
      assert(__WBBagIcon({potion='health'})=='Interface\\Icons\\INV_Potion_51')
      assert(__WBBagIcon({potion='magicka'})=='Interface\\Icons\\INV_Potion_76')
      assert(__WBBagIcon({potion='stamina'})=='Interface\\Icons\\INV_Potion_43')
      assert(__WBBagIcon({kind='Potion',keywords={'VendorItemFood'}})=='Interface\\Icons\\INV_Misc_Food_14')
      assert(__WBBagIcon({keywords={'VendorItemGem'}})=='Interface\\Icons\\INV_Misc_Gem_Sapphire_02')
      assert(__WBBagIcon({armorSlot='hands',armorType=1})=='Interface\\Icons\\INV_Gauntlets_04')
    )");
    lua_getglobal(L,"__WBBagIconPaths");
    const auto count=lua_objlen(L,-1);
    for (size_t n=1;n<=count;++n) {
        lua_rawgeti(L,-1,static_cast<int>(n));
        const std::string path=lua_tostring(L,-1);
        INFO(path);
        CHECK(mpq->read(path).has_value());
        lua_pop(L,1);
    }
    lua_pop(L,1);
}

TEST_CASE("mouse drags release bag swaps and retain bag and character cursors for world drops", "[client][api][bags][drag]") {
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
        INFO((result ? lua_tostring(L,-1) : ""));
        REQUIRE(result==0);
    };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run(R"(
      __WBLogin() __WBSkyrimMenus()
      __WBInventory({{key='arrows',identity='arrows',name='Arrows',form=42,kind='Ammo',pos=1,count=12},
        {key='sword',identity='sword',name='Sword',form=43,kind='Weapon',weapon=1,pos=2,count=1}},1,'',0,0)
      OpenAllBags(1)
      function __wbBagButton(slot)
        for f=1,NUM_CONTAINER_FRAMES do local frame=getglobal('ContainerFrame'..f)
          if frame:IsShown() and frame:GetID()==0 then
            for n=1,16 do local b=getglobal(frame:GetName()..'Item'..n)
              if b:GetID()==slot then return b:GetName() end
            end
          end
        end
      end
    )");
    std::vector<std::string> commands;
    wowcore::setHostHandler(L,[&](lua_State* s,const std::string& name) {
        if (name=="bagCursor") commands.emplace_back(lua_tostring(s,2));
        return 0;
    });
    auto frames=[&]() { for (int n=0;n<3;++n) textures.sync(ui.frame(1.f/60)); };
    auto centre=[&](const std::string& name) {
        frames();
        const auto& tree=ui.engine().widgets();
        const auto* w=tree.findByName(name);
        REQUIRE(w);
        const float scale=tree.uiScale();
        return std::pair{(w->left+w->rectW/2)*scale,ui.height()-(w->bottom+w->rectH/2)*scale};
    };
    auto bagButton=[&](int slot) {
        REQUIRE(luaL_dostring(L,("return __wbBagButton("+std::to_string(slot)+")").c_str())==0);
        REQUIRE(lua_isstring(L,-1));
        const std::string name=lua_tostring(L,-1); lua_pop(L,1);
        return name;
    };
    auto mouse=[&](float x,float y,bool held) {
        ui.pointer(x,y); ui.engine().dispatchMouse(x,y,ui.height(),{held,false,false}); frames();
    };
    auto begin=[&](const std::string& name) {
        const auto [x,y]=centre(name);
        mouse(x,y,false); mouse(x,y,true); mouse(x+8,y,true);
        run("assert(CursorHasItem() and __WBBagCursorHeld()==1)");
    };
    auto releaseWorld=[&]() {
        mouse(1280,300,true); mouse(1280,300,false);
        CHECK_FALSE(ui.engine().mouseOverFrameXml());
        CHECK_FALSE(ui.engine().holdsMousePress());
        // This is the boundary WowUi reads after dispatch, before Bridge::HandleClicks.
        run("assert(CursorHasItem() and __WBBagCursorHeld()==1) assert(__WBBagWorldClick(0)==1)");
        REQUIRE(commands.back()=="drop");
        CHECK(std::ranges::find(commands,"cancel")==commands.end());
    };
    begin(bagButton(1));
    const auto [x,y]=centre(bagButton(2)); mouse(x,y,true); mouse(x,y,false);
    run("assert(__WB.bagItems[1].key=='sword' and __WB.bagItems[2].key=='arrows' and not CursorHasItem() and not __WB.cursorTexture())");
    REQUIRE(commands==std::vector<std::string>{"grab","place"});
    commands.clear();
    begin(bagButton(2)); releaseWorld(); // whole stack
    run("ClearCursor() SplitContainerItem(0,2,3) assert(__WB.bagHeld.count==3)");
    commands.clear();
    mouse(1280,300,true); mouse(1280,300,false); // a split quantity already on the cursor
    run("assert(__WB.bagHeld.count==3) assert(__WBBagWorldClick(0)==1)");
    REQUIRE(commands==std::vector<std::string>{"drop"});
    run(R"(
      ClearCursor()
      __WBInventory({{key='dual',identity='dual',name='Twin Swords',form=43,kind='Weapon',weapon=1,
        inventorySlots={16,17},worn=true,pos=1,count=3}},1,'',0,__WB.bagRevision)
      CloseAllBags() ToggleCharacter('PaperDollFrame')
    )");
    commands.clear();
    begin("CharacterMainHandSlot");
    run("assert(__WB.bagEquipSlot==16 and __WB.bagHeld.count==1 and GetInventoryItemLink('player',16))");
    releaseWorld();
    REQUIRE(commands==std::vector<std::string>{"undress","drop"});
    run("assert(GetInventoryItemLink('player',16)) ClearCursor()"); // equipment stays until the host accepts
}

TEST_CASE("all 95 imported gear tooltips resolve client tokens and display icons", "[client][equipment][tooltip]") {
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
        INFO((result ? lua_tostring(L,-1) : ""));
        REQUIRE(result==0);
    };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run("__WBLogin()");
    std::ifstream input(std::string(WOWBRIDGE_ROOT)+"/build/gear.json");
    REQUIRE(input.good());
    const auto gear=nlohmann::json::parse(input);
    REQUIRE(gear.at("items").size()==95);
    const auto push=[&](auto&& self,const nlohmann::json& value)->void {
        if (value.is_object() || value.is_array()) {
            lua_newtable(L);
            for (const auto& [key,v]:value.items()) {
                if (value.is_array()) lua_pushinteger(L,std::stoi(key)+1);
                else lua_pushstring(L,key.c_str());
                self(self,v); lua_settable(L,-3);
            }
        } else if (value.is_string()) lua_pushstring(L,value.get<std::string>().c_str());
        else if (value.is_number()) lua_pushnumber(L,value.get<double>());
        else if (value.is_boolean()) lua_pushboolean(L,value.get<bool>());
        else lua_pushnil(L);
    };
    push(push,gear.at("items")); lua_setglobal(L,"__gearAudit");
    for (const auto& item:gear.at("items")) {
        INFO(item.at("name").get<std::string>());
        REQUIRE(item.at("wow").value("RequiredLevel",0)==0);
        const auto icon=db.itemIcon(item.at("displayid"));
        REQUIRE_FALSE(icon.empty());
        CHECK_FALSE(wowcore::loadImage(*mpq,icon+".blp").rgba.empty());
    }
    CHECK(db.itemIcon(31797)=="Interface\\Icons\\INV_Chest_Cloth_06");
    run(R"(
      local p=__WB.resolve('player')
      local comparison=CreateFrame('GameTooltip','WBGearAuditCompare',UIParent,'GameTooltipTemplate')
      local function check(tip,item)
        assert(tip:NumLines()>0,item.name..' empty tooltip')
        for n=1,tip:NumLines() do
          local fs=getglobal(tip:GetName()..'TextLeft'..n)
          local text=fs:GetText() or ''
          assert(not strfind(text,'$',1,true),item.name..': '..text)
          assert(not strfind(text,'Requires Level',1,true),item.name..': '..text)
          if strfind(text,'Classes: ',1,true)==1 then
            local r,g,b=fs:GetTextColor()
            local met=math.mod(math.floor(item.wow.AllowableClass/1024),2)==1
            assert(r==1 and math.abs(g-(met and 1 or 32/255))<0.000001 and b==g,item.name..' class color')
          end
        end
      end
      for _,lv in ipairs({2,60}) do
        p.level=lv
        for _,native in ipairs({false,true}) do
          p.skyrim=native and {Destruction=50,Restoration=50} or nil
          for entry,item in pairs(__gearAudit) do
            item.display=item.displayid item.key=entry item.pos=1 item.count=1
            item.texture='Interface\Icons\INV_Misc_QuestionMark' -- stale host texture
            __WBInventory({item},1)
            assert(item.texture==__WBItemIcon(item.display),item.name..' icon')
            GameTooltip:SetBagItem(0,1)
            check(GameTooltip,item)
            assert(select(5,GetItemInfo(item.link))==0,item.name..' item info level')
            assert(IsUsableItem(item.link)==1,item.name..' usability')
            GameTooltip:SetHyperlink(item.link) check(GameTooltip,item)
            ItemRefTooltip:SetHyperlink(item.link) check(ItemRefTooltip,item)
            local slots=__WoweeCompareSlots[select(9,GetItemInfo(item.entry))]
            assert(slots,item.name..' comparison slots')
            __WB.equipment[slots[1]]=item
            GameTooltip:SetInventoryItem('player',slots[1]) check(GameTooltip,item)
            assert(comparison:SetHyperlinkCompareItem(item.link,1)) check(comparison,item)
            __WB.liveMerchant=true __WB.merchant={items={item},buyback={item}}
            GameTooltip:SetMerchantItem(1) check(GameTooltip,item)
            assert(comparison:SetMerchantCompareItem(1,1),item.name..' merchant comparison') check(comparison,item)
            GameTooltip:SetBuybackItem(1) check(GameTooltip,item)
            __WB.merchant=nil
            __WB.liveLoot=true __WB.lootSession='audit' __WB.lootItems={item}
            GameTooltip:SetLootItem(1) check(GameTooltip,item)
            __WB.lootSession=nil __WB.lootItems={}
            __WBApplyLive({mouseover={object=item.name,action='Right-click: Pick up'}}) check(GameTooltip,item)
            __WBApplyLive({mouseover=false})
            local dressed=false
            local tryOn=__WBDressTryOn
            __WBDressTryOn=function(slot,display) dressed=display==item.display end
            DressUpModel:TryOn(item.link)
            assert(dressed,item.name..' low-level preview')
            __WBDressTryOn=tryOn
          end
        end
      end
      -- A stale export and a captured pre-item_template tooltip must also lose the gate.
      local staff=__gearAudit['873']
      staff.wow.RequiredLevel=35
      __WBInventory({staff},1)
      assert(staff.wow.RequiredLevel==0)
      GameTooltip:SetBagItem(0,1)
      check(GameTooltip,staff)
      local legacy={entry=873,display=staff.display,key='legacy',pos=1,count=1,name=staff.name,
        tooltip={{left=staff.name},{left='Requires Level 35',r=1,g=32/255,b=32/255},
          {left='Requires Engineering (300)',r=1,g=32/255,b=32/255}}}
      __WBBagPrepareItem(legacy)
      __WBBagTooltip(GameTooltip,legacy)
      assert(GameTooltip:NumLines()==2 and GameTooltipTextLeft2:GetText()=='Requires Engineering (300)')
      local r,g,b=GameTooltipTextLeft2:GetTextColor()
      assert(r==1 and math.abs(g-32/255)<0.000001 and b==g)
      __WBInventory({staff},1)
      __WBLogin()
      assert(select(5,GetItemInfo(staff.link))==0 and IsUsableItem(staff.link)==1)
      ItemRefTooltip:Hide() comparison:Hide()
      p.level=60 p.skyrim=nil
      assert(__WB.spellText(__WB.spellInfo(21872))=='Reduces the casting time of your Regrowth spell by 0.2 sec.')
      assert(__WB.spellText(__WB.spellInfo(21973))=='-0.1 sec to the casting time of your Flash Heal spell.')
    )");
    const std::vector<std::pair<unsigned,const char*>> cenarion{{31722,"INV_Belt_06"},{31724,"INV_Boots_08"},
        {31725,"INV_Bracer_03"},{31797,"INV_Chest_Cloth_06"},{31726,"INV_Gauntlets_07"},
        {32790,"INV_Helmet_09"},{31729,"INV_Pants_06"},{32016,"INV_Shoulder_07"}};
    for (const auto& [display,icon]:cenarion) CHECK(db.itemIcon(display)==std::string("Interface\\Icons\\")+icon);
    run(R"(
      local oldAlt=IsAltKeyDown
      __WBInventory({__gearAudit['18608']},1)
      IsAltKeyDown=function() return true end
      UseContainerItem(0,1)
      assert(strfind(__WB.log[table.getn(__WB.log)],'bagUseEffect 18608',1,true))
      IsAltKeyDown=oldAlt
      __WBCooldown(1073741824+1031,1800)   -- the staves' shared category
      local start,duration,enable=GetContainerItemCooldown(0,1)
      assert(duration==1800 and enable==1)
      local list={}
      for _,entry in ipairs({'16828','16829','16830','16833','16831','16834','16835','16836'}) do
        local item=__gearAudit[entry]
        item.pos=table.getn(list)+1 item.worn=true item.kind='Armor'
        item.inventorySlots={item.InventoryType==20 and 5 or item.InventoryType}
        tinsert(list,item)
      end
      local staff,chest=__gearAudit['873'],__gearAudit['16897']
      staff.pos=9 staff.kind='Weapon' chest.pos=10 chest.kind='Armor'
      tinsert(list,staff) tinsert(list,chest)
      __WBPlayerLevel(2) __WB.resolve('player').skyrim={}
      __WBInventory(list,1)
      ToggleCharacter('PaperDollFrame')
      for _,item in ipairs(list) do if item.worn then assert(GetInventoryItemTexture('player',item.inventorySlots[1])==__WBItemIcon(item.display)) end end
      UIErrorsFrame:Hide()
      GameTooltip:SetOwner(UIParent,'ANCHOR_NONE') GameTooltip:SetBagItem(0,10)
      GameTooltip:ClearAllPoints() GameTooltip:SetPoint('TOPLEFT',UIParent,'TOPLEFT',440,-50)
      local tip=CreateFrame('GameTooltip','WBGearRequirementTip',UIParent,'GameTooltipTemplate')
      tip:SetOwner(UIParent,'ANCHOR_NONE') tip:SetBagItem(0,9)
      tip:ClearAllPoints() tip:SetPoint('TOPLEFT',UIParent,'TOPLEFT',760,-50)
    )");
    for (int frame=0;frame<600;++frame) textures.sync(ui.frame(.1f));
    auto* draw=ui.frame(.1f); textures.sync(draw);
    wowcore::Image rendered(2560,1440); wowcore::rasterize(draw,textures,rendered);
    REQUIRE(wowcore::writePng(std::string(WOWBRIDGE_ROOT)+"/build/gear-feedback.png",rendered));
}

TEST_CASE("imported gear tooltips give the converted Skyrim armour and damage in the client's lines", "[client][equipment][tooltip]") {
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
        INFO((result ? lua_tostring(L,-1) : ""));
        REQUIRE(result==0);
    };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run("__WBLogin()");
    std::ifstream input(std::string(WOWBRIDGE_ROOT)+"/build/gear.json");   // data/build_esp.py's export
    REQUIRE(input.good());
    const auto gear=nlohmann::json::parse(input).at("items");
    // As Bags.cpp describes them, before Skyrim has said anything of a copy: the export's own conversion.
    std::string items="__gear={";
    for (const char* entry : {"16897","873","17182","19019"}) {
        const auto& item=gear.at(entry);
        items+=std::format("['{}']={{entry={},form={},name={},display={},quality={},kind='{}',pos=1,count=1,key='{}',"
            "skyrim={{armor={},damage={}}},wow={{class={},subclass={},InventoryType={},armor={},delay={},"
            "dmg_min1={},dmg_max1={},dmg_min2={},dmg_max2={},dmg_type2={}}}}},",entry,entry,item.at("form").get<int>(),
            nlohmann::json(item.at("name").get<std::string>()).dump(),item.at("displayid").get<int>(),item.at("quality").get<int>(),
            item.at("kind")=="WEAP" ? "Weapon" : "Armor",entry,item["skyrim"].value("armor",0),item["skyrim"].value("damage",0),
            item.at("class").get<int>(),item.at("subclass").get<int>(),item.at("InventoryType").get<int>(),
            item["wow"].value("armor",0),item["wow"].value("delay",0),item["wow"].value("dmg_min1",0),item["wow"].value("dmg_max1",0),
            item["wow"].value("dmg_min2",0),item["wow"].value("dmg_max2",0),item["wow"].value("dmg_type2",0));
    }
    run((items+"}").c_str());
    CHECK(gear.at("16897").at("skyrim").at("armor")==45);   // a Dragonscale Cuirass at level 48, epic
    CHECK(gear.at("873").at("skyrim").at("damage")==22);     // an Elven Warhammer at level 21, swung at 3.7 s
    run(R"(
      local function lines(item)
        __WBInventory({item},1)
        GameTooltip:SetOwner(UIParent,'ANCHOR_NONE') GameTooltip:SetBagItem(0,1)
        local out={}
        for n=1,GameTooltip:NumLines() do
          local right=getglobal('GameTooltipTextRight'..n)
          right=right:IsShown() and right:GetText() or ''
          tinsert(out,getglobal('GameTooltipTextLeft'..n):GetText()..(right~='' and '|'..right or ''))
        end
        return '\n'..table.concat(out,'\n')..'\n'
      end
      local function has(text,line) assert(strfind(text,'\n'..line..'\n',1,true),line..' in'..text) end
      local chest=lines(__gear['16897'])
      has(chest,'45 Armor') assert(not strfind(chest,'225 Armor',1,true))
      -- Staff of Jordan: 22 over WoW's 119 - 180; Sulfuras 26 over 223 - 372; Thunderfury 11 over both its ranges.
      local staff=lines(__gear['873'])
      has(staff,'18 - 26 Damage|Speed 3.70') has(staff,'(5.9 damage per second)')
      local sulfuras=lines(__gear['17182'])
      has(sulfuras,'19 - 33 Damage|Speed 3.70') has(sulfuras,'(7.0 damage per second)')
      local thunderfury=lines(__gear['19019'])
      has(thunderfury,'5 - 12 Damage|Speed 1.90') has(thunderfury,'2 - 3 Nature Damage') has(thunderfury,'(5.8 damage per second)')
      -- Their swing as the service rolls it (skill, gear, perks: service/melee.py weapon_tooltip), this copy's tempering
      -- on top; damage per second is that swing's, the second school scaled alongside.
      __WB.skyrimWeapons={[873]={min=24.6,max=30.4},[19019]={min=14,max=20}}
      local wielded=__gear['873'] wielded.damage=33 wielded.plainDamage=22
      local text=lines(wielded) has(text,'37 - 46 Damage|Speed 3.70') has(text,'(11.1 damage per second)')
      wielded.damage=nil wielded.plainDamage=nil
      text=lines(__gear['19019']) has(text,'14 - 20 Damage|Speed 1.90') has(text,'3 - 6 Nature Damage') has(text,'(8.9 damage per second)')
      __WB.skyrimWeapons=nil
      -- Skyrim's own word on a copy wins: a tempered chest, their skill and perks.
      local tempered=__gear['16897'] tempered.armor=52.6 tempered.temper='Fine'
      local text=lines(tempered) has(text,'53 Armor') has(text,'Improved: Fine')
      assert(not strfind(text,'Skyrim Armor',1,true) and not strfind(text,'45 Armor',1,true))
      local swung=__gear['873'] swung.damage=33
      has(lines(swung),'26 - 40 Damage|Speed 3.70')
      -- An export older than the conversion still reads as WoW's item.
      local old=__gear['16897'] old.skyrim=nil old.armor=nil old.temper=nil
      has(lines(old),'225 Armor')
    )");
}

TEST_CASE("Armory items read as their 1.12 items from armory.json's compact rows", "[client][equipment][tooltip][armory]") {
    const char* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW 1.12 client data not present");
    std::ifstream input(std::string(WOWBRIDGE_ROOT)+"/build/armory.json");   // data/build_armory.py's export
    if (!input.good()) SKIP("build/armory.json not built");
    const auto armory=nlohmann::json::parse(input);
    auto mpq=std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq,db,textures,2560,1440);
    std::string err;
    REQUIRE(ui.start(&err));
    auto* L=ui.lua();
    auto run=[&](const char* script) {
        const int result=luaL_dostring(L,script);
        INFO((result ? lua_tostring(L,-1) : ""));
        REQUIRE(result==0);
    };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run("__WBLogin()");
    // As skse/src/Armory.cpp's Row and Bags describe them: the row without its columns' defaults, its Skyrim stats,
    // quality and display, and its set with every member's name.
    auto items=nlohmann::json::object();
    for (const char* entry : {"12640","10623","16921","20130","23558","18820"}) {
        const auto& row=armory.at("items").at(entry);
        auto item=nlohmann::json{{"entry",std::stoi(entry)},{"key",entry},{"form",0x02000800+row.at("form").get<int>()},{"pos",1},{"count",1},
            {"name",row.at("wow").at("name")},{"display",row.at("wow").at("displayid")},{"quality",row.at("wow").at("Quality")},
            {"wow",row.at("wow")},{"skyrim",row.at("skyrim")},{"kind",row.at("kind")=="WEAP" ? "Weapon" : "Armor"}};
        if (row.contains("set")) item["set"]=armory.at("sets").at(std::to_string(row.at("set").get<int>()));
        items[entry]=item;
    }
    const auto push=[&](auto&& self,const nlohmann::json& value)->void {
        if (value.is_object() || value.is_array()) {
            lua_newtable(L);
            for (const auto& [key,v]:value.items()) {
                if (value.is_array()) lua_pushinteger(L,std::stoi(key)+1);
                else lua_pushstring(L,key.c_str());
                self(self,v); lua_settable(L,-3);
            }
        } else if (value.is_string()) lua_pushstring(L,value.get<std::string>().c_str());
        else if (value.is_number()) lua_pushnumber(L,value.get<double>());
        else if (value.is_boolean()) lua_pushboolean(L,value.get<bool>());
        else lua_pushnil(L);
    };
    push(push,items); lua_setglobal(L,"__armory");
    CHECK(armory.at("items").at("12640").at("skyrim").at("armor")==23);   // an Ebony Helmet's 21 at level 37, epic
    CHECK_FALSE(armory.at("items").at("12640").at("wow").contains("RequiredLevel"));   // Skyrim gear has no level gate
    run(R"(
      local function lines(item)
        __WBInventory({item},1)
        GameTooltip:SetOwner(UIParent,'ANCHOR_NONE') GameTooltip:SetBagItem(0,1)
        local out={}
        for n=1,GameTooltip:NumLines() do
          local right=getglobal('GameTooltipTextRight'..n)
          right=right:IsShown() and right:GetText() or ''
          tinsert(out,getglobal('GameTooltipTextLeft'..n):GetText()..(right~='' and '|'..right or ''))
        end
        return '\n'..table.concat(out,'\n')..'\n'
      end
      local function has(text,line) assert(strfind(text,'\n'..line..'\n',1,true),line..' in'..text) end
      local helm=lines(__armory['12640'])
      has(helm,'Lionheart Helm') has(helm,'Binds when equipped') has(helm,'Head|Plate') has(helm,'23 Armor')
      has(helm,'+18 Strength') has(helm,'Durability 100 / 100')
      assert(strfind(helm,'\nEquip: ',1,true) and not strfind(helm,'565 Armor',1,true) and not strfind(helm,'Requires Level',1,true))
      assert(GetContainerItemInfo(0,1)==__WBItemIcon(22920))
      -- Winter's Bite: an axe (subclass 0, kept), Skyrim's 9 spread over WoW's 47 - 88 at its 2.1 s swing.
      local axe=lines(__armory['10623'])
      has(axe,"Winter's Bite") has(axe,'Main Hand|Axe') has(axe,'6 - 12 Damage|Speed 2.10') has(axe,'(4.3 damage per second)')
      assert(strfind(axe,'\nChance on hit: ',1,true))
      local _,_,quality,level,_,kind,sub=GetItemInfo(10623)
      assert(quality==3 and level==48 and kind=='Weapon' and sub=='One-Handed Axes')
      -- Halo of Transcendence: its set as WoW lists it, nothing worn yet.
      local halo=lines(__armory['16921'])
      has(halo,'Vestments of Transcendence (0/8)') has(halo,'  Belt of Transcendence')
      assert(strfind(halo,'\n(3) Set: ',1,true) and strfind(halo,'\n(8) Set: ',1,true))
      -- Old compact Armory exports have no `use` metadata. Derive it from item_template;
      -- two worn trinkets still leave the spare flask in a bag.
      local flask,shell,power=__armory['20130'],__armory['23558'],__armory['18820']
      flask.count=2 flask.pos=1 flask.worn=true flask.inventorySlots={13}
      shell.pos=2 shell.worn=true shell.inventorySlots={13}
      power.pos=3
      __WBInventory({flask,shell,power},1)
      __WBInstallCharacter()
      assert(__WB.equipment[13].entry==20130 and __WB.equipment[14].entry==23558)
      assert(flask.use.spell==24427 and flask.use.category==1141 and flask.use.cooldown_ms==360000)
      assert(flask.use.category_cooldown_ms==60000 and shell.use.spell==29506)
      assert(flask.use.ready_category==1141 and shell.use.ready_category==0)
      UseInventoryItem(13)
      assert(strfind(__WB.log[table.getn(__WB.log)],'bagUseEffect 20130',1,true),tostring(__WB.log[table.getn(__WB.log)]))
      UseInventoryItem(14)
      assert(strfind(__WB.log[table.getn(__WB.log)],'bagUseEffect 23558',1,true))
      UseContainerItem(0,1)
      assert(strfind(__WB.log[table.getn(__WB.log)],'bagUseEffect 20130',1,true))
      local oldAlt=IsAltKeyDown IsAltKeyDown=function() return true end
      UseContainerItem(0,3)
      assert(strfind(__WB.log[table.getn(__WB.log)],'bagUseEffect 18820',1,true))
      IsAltKeyDown=oldAlt
      __WBRefreshCooldowns()
      -- Item timers are kept under the item (536870912 + entry); a bare spell timer does not show on the item.
      __WBCooldown(24427,500)
      assert(select(2,GetInventoryItemCooldown('player',13))==0)
      __WBCooldown(536870912+20130,360) __WBCooldown(1073741824+1141,60)
      local start,duration,enable=GetInventoryItemCooldown('player',13)
      assert(duration==360 and enable==1)
      assert(select(2,GetContainerItemCooldown(0,1))==360)
      assert(select(2,GetContainerItemCooldown(0,3))==60)
      assert(select(2,GetInventoryItemCooldown('player',14))==0)
      __WBCooldown(536870912+18820,90)
      assert(select(2,GetContainerItemCooldown(0,3))==90)
      -- The shell's category 0 checks no category, even one its spell would start.
      __WBCooldown(1073741824+0,600)
      assert(select(2,GetInventoryItemCooldown('player',14))==0)
      -- A timer held until an aura fades is enable 0: no sweep, a grey icon.
      __WBCooldown(536870912+23558,120,1)
      local _,held,on=GetInventoryItemCooldown('player',14)
      assert(held==120 and on==0)
      __WBCooldown(536870912+23558,120)
      assert(select(3,GetInventoryItemCooldown('player',14))==1)
      -- The GCD of a use spell sweeps bag and character slots, so it also fires BAG_UPDATE_COOLDOWN.
      local heard=CreateFrame('Frame') local bagEvents=0
      heard:RegisterEvent('BAG_UPDATE_COOLDOWN') heard:SetScript('OnEvent',function() bagEvents=bagEvents+1 end)
      __WBRefreshCooldowns() bagEvents=0
      __WBGcd(1.5)
      assert(bagEvents==1 and select(2,GetContainerItemCooldown(0,1))==1.5)
      assert(select(2,GetInventoryItemCooldown('player',14))==0)   -- the shell's use spell has no GCD
      heard:UnregisterEvent('BAG_UPDATE_COOLDOWN')
      __WBRefreshCooldowns() __WBCooldown(536870912+20130,360)
      -- Both bag and character drags can bind item actions without moving the equipment.
      PickupContainerItem(0,1) PlaceAction(10)
      assert(GetActionInfo(10)=='item' and select(2,GetActionInfo(10))==20130)
      assert(GetActionTexture(10)==flask.texture and IsEquippedAction(10)==1)
      assert(select(2,GetActionCooldown(10))==360)
      UseAction(10)
      assert(strfind(__WB.log[table.getn(__WB.log)],'bagUseEffect 20130',1,true))
      PickupInventoryItem(14) PlaceAction(11)
      assert(select(2,GetActionInfo(11))==23558)
      -- An equippable item that is not worn is equipped from the action bar, as the 1.12 client does.
      PickupContainerItem(0,3) PlaceAction(13)
      assert(select(2,GetActionInfo(13))==18820 and IsEquippedAction(13)==nil)
      UseAction(13)
      assert(strfind(__WB.log[table.getn(__WB.log)],'bagUse 18820',1,true),tostring(__WB.log[table.getn(__WB.log)]))
      PickupAction(13) ClearCursor()
      PickupAction(10) PlaceAction(12)
      assert(not HasAction(10) and select(2,GetActionInfo(12))==20130)
      assert(strfind(WBItemActionSlots,'12:20130',1,true))
      __WBClassSpells({lines={},spells={},unlearned_spells={},known_spells={},highest_ranks={}})
      assert(HasAction(11) and HasAction(12))
      local p=__WB.resolve('player') p.skyrim={Alteration=50,Restoration=50,Destruction=50}
      __WBSpellTooltips({[24427]={item_use=true,effects={{75,75},{9,9},{0,0}},duration_ms=60000,
        native_lines={'Skyrim: 75 carry weight','Skyrim: 37.5 One-Handed damage %'}},
        [29506]={item_use=true,effects={{900,900},{0,0},{0,0}},duration_ms=20000}})
      GameTooltip:SetAction(12)
      local text=''
      for n=1,GameTooltip:NumLines() do text=text..'\n'..getglobal('GameTooltipTextLeft'..n):GetText() end
      assert(strfind(text,'75 carry weight',1,true) and strfind(text,'37.5 One-Handed',1,true))
      GameTooltip:SetInventoryItem('player',14)
      text=''
      for n=1,GameTooltip:NumLines() do text=text..'\n'..getglobal('GameTooltipTextLeft'..n):GetText() end
      assert(strfind(text,'225',1,true) and not strfind(text,'900',1,true))
      __WBRefreshCooldowns()
      assert(select(2,GetActionCooldown(12))==0 and select(2,GetContainerItemCooldown(0,3))==0)
    )");
}

TEST_CASE("spell books and mount items read as their 1.12 items and teach when used", "[client][bags][tooltip][learning]") {
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
        INFO((result ? lua_tostring(L,-1) : ""));
        REQUIRE(result==0);
    };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run("__WBLogin()");
    std::ifstream input(std::string(WOWBRIDGE_ROOT)+"/build/gear.json");
    REQUIRE(input.good());
    const auto gear=nlohmann::json::parse(input);
    REQUIRE(gear.at("books").size()==179+142);   // the Druid's books and the Paladin's librams
    REQUIRE(gear.at("mounts").size()==23);
    const auto push=[&](auto&& self,const nlohmann::json& value)->void {
        if (value.is_object() || value.is_array()) {
            lua_newtable(L);
            for (const auto& [key,v]:value.items()) {
                if (value.is_array()) lua_pushinteger(L,std::stoi(key)+1);
                else lua_pushstring(L,key.c_str());
                self(self,v); lua_settable(L,-3);
            }
        } else if (value.is_string()) lua_pushstring(L,value.get<std::string>().c_str());
        else if (value.is_number()) lua_pushnumber(L,value.get<double>());
        else if (value.is_boolean()) lua_pushboolean(L,value.get<bool>());
        else lua_pushnil(L);
    };
    push(push,gear.at("books")); lua_setglobal(L,"__books");
    push(push,gear.at("mounts")); lua_setglobal(L,"__mountItems");
    for (const auto* section : {"books","mounts"}) for (const auto& [key,item]:gear.at(section).items()) {
        INFO(item.at("name").get<std::string>());
        if (item.at("displayid")!=0) CHECK_FALSE(db.itemIcon(item.at("displayid")).empty());
    }
    run(R"(
      -- What Bags.cpp publishes of one (its gear.json row: the client's item row and what it teaches).
      local function stack(row,key,pos)
        local item={key=key,pos=pos,form=4096+pos,count=1,kind='Book',name=row.name,entry=row.entry,display=row.displayid,
          quality=row.quality,wow=row.wow,teaches=row.teaches,texture=row.icon}
        return item
      end
      local function lines(bag,slot)
        GameTooltip:SetOwner(UIParent,'ANCHOR_NONE') GameTooltip:SetBagItem(bag,slot)
        local t={}
        for n=1,GameTooltip:NumLines() do
          local fs=getglobal('GameTooltipTextLeft'..n) local r,g,b=fs:GetTextColor()
          t[n]={text=fs:GetText(),r=r,g=g,b=b}
        end
        return t
      end
      local function find(t,text) for _,l in ipairs(t) do if l.text==text then return l end end end
      local function red(l) return l and l.r==1 and math.abs(l.g-32/255)<0.000001 and l.b==l.g end
      local function white(l) return l and l.r==1 and l.g==1 and l.b==1 end
      local function green(l) return l and l.r==0 and l.g==1 and l.b==0 end
      local function quality(l,n) local c=ITEM_QUALITY_COLORS[n]
        return math.abs(l.r-c.r)<0.00001 and math.abs(l.g-c.g)<0.00001 and math.abs(l.b-c.b)<0.00001 end
      local saber={key='swift_frostsaber',spell=23221,name='Swift Frostsaber',display=14331,speed_percent=100,cast_ms=3000,
        icon='Interface\\Icons\\Ability_Mount_WhiteTiger',learned=false}
      local rider={key='swift_green_wind_rider',spell=32295,name='Swift Green Wind Rider',display=11011,speed_percent=100,
        cast_ms=3000,icon='Interface\\Icons\\Ability_Hunter_Pet_Bat',flying=true,flight_speed_percent=280,learned=false,
        description='Summons and dismisses a rideable Wind Rider.  This is a very fast mount that can fly outdoors.'}
      __WBMountCatalog({saber,rider})
      __WB.classReply={known_spells={5185}}   -- their class book's answer, as __WBClassSpells keeps it
      __WBPlayerLevel(20)
      __WBInventory({stack(__books['21294'],'touch',1),stack(__books['900768'],'cat',2),stack(__mountItems['18766'],'reins',3),
        stack(__mountItems['25531'],'rider',4)},1)
      -- A book-only rank's 1.12 book: rare blue, the druids' class line, Skyrim's level and the rank before it in red
      -- while unmet, then the client's learning wording in green.
      local t=lines(0,1)
      assert(t[1].text=='Book of Healing Touch XI')
      assert(quality(t[1],3))
      assert(white(find(t,'Classes: Druid')))
      assert(red(find(t,'Requires Level 36')))
      assert(red(find(t,'Requires Healing Touch (Rank 10)')))
      assert(not find(t,'Already known'))
      assert(green(find(t,'Use: Teaches Healing Touch (Rank 11).')))
      assert(find(t,'<Right Click to Use>') and find(t,'<Tab, then Items: Read>'))
      local function at(text) for n,l in ipairs(t) do if l.text==text then return n end end end
      assert(at('Classes: Druid')<at('Requires Level 36') and at('Requires Level 36')<at('Requires Healing Touch (Rank 10)') and
        at('Requires Healing Touch (Rank 10)')<at('Use: Teaches Healing Touch (Rank 11).'))
      __WBPlayerLevel(40)
      __WB.classReply={known_spells={5185,9889,25297}}
      t=lines(0,1)
      assert(white(find(t,'Requires Level 36')) and white(find(t,'Requires Healing Touch (Rank 10)')) and red(find(t,'Already known')))
      -- A made-up book in the same style: common white, the druid books' icon, no rank numeral for an unranked spell.
      t=lines(0,2)
      assert(t[1].text=='Book of Cat Form' and quality(t[1],1) and green(find(t,'Use: Teaches Cat Form.')))
      assert(not find(t,'Requires Level 0') and white(find(t,'Requires Level 8')))
      local texture=GetContainerItemInfo(0,2) assert(texture==__WBItemIcon(1317) and texture=='Interface\\Icons\\INV_Misc_Book_11')
      -- 1.12's reins: epic purple, soulbound, Skyrim's level 30, the mount spell's own Use: line.
      t=lines(0,3)
      assert(t[1].text=='Reins of the Swift Frostsaber' and quality(t[1],4))
      assert(t[2].text==ITEM_SOULBOUND and white(find(t,'Requires Level 30')))
      assert(green(find(t,'Use: Summons and dismisses a rideable Swift Frostsaber.  This is a very fast mount.')))
      assert(not find(t,'Already known') and not find(t,'Requires Riding (150)'))
      -- A TBC flyer's item: the icon its mount has (no 1.12 item display shows it) and its spell's description.
      t=lines(0,4)
      assert(t[1].text=='Swift Green Wind Rider' and red(find(t,'Requires Level 46')))   -- the player is 40
      assert(green(find(t,'Use: '..rider.description)))
      assert(GetContainerItemInfo(0,4)=='Interface\\Icons\\Ability_Hunter_Pet_Bat')
      -- Right-click asks the host to use it (Bags.cpp: learn and use up, or the client's refusal).
      UseContainerItem(0,3) assert(strfind(__WB.log[table.getn(__WB.log)],'bagUse reins',1,true))
      UseContainerItem(0,1) assert(strfind(__WB.log[table.getn(__WB.log)],'bagUse touch',1,true))
      local errors={} local f=CreateFrame('Frame') f:RegisterEvent('UI_ERROR_MESSAGE') f:SetScript('OnEvent',function() tinsert(errors,arg1) end)
      __WBItemError('ERR_CANT_EQUIP_LEVEL_I',36) __WBItemError('ERR_PROFICIENCY_NEEDED') __WBItemError('SPELL_FAILED_SPELL_LEARNED')
      __WBItemError('ERR_CANT_EQUIP_EVER')
      assert(errors[1]=='You must reach level 36 to use that item.' and errors[2]=='You do not have the required proficiency for that item.')
      assert(errors[3]=='You have already learned the spell' and errors[4]=='You can never use that item.')
      -- Their General tab lists the mounts the player has learned, and no other; a learned mount's reins are "Already known".
      local function listed(spell) for _,id in ipairs(__WB.spellbook) do if id==spell then return true end end end
      assert(not listed(23221) and not listed(32295) and __WB.spellInfo(32295))
      saber.learned=true __WBMountCatalog({saber,rider})
      assert(listed(23221) and not listed(32295))
      assert(red(find(lines(0,3),'Already known')))
      __WBLearnedSpell(23221)
      rider.learned=true __WBMountCatalog({saber,rider}) assert(listed(23221) and listed(32295))
      saber.learned=false rider.learned=false __WBMountCatalog({saber,rider})
      assert(not listed(23221) and not listed(32295))
      __WBPlayerLevel(60)
    )");
    CHECK(ui.engine().lastError().empty());
}
