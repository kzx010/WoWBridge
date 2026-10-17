#include <catch_amalgamated.hpp>
#include "wowcore/character.hpp"
#include "wowcore/character.h"
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/player_model.hpp"
#include "wowcore/soft_raster.hpp"
#include "wowcore/ui_session.hpp"
#include "wowcore/wowapi.hpp"
#include "addons/lua_engine.hpp"
#include "ui/widget_tree.hpp"
extern "C" {
#include <lauxlib.h>
}
#include <algorithm>
#include <cstdlib>
#include <filesystem>

TEST_CASE("Paladin identity reaches client APIs and class race controls", "[client][api][character][identity]") {
    const char* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW client absent");
    auto mpq=std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq,db,textures,1280,720);
    std::string error; REQUIRE(ui.start(&error));
    auto* L=ui.lua();
    const auto run=[&](const std::string& script) {
        const int rc=luaL_dostring(L,script.c_str()); INFO(script); INFO((rc ? lua_tostring(L,-1) : "")); REQUIRE(rc==0);
    };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    run("__WBFixture('idle')"); REQUIRE(ui.loadInterface(&error)); run("__WBLogin()");
    std::string classes="classes={";
    for (const auto& c : wowcore::characterClasses(*mpq)) {
        classes+="["+std::to_string(c.id)+"]={name='"+c.name+"',token='"+c.token+"',races={";
        for (const auto r : c.races) classes+="["+std::to_string(r)+"]=true,";
        classes+="}},";
    }
    run(classes+"}");
    run(R"(
      defaults={}
      for _,race in ipairs({1,3,4,6}) do
        defaults[race]={}
        for sex=0,1 do defaults[race][sex]={race=race,sex=sex,skin=0,face=0,hairStyle=0,hairColor=0,facialHair=0} end
      end
      calls={} __WB.host=function(...) tinsert(calls,arg) end
      for _,race in ipairs({1,3}) do for sex=0,1 do
        __WB.spellbook={5176,768,20580,6603} __WB.actions={[1]={spell=5176},[73]={spell=768}}
        __WBAppearance(defaults[race][sex],defaults,2,classes)
        local c,t=UnitClass('player') assert(c=='Paladin' and t=='PALADIN')
        local r,token=UnitRace('player') assert(r==(race==1 and 'Human' or 'Dwarf') and token==r)
        assert(UnitSex('player')==sex+2 and UnitClassification('player')=='normal')
        assert(UnitIsPlayer('player') and UnitCreatureType('player')=='Humanoid' and UnitHasRelicSlot('player'))
        assert(GetNumShapeshiftForms()==0 and GetNumTalentTabs()==0 and UnitCharacterPoints('player')==0)
        -- Attack, then their race's General racials (actives first); no Druid spell or Night Elf racial stays.
        local general=race==1 and {6603,20600,20597,20598,20599,20864} or {6603,20594,2481,20595,20596}
        assert(table.getn(__WB.spellbook)==table.getn(general) and not __WB.actions[1] and not __WB.actions[73])
        for i,id in ipairs(general) do assert(__WB.spellbook[i]==id,id) end
        assert(not __WB.spellInfo(20600).passive and __WB.spellInfo(20598).passive and not __WB.spellInfo(20594).passive)
        __WBDruidForms() __WBDefaultFormActions() assert(GetNumShapeshiftForms()==0 and not __WB.actions[73])
        -- Their own trees arrive from Progression (here a stand-in tree): a Paladin's talents are accepted.
        __WBTalents({ok=true,trees={{name='Holy',icon='Interface\\Icons\\Spell_Holy_HolyBolt',points_spent=0,
          background='PaladinHoly',talents={}}},talents={},level=60,points_spent=0,points_available=51})
        assert(GetNumTalentTabs()==1)
        ToggleTalentFrame() TalentFrame_Update()
        assert(TalentFrame:IsShown() and not TalentFrameTalent1:IsVisible()) HideUIPanel(TalentFrame)
        PaperDollFrame_SetLevel()
        assert(string.find(CharacterLevelText:GetText(),r,1,true) and string.find(CharacterLevelText:GetText(),'Paladin',1,true))
        __WBCustomizeAppearance()
        assert(WBCustomizeClass2:GetChecked() and WBCustomizeClass2HighlightText:GetText()=='Paladin')
        assert(WBCustomizeRace1:IsEnabled()==1 and WBCustomizeRace3:IsEnabled()==1)
        for _,id in ipairs({2,4,5,6,7,8}) do assert(getglobal('WBCustomizeRace'..id):IsEnabled()==0) end
        WBCustomizeRace4:Click() assert(not WBCustomizeRace4:GetChecked())
        assert(getglobal('WBCustomizeGender'..(sex==1 and 'Female' or 'Male')):GetChecked())
        WBCustomizeCancel:Click()
      end end
      -- Choosing a class only changes the preview until accepted; Cancel keeps the host's identity.
      __WBAppearance(defaults[4][1],defaults,11,classes,true) __WBCustomizeAppearance() calls={}
      WBCustomizeClass2:Click()
      assert(WBCustomizeClass2:GetChecked() and WBCustomizeRace1:GetChecked())
      assert(WBCustomizeRace4:IsEnabled()==0 and UnitClass('player')=='Druid' and table.getn(calls)==0)
      WBCustomizeCancel:Click() assert(calls[1][1]=='appearance' and calls[1].n==1)
      __WBCustomizeAppearance() WBCustomizeClass2:Click() WBCustomizeRace3:Click() WBCustomizeGenderMale:Click()
      calls={} WBCustomizeAccept:Click()
      assert(calls[1][1]=='testCommand' and calls[1][2]=='appearance' and calls[1][3]=='2' and calls[1][4]=='3,0,0,0,0,0,0')
      __WBAppearance(defaults[3][0],defaults,2,classes,true)
      __WBLogin() assert(UnitClass('player')=='Paladin' and GetNumTalentTabs()==0 and GetNumShapeshiftForms()==0)
      -- Their auras ride the stance bar (AttributesEx2 USE_SHAPESHIFT_BAR): the highest rank of each, the one on their active.
      local book=__WB.spellbook __WB.spellbook={635,465,10290,7294} __WBPaladinAuras()
      assert(GetNumShapeshiftForms()==2 and __WB.forms[1].spell==10290 and __WB.forms[2].spell==7294)
      local _,devotion,on=GetShapeshiftFormInfo(1) assert(devotion=='Devotion Aura' and not on)
      local _,retribution=GetShapeshiftFormInfo(2) assert(retribution=='Retribution Aura')
      __WBAuraAdd('player',10290,false,0) _,_,on=GetShapeshiftFormInfo(1) assert(on==1)
      __WBAuraRemove('player',10290) _,_,on=GetShapeshiftFormInfo(1) assert(not on)
      __WB.spellbook=book __WBPaladinAuras()
      __WBCustomizeAppearance() WBCustomizeClass11:Click()
      assert(WBCustomizeRace4:IsEnabled()==1 and WBCustomizeRace6:IsEnabled()==1 and WBCustomizeRace1:IsEnabled()==0)
      WBCustomizeCancel:Click()
      -- Established characters can change race/looks, but neither click nor a stale handler changes class.
      for _,id in ipairs({11,2}) do
        local race=id==11 and 4 or 1
        __WBAppearance(defaults[race][1],defaults,id,classes,false) __WBCustomizeAppearance() calls={}
        assert(WBCustomizeClass11:IsEnabled()==0 and WBCustomizeClass2:IsEnabled()==0)
        local other=id==11 and 2 or 11
        getglobal('WBCustomizeClass'..other):Click()
        getglobal('WBCustomizeClass'..other):GetScript('OnClick')()
        assert(getglobal('WBCustomizeClass'..id):GetChecked())
        WBCustomizeAccept:Click()
        assert(table.getn(calls)==1 and calls[1][1]=='appearance' and calls[1][2]==race)
      end
      -- Repeated identity publications and login keep learned ground/synthetic mounts and non-spell actions.
      -- Start reads saved actions before the mount catalog has supplied its synthetic spells.
      __WB.mountCatalog={} __WB.spellbook={5176,6603,23221}
      __WB.actions={[1]={spell=5176},[2]={spell=23221},[3]={spell=900001},[5]={item=123},[6]={macro=1}}
      __WBAppearance(defaults[1][1],defaults,2,classes,false)
      assert(not __WB.actions[1] and __WB.actions[2].spell==23221 and __WB.actions[3].spell==900001)
      assert(__WB.actions[5].item==123 and __WB.actions[6].macro==1)
      __WBMountCatalog({{key='swift_frostsaber',spell=23221,learned=true},
        {key='test_flyer',spell=900001,learned=true,name='Test flyer',icon='Interface\\Icons\\Ability_Mount_Gryphon_01'},
        {key='unlearned',spell=23227,learned=false}})
      __WB.actions={[1]={spell=5176},[2]={spell=23221},[3]={spell=900001},[4]={spell=23227},
        [5]={item=123},[6]={macro=1},[7]={spell=6603}}
      for n=1,3 do
        __WBAppearance(defaults[1][1],defaults,2,classes,false)
        if n==2 then __WBLogin() end
        local seen={} for _,spell in ipairs(__WB.spellbook) do seen[spell]=true end
        assert(seen[6603] and seen[23221] and seen[900001] and not seen[5176] and not seen[23227])
        assert(__WB.actions[2].spell==23221 and __WB.actions[3].spell==900001)
        assert(__WB.actions[5].item==123 and __WB.actions[6].macro==1 and __WB.actions[7].spell==6603)
        assert(not __WB.actions[1] and not __WB.actions[4])
      end
      -- Native abilities have metadata once published, but still belong to Skyrim rather than the class kit.
      __WBSkyrimSpells({{id=-0x12FCC,name='Healing'}})
      __WBSkyrimPowers({{id=-0xFE001234,name='Native power',kind='greater'}})
      __WBSkyrimShouts({{id=-0xE40C3,name='Unrelenting Force',words={
        {name='Force',spell=123,learned=true,unlocked=true,recovery=15}}}})
      __WB.actions[8]={spell=-0x12FCC} __WB.actions[9]={spell=-0xFE001234} __WB.actions[10]={spell=-0xE40C3}
      for n=1,3 do
        __WBAppearance(defaults[1][1],defaults,2,classes,false)
        __WBLogin() -- ReloadUI's login path republishes the empty class too
        assert(__WB.actions[8].spell==-0x12FCC and __WB.actions[9].spell==-0xFE001234 and __WB.actions[10].spell==-0xE40C3)
        local seen={} for _,id in ipairs(__WB.spellbook) do seen[id]=true end
        assert(seen[-0x12FCC] and seen[-0xFE001234] and seen[-0xE40C3] and not seen[5176])
      end
    )");
    run("__WB.actions[6]=nil"); // the dummy macro above only exercises filtering; macros have no client backend
    REQUIRE(luaL_dostring(L,"return __WBReloadStateText()") == 0);
    const std::string reload=lua_tostring(L,-1); lua_pop(L,1);
    wowcore::SoftTextures freshTextures;
    wowcore::UiSession fresh(mpq,db,freshTextures,1280,720);
    REQUIRE(fresh.start(&error));
    REQUIRE(luaL_dostring(fresh.lua(),reload.c_str())==0);
    const bool freshLoaded=fresh.loadInterface(&error);
    INFO(error); REQUIRE(freshLoaded);
    const int reloaded=luaL_dostring(fresh.lua(),R"(
      __WBLogin()
      assert(UnitClass('player')=='Paladin')
      assert(__WB.actions[8].spell==-0x12FCC and __WB.actions[9].spell==-0xFE001234 and __WB.actions[10].spell==-0xE40C3)
      assert(not __WB.actions[1] and not __WB.actions[4])
    )");
    INFO((reloaded ? lua_tostring(fresh.lua(),-1) : "")); REQUIRE(reloaded==0);
    CHECK(fresh.engine().lastError().empty());
    CHECK(ui.engine().lastError().empty());
}

TEST_CASE("live CharacterFrame uses Skyrim equipment, skills and native Skyrim statistics", "[client][api][character]") {
    const char* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq=std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq,db,textures,1280,720);
    std::string err;
    REQUIRE(ui.start(&err));
    auto* L=ui.lua();
    auto run=[&](const char* script) {
        const int rc=luaL_dostring(L,script);
        INFO(script);
        INFO((rc ? lua_tostring(L,-1) : ""));
        REQUIRE(rc==0);
    };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/hud_states.lua").c_str())==0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    std::vector<std::string> commands;
    wowcore::setHostHandler(L,[&](lua_State* s,const std::string& name) {
        if (name=="bagCursor") commands.push_back(std::string(lua_tostring(s,2))+":"+lua_tostring(s,3)+":"+std::to_string(int(lua_tonumber(s,5))));
        if (name=="menu") commands.push_back(std::string("menu:")+lua_tostring(s,2));
        return 0;
    });
    run(R"(
      __WBLogin() __WBSkyrimMenus()
      assert(GetBindingAction('C')=='TOGGLECHARACTER0')
      __WBApplyLive({player={health=123,healthMax=150,power=80,powerMax=100,
        skyrim={['One-Handed']=45,['Two-Handed']=50,Archery=37,Block=22,Smithing=67,Restoration=65},
        armor={176,176,0,0,0},damage={70,90,0,0,0,0,1},attackPower={250,0,0},attackSpeed=3,
        nativeSheet={health=123,healthMax=150,magicka=80,magickaMax=100,stamina=90,staminaMax=110,
          weight=42,carry=300,armor=176,damage=70,offDamage=25,fire=45,frost=20,shock=15,magic=10,poison=5,disease=100},
        attackBothHands={300,0,0,0},stats={{100,100,0,0},{90,90,0,0},{80,80,0,0},{70,70,0,0},{60,60,0,0}},
        resistances={[2]={45,45,0,0},[3]={15,15,0,0},[4]={20,20,0,0},[5]={0,0,0,0},[6]={0,0,0,0}}}})
      __WBInventory({{key='helm',form=1,name='Iron Helmet',kind='Armor',armorSlot='head',armorType=1,
          armor=12,weight=5,value=60,worn=true,inventorySlots={1},pos=1,count=1},
        {key='staff',form=2,name='Staff of Jordan',kind='Weapon',weapon=8,entry=873,worn=true,
          inventorySlots={16},pos=2,count=1},
        {key='sword',form=3,name='Iron Sword',kind='Weapon',weapon=1,pos=3,count=2}},1,'',0,0)
      CharacterMicroButton:Click('LeftButton')
      assert(CharacterFrame:IsShown() and PaperDollFrame:IsShown())
      assert(CharacterHeadSlot.hasItem and CharacterMainHandSlot.hasItem and not CharacterChestSlot.hasItem)
      assert(CharacterChestSlotIconTexture:GetTexture()==CharacterChestSlot.backgroundTextureName)
      assert(UnitHealth('player')==123 and UnitAttackSpeed('player')==3)
      assert(CharacterStatFrame1Label:GetText()=='Health:' and CharacterStatFrame1StatText:GetText()=='150')
      assert(CharacterStatFrame2StatText:GetText()=='100')
      assert(CharacterStatFrame3StatText:GetText()=='110')
      assert(CharacterStatFrame4StatText:GetText()=='42 / 300')
      assert(CharacterStatFrame5StatText:GetText()=='100%')
      assert(CharacterArmorFrameStatText:GetText()=='176' and CharacterAttackFrameStatText:GetText()=='70')
      assert(CharacterAttackPowerFrameStatText:GetText()=='25' and MagicResText1:GetText()=='45%')
      assert(not CharacterFrameTab3:IsShown() and not CharacterFrameTab5:IsShown())
      assert(not GetContainerItemLink(0,1) and not GetContainerItemLink(0,2))
      GameTooltip:SetOwner(CharacterHeadSlot,'ANCHOR_RIGHT')
      assert(GameTooltip:SetInventoryItem('player',1)==1)
      assert(GameTooltipTextLeft1:GetText()=='Iron Helmet')
      for n=1,GameTooltip:NumLines() do assert(not string.find(getglobal('GameTooltipTextLeft'..n):GetText(),'<Drag',1,true)) end
      ToggleCharacter('SkillFrame')
      assert(SkillFrame:IsShown() and GetNumSkillLines()==19)
      assert(SkillTypeLabel1:GetText()=='Skyrim')
      assert(SkillRankFrame2SkillName:GetText()=='One-Handed' and SkillRankFrame2SkillRank:GetText()=='45/100')
      SetSelectedSkill(6) SkillFrame_UpdateSkills()
      assert(SkillDetailStatusBarSkillName:GetText()=='Smithing')
      __WBApplyLive({player={skyrim={Smithing=68}}})
      assert(SkillDetailStatusBarSkillRank:GetText()=='68/100')
      CollapseSkillHeader(1) assert(GetNumSkillLines()==1 and GetSelectedSkill()==0)
      ExpandSkillHeader(0) assert(GetNumSkillLines()==19)
      ToggleCharacter('ReputationFrame')
      assert(not ReputationFrame:IsShown() and PaperDollFrame:IsShown())
      ToggleCharacter('HonorFrame') assert(not HonorFrame:IsShown())
      ToggleCharacter('PaperDollFrame')
      __WBApplyLive({player={attackPower={300,0,0},stats={{101,101,0,0}}}})
      assert(CharacterAttackPowerFrameStatText:GetText()=='25' and CharacterStatFrame1StatText:GetText()=='150')
      __WBApplyLive({player={nativeSheet={health=120,healthMax=150,magicka=80,magickaMax=100,stamina=75,staminaMax=110,
        weight=50,carry=320,armor=200,damage=72,offDamage=25,fire=45,frost=20,shock=15,magic=10,poison=5,disease=100}}})
      assert(CharacterStatFrame1StatText:GetText()=='150' and CharacterStatFrame4StatText:GetText()=='50 / 320')
      CharacterHeadSlot:Click('RightButton')
    )");
    REQUIRE_FALSE(commands.empty());
    CHECK(commands.back()=="unequip:helm:1");
    run(R"(
      CharacterMainHandSlot:Click('LeftButton')
      assert(__WB.bagHeld.key=='staff' and __WB.bagHeld.count==1 and __WB.bagEquipSlot==16)
      assert(GetInventoryItemLink('player',16) and CharacterMainHandSlot.hasItem)
      __WBInventory({},1,'',0,0) -- stale acknowledgement cannot clear a newer equipment gesture
      assert(__WB.bagHeld.key=='staff')
      PickupContainerItem(0,4)
      assert(not __WB.bagHeld and GetInventoryItemLink('player',16)) -- waits for native confirmation
      __WBInventory({{key='staff',form=2,name='Staff of Jordan',kind='Weapon',weapon=8,entry=873,pos=4,count=1},
        {key='sword',form=3,name='Iron Sword',kind='Weapon',weapon=1,pos=3,count=2}},1,'',0,100)
      assert(GetContainerItemLink(0,4) and not GetInventoryItemLink('player',16))
      PickupContainerItem(0,3) assert(CursorCanGoInSlot(16) and CursorCanGoInSlot(17) and not CursorCanGoInSlot(1))
      CharacterSecondaryHandSlot:Click('LeftButton')
      assert(not __WB.bagHeld)
      HideUIPanel(CharacterFrame)
      CharacterMicroButton:Click('LeftButton') assert(PaperDollFrame:IsShown())
      __WBCharacterLook({race=4,sex=1,skin=5,face=8,hairStyle=5,hairColor=3},{{slot=15,displayId=20298,sheath=2}})
    )");
    CHECK(std::find(commands.begin(),commands.end(),"undress:staff:16")!=commands.end());
    CHECK(std::find(commands.begin(),commands.end(),"equip:sword:17")!=commands.end());
    CHECK(std::none_of(commands.begin(),commands.end(),[](const auto& cmd) { return cmd.starts_with("menu:"); }));
    // A cold character pane cannot upload its model texture while hidden. The runtime
    // feedback probe must open the pane before waiting for this readiness predicate.
    run("HideUIPanel(CharacterFrame)");
    textures.sync(ui.frame(0.1f));
    run("assert(__WBPlayerModelReady()==0) ToggleCharacter('PaperDollFrame')");
    auto* draw=ui.frame(0.1f); textures.sync(draw);
    wowcore::Image image(1280,720); wowcore::rasterize(draw,textures,image);
    CHECK(ui.engine().widgets().findByName("CharacterModelFrame")->hostModel);
    run("assert(__WBPlayerModelReady()==1)");
    // A depth-tested dressed model changes with the actual client's rotation control.
    wowcore::PlayerModel model(*mpq);
    model.setLook(wowcore::dressCharacter(*mpq,{4,1,5,8,5,3,0},{{15,20298,2}}));
    REQUIRE(model.ready());
    const auto front=model.render(0.61f), side=model.render(1.8f);
    CHECK(front.rgba!=side.rgba);
    size_t covered=0;
    for (size_t i=3;i<front.rgba.size();i+=4) if (front.rgba[i]) ++covered;
    CHECK(covered>1000);
    CHECK(covered<65536);
    run(R"(
      ClearCursor()
      __WBInventory({
        {key='headA',identity='a',name='Helmet',form=1,kind='Armor',armorSlot='head',worn=true,inventorySlots={1},pos=1,count=1},
        {key='headB',identity='b',name='Circlet',form=2,kind='Armor',armorSlot='head',worn=true,inventorySlots={1},pos=2,count=1},
        {key='extra',name='Modded Tail',form=3,kind='Armor',armorSlot='unmapped',worn=true,inventorySlots={},pos=3,count=1},
        {key='duals',identity='dual',name='Twin Swords',form=4,kind='Weapon',weapon=1,worn=true,inventorySlots={16,17},pos=4,count=3},
        {key='ringA',name='Ring A',form=5,kind='Armor',armorSlot='ring',worn=true,inventorySlots={11},pos=5,count=1},
        {key='ringB',name='Ring B',form=6,kind='Armor',armorSlot='ring',worn=true,inventorySlots={11},pos=6,count=1},
        {key='ringC',name='Ring C',form=7,kind='Armor',armorSlot='ring',worn=true,inventorySlots={11},pos=7,count=1}},1,'',0,200)
      assert(__WB.equipment[1].key=='headA' and not GetContainerItemLink(0,1))
      assert(GetContainerItemLink(0,2) and __WB.bagItems[2].bagEquipped)
      assert(GetContainerItemLink(0,3) and __WB.bagItems[3].bagEquipped)
      OpenBackpack() __WBBagsChanged()
      local marker=false for n=1,16 do local b=getglobal('ContainerFrame1Item'..n) if b:GetID()==3 then marker=b.wbEquipped:GetText()=='E' end end assert(marker)
      CloseAllBags()
      local _,count=GetContainerItemInfo(0,4) assert(count==1 and not __WB.bagItems[4].bagEquipped)
      assert(__WB.equipment[16]==__WB.equipment[17] and GetInventoryItemCount('player',16)==1)
      assert(__WB.equipment[11].key=='ringA' and __WB.equipment[12].key=='ringB' and __WB.bagItems[7].bagEquipped)
      GameTooltip:SetOwner(UIParent,'ANCHOR_NONE') GameTooltip:SetBagItem(0,3)
      local equipped=false for n=1,GameTooltip:NumLines() do if getglobal('GameTooltipTextLeft'..n):GetText()=='Equipped' then equipped=true end end
      assert(equipped)
      PickupContainerItem(0,4)
      assert(__WB.bagHeld.count==1 and not __WB.bagEquipSlot and GetInventoryItemLink('player',16))
      ClearCursor()
      PickupInventoryItem(17)
      assert(__WB.bagHeld.count==1 and __WB.bagEquipSlot==17 and GetInventoryItemLink('player',16) and GetInventoryItemLink('player',17))
      ClearCursor() assert(GetInventoryItemLink('player',17) and not __WB.bagHeld)
      __WB.dressItems={[16897]={display=31797,type=20,sheath=0}}
      ShowUIPanel(DressUpFrame) DressUpModel:SetUnit('player')
      assert(DressUpModel:GetModel()=='__dress' and DressUpModel:GetScale()==1)
      assert(DressUpModel:TryOn('item:16897:0:0:0'))
      assert(GetInventoryItemLink('player',17))
      DressUpModel:Undress() DressUpModel:Dress()
      assert(DressUpModel:GetModel()=='__dress')
    )");
    auto render=[&]() {
        textures.sync(ui.frame(0)); textures.sync(ui.frame(0));
        auto* draw=ui.frame(0); textures.sync(draw);
        wowcore::Image rendered(ui.width(),ui.height()); wowcore::rasterize(draw,textures,rendered);
        const auto* w=ui.engine().widgets().findByName("DressUpModel");
        const float scale=ui.engine().widgets().uiScale();
        std::vector<uint8_t> crop;
        for (int y=int(ui.height()-(w->bottom+w->rectH-40)*scale); y<int(ui.height()-w->bottom*scale); ++y)
            for (int x=int((w->left+60)*scale); x<int((w->left+w->rectW-60)*scale); ++x)
                for (int c=0;c<4;++c) crop.push_back(rendered.rgba[(y*ui.width()+x)*4+c]);
        return crop;
    };
    run("DressUpModel:Dress()");
    // Client textures are uploaded incrementally; settle the frame before comparing outfits.
    auto dressed=render();
    unsigned stable=0;
    for (unsigned n=0; n<30 && stable<4; ++n) {
        auto next=render();
        stable=next==dressed ? stable+1 : 0;
        dressed=std::move(next);
    }
    REQUIRE(stable==4);
    run("DressUpModel:Undress()");
    const auto undressed=render();
    CHECK(bool(dressed!=undressed));
    run("DressUpModel:Dress()");
    { const auto restored=render(); size_t different=0; for(size_t n=0;n<dressed.size();++n) if(dressed[n]!=restored[n]) ++different;
      INFO("preview reset differs in " << different << " / " << dressed.size() << " channels"); CHECK(bool(restored==dressed)); }
    // A TryOn changes the dressing image, while restoring it reproduces the same outfit.
    run("assert(DressUpModel:TryOn('16897'))");
    CHECK(bool(render()!=dressed));
    run("DressUpModel:Dress()");
    { const auto restored=render(); size_t different=0; for(size_t n=0;n<dressed.size();++n) if(dressed[n]!=restored[n]) ++different;
      INFO("preview reset differs in " << different << " / " << dressed.size() << " channels"); CHECK(bool(restored==dressed)); }
    // Drive both original client Model_OnUpdate handlers with actual mouse presses.
    auto& engine=ui.engine();
    for (const auto* name : {"CharacterModelFrame", "DressUpModel"}) {
        const std::string base=name;
        run(("ShowUIPanel("+std::string(base=="DressUpModel" ? "DressUpFrame" : "CharacterFrame")+")").c_str());
        textures.sync(ui.frame(0));
        for (const auto* direction : {"Left", "Right"}) {
            const auto button=base+"Rotate"+direction+"Button";
            const auto* w=engine.widgets().findByName(button);
            REQUIRE(w);
            const float scale=engine.widgets().uiScale();
            CHECK(w->rectW==35);
            CHECK(w->rectH==35);
            const float x=(w->left+w->rectW/2)*scale, y=ui.height()-(w->bottom+w->rectH/2)*scale;
            run(("__rotationBefore="+base+":GetRotation()").c_str());
            ui.pointer(x,y); engine.dispatchMouse(x,y,ui.height(),{true,false,false});
            run(("assert("+button+":GetButtonState()=='PUSHED')").c_str());
            textures.sync(ui.frame(0.2f)); textures.sync(ui.frame(0.2f));
            run(("assert(math.abs("+base+":GetRotation()-__rotationBefore)>0.1)").c_str());
            engine.dispatchMouse(x,y,ui.height(),{});
            run(("assert("+button+":GetButtonState()=='NORMAL') __rotationReleased="+base+":GetRotation()").c_str());
            textures.sync(ui.frame(0.2f));
            run(("assert("+base+":GetRotation()==__rotationReleased)").c_str());
        }
    }
    // Real pointer hover must reach aura buttons beneath the frame-lock region.
    run("HideUIPanel(CharacterFrame) HideUIPanel(DressUpFrame) __WBSkyrimAura('lock-hover','Lock hover aura','Interface\\\\Icons\\\\Spell_Nature_Rejuvenation',false,60,'Hover description') __WBPanelLocks()");
    textures.sync(ui.frame(0));
    {
        const auto& tree=engine.widgets();
        const auto* w=tree.findByName("BuffButton0"); REQUIRE(w);
        const float scale=tree.uiScale();
        const float x=(w->left+w->rectW/2)*scale, y=ui.height()-(w->bottom+w->rectH/2)*scale;
        ui.pointer(x,y); engine.dispatchMouse(x,y,ui.height(),{});
        textures.sync(ui.frame(.1f));
        run("assert(GameTooltip:IsShown() and GameTooltipTextLeft1:GetText()=='Lock hover aura') __WBSkyrimAuraRemove('lock-hover')");
    }
    run(R"(
      TalentFrame_LoadUI() ClassTrainerFrame_LoadUI() KeyBindingFrame_LoadUI() assert(TalentFrame.wbPanelLock and ClassTrainerFrame.wbPanelLock)
      local names={'CharacterFrame','DressUpFrame','QuestLogFrame','TalentFrame','MerchantFrame','WorldMapFrame',
        'LootFrame','ClassTrainerFrame','TaxiFrame','SpellBookFrame','MinimapCluster','BuffFrame',
        'GameMenuFrame','KeyBindingFrame','StackSplitFrame','ChatFrame1','ChatFrame2','PartyMemberFrame1','PartyMemberFrame2','PartyMemberFrame3','PartyMemberFrame4'}
      for n=1,NUM_CONTAINER_FRAMES do tinsert(names,'ContainerFrame'..n) end
      for _,name in ipairs(names) do
        local f=getglobal(name) assert(f and f.wbPanelLock,name)
        __WBSetFrameUnlocked(name,false) assert(not f:IsMovable())
        __WBFrameLockMenu(name)
        local found=false
        for n=1,UIDROPDOWNMENU_MAXBUTTONS do local b=getglobal('DropDownList1Button'..n)
          if b:IsShown() and b:GetText()=='Unlock Frame' then b:Click() found=true break end
        end
        assert(found and f:IsMovable())
        __WBFrameLockMenu(name)
        found=false
        for n=1,UIDROPDOWNMENU_MAXBUTTONS do local b=getglobal('DropDownList1Button'..n)
          if b:IsShown() and b:GetText()=='Lock Frame' then b:Click() found=true break end
        end
        assert(found and not f:IsMovable())
      end
      __WBFrameLocks() __WBFrameLocks()
      assert(ChatFrame1.isLocked and not ChatFrame1:IsMovable())
      this=ChatFrame1Tab FCF_Tab_OnClick('RightButton') this=nil
      local shown=0
      for n=1,UIDROPDOWNMENU_MAXBUTTONS do local b=getglobal('DropDownList1Button'..n)
        if b:IsShown() then shown=shown+1 assert(b:GetText()=='Unlock Frame') b:Click() end
      end
      assert(shown==1 and not ChatFrame1.isLocked and ChatFrame1:IsMovable())
      __WBSetFrameUnlocked('ChatFrame1',false)
      __WBApplyLive({target={guid=4242,name='Bandit',health=30,healthMax=120,power=40,powerMax=80,powerType=0}})
      __WBComboPoints(3)
      assert(ComboFrame:GetParent()==PlayerFrame and ComboFrame:IsShown())
      local anchor,relative,relativePoint,x,y=ComboFrame:GetPoint()
      assert(anchor=='TOPLEFT' and relative==PlayerFrameManaBar and relativePoint=='BOTTOMLEFT' and y<0)
      local _,p2,_,x2=ComboPoint2:GetPoint()
      assert(p2==ComboFrame and x2==16)
      this=TargetFrame TargetFrame:GetScript('OnEnter')() this=nil
      assert(TargetFrameHealthBarText:GetText()=='30 / 120' and TargetFrameHealthBarText:IsShown())
      assert(TargetFrameManaBarText:GetText()=='40 / 80' and TargetFrameManaBarText:IsShown())
      __WBApplyLive({target={guid=4242,name='Bandit',health=24,healthMax=120,power=20,powerMax=80,powerType=0}})
      assert(TargetFrameHealthBarText:GetText()=='24 / 120' and UnitHealth('target')==20)
      this=TargetFrame TargetFrame:GetScript('OnLeave')() this=nil
      assert(not TargetFrameHealthBarText:IsShown() and not TargetFrameManaBarText:IsShown())
      __WBApplyLive({target=false})
      local count=0 for _,v in ipairs(UnitPopupMenus.SELF) do if v=='WB_UNLOCK_FRAME' then count=count+1 end end assert(count==1)
      HideUIPanel(DressUpFrame) HideUIPanel(TalentFrame) ShowUIPanel(CharacterFrame)
    )");
    run(R"(
      __WBInventory({{key='bow',form=10,name='Hunting Bow',kind='Weapon',weapon=7,inventorySlots={18},worn=true,pos=1,count=1}},1)
      __WBApplyLive({player={rangedAttack={300,0},rangedAttackPower={350,0,0},rangedDamage={2,50,70,0,0,1}}})
      assert(not UnitHasRelicSlot('player'))
      PaperDollFrame_SetRangedAttack()
      assert(CharacterRangedAttackFrameLabel:GetText()=='Poison Resist:')
      assert(not CharacterRangedAttackPowerFrame:IsShown())
      assert(GetInventoryItemCount('player',18)==1)
    )");
    run("local before=CharacterModelFrame:GetRotation() Model_RotateLeft(CharacterModelFrame,0.2) assert(CharacterModelFrame:GetRotation()==before-0.2)");
    // Recreate the Lua closures and frames from the same state text ReloadUI retains in game.
    run("WBUnlockedFrames='CharacterFrame ContainerFrame1 TalentFrame' __WBFrameLocks() __WBApplyLive({target={guid=4242,name='Reload target',health=24,healthMax=120,power=20,powerMax=80,powerType=0}})");
    REQUIRE(luaL_dostring(L,"return __WBReloadStateText()") == 0);
    const std::string reload=lua_tostring(L,-1); lua_pop(L,1);
    REQUIRE(luaL_dostring(L,"return __WBSavedVariablesText()") == 0);
    const std::string saved=lua_tostring(L,-1); lua_pop(L,1);
    wowcore::SoftTextures freshTextures;
    wowcore::UiSession fresh(mpq,db,freshTextures,1280,720);
    REQUIRE(fresh.start(&err));
    REQUIRE(luaL_dostring(fresh.lua(),reload.c_str())==0);
    const bool freshLoaded=fresh.loadInterface(&err);
    INFO(err); REQUIRE(freshLoaded);
    REQUIRE(luaL_dostring(fresh.lua(),saved.c_str())==0);
    REQUIRE(luaL_dostring(fresh.lua(),R"(
      __WBLogin() __WBSkyrimMenus()
      assert(UnitName('target')=='Reload target' and UnitHealth('target')==20 and TargetFrame:IsShown())
      __WBApplyLive({target={guid=5252,name='Reload probe',health=45,healthMax=90,power=10,powerMax=40,powerType=0}})
      this=TargetFrame TargetFrame:GetScript('OnEnter')() this=nil
      assert(TargetFrameHealthBarText and TargetFrameHealthBarText:GetText()=='45 / 90' and TargetFrameHealthBarText:IsShown())
      assert(TargetFrameManaBarText and TargetFrameManaBarText:GetText()=='10 / 40' and TargetFrameManaBarText:IsShown())
      this=TargetFrame TargetFrame:GetScript('OnLeave')() this=nil
      __WBApplyLive({target=false})
      CharacterMicroButton:Click('LeftButton')
      assert(PaperDollFrame:IsVisible() and CharacterStatFrame1StatText:GetText()=='150')
      assert(GetInventoryItemTexture('player',18) and GetNumFactions()==0)
      ToggleCharacter('SkillFrame') assert(GetNumSkillLines()==19 and GetSkillLineInfo(1)=='Skyrim')
      assert(CharacterFrame:IsMovable() and ContainerFrame1:IsMovable())
      TalentFrame_LoadUI() assert(TalentFrame:IsMovable())
      assert(not CharacterFrameTab3:IsShown() and not CharacterFrameTab5:IsShown())
    )") == 0);

}

TEST_CASE("epic mount models carry animated saddles and independent rider scale", "[model][mount]") {
    const auto* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW client absent");
    const wowcore::Mpq mpq(data);
    for (const uint32_t display : {14332,14329,14331,14377,14376,14374,14582,14338,14583,
        14347,14576,14346,14339,14344,14342,10721,14349,14579,14578,14573,14575,
        8469,14584}) {   // the last two: the Paladin's Warhorse and Charger (class mounts)
        INFO(display);
        const auto look = wowcore::dressCreature(mpq, display);
        REQUIRE(look);
        const auto model = wowcore::loadModel(mpq, look->body);
        REQUIRE(model);
        REQUIRE(wowcore::findSequence(*model, 5) >= 0);
        for (const float t : {0.0f, 300.0f, 600.0f}) {
            std::vector<glm::mat4> bones;
            wowcore::poseBones(*model, wowcore::findSequence(*model, 5), t, t, bones);
            const auto at = wowcore::mountedRiderMatrix(*model, bones, look->scale);
            REQUIRE(at);
            REQUIRE(std::isfinite((*at)[3].z));
            REQUIRE((*at)[3].z > .2f);
            REQUIRE(glm::length(glm::vec3((*at)[0])) == Catch::Approx(1).margin(.01));
            REQUIRE_FALSE(wowcore::mountedRiderMatrix(*model, bones, 0));
        }
    }
    const auto rider = wowcore::dressCharacter(mpq, {}, {});
    const auto model = wowcore::loadModel(mpq, rider.body);
    REQUIRE(model);
    REQUIRE(wowcore::findSequence(*model, 91) >= 0);
    REQUIRE_FALSE(wowcore::mountedRiderMatrix({}, {}, 1));
}

TEST_CASE("flying mount models carry Fly and animated saddles", "[model][mount]") {
    const auto* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW client absent");
    const wowcore::Mpq mpq(data);
    // The taxi Riding Gryphon and the green Wind Rider (data/mounts.json). Their Stand/Walk/Run are 1.12's
    // hovering poses; neither model has Walkbackwards (13) or the Jump set (37-40).
    for (const uint32_t display : {6852u, 11011u}) {
        INFO(display);
        const auto look = wowcore::dressCreature(mpq, display);
        REQUIRE(look);
        const auto model = wowcore::loadModel(mpq, look->body);
        REQUIRE(model);
        for (const uint32_t anim : {0u, 4u, 5u, 135u}) {
            INFO(anim);
            const int seq = wowcore::findSequence(*model, anim);
            REQUIRE(seq >= 0);
            for (const float t : {0.0f, 300.0f, 600.0f}) {
                std::vector<glm::mat4> bones;
                wowcore::poseBones(*model, seq, t, t, bones);
                const auto at = wowcore::mountedRiderMatrix(*model, bones, look->scale);
                REQUIRE(at);
                REQUIRE(std::isfinite((*at)[3].x));
                REQUIRE(std::isfinite((*at)[3].z));
                REQUIRE(glm::length(glm::vec3((*at)[0])) == Catch::Approx(1).margin(.01));
            }
        }
    }
}
