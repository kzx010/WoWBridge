#include <catch_amalgamated.hpp>
#include "wowcore/trainer.hpp"
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

TEST_CASE("Native training quotes enforce Skyrim limits and stale requests", "[trainer]") {
    wowcore::TrainingOffer offer{22,50,0,5,270,2000};
    REQUIRE(offer.matches(22,270,0));
    CHECK_FALSE(offer.matches(21,270,0));
    CHECK_FALSE(offer.matches(22,269,0));
    CHECK_FALSE(offer.matches(22,270,1));
    offer.gold=269; CHECK_FALSE(offer.available());
    offer.gold=270; REQUIRE(offer.available());
    offer.trained=4; REQUIRE(offer.available());
    offer.trained=5; CHECK_FALSE(offer.available());
    offer.trained=0; offer.current=50; CHECK_FALSE(offer.available());
    offer.current=49; REQUIRE(offer.available());
    offer.cost=-1; CHECK_FALSE(offer.available());
    offer.cost=0; REQUIRE(offer.available());
    offer.current=-1; CHECK_FALSE(offer.available());
}

TEST_CASE("ClassTrainerFrame uses live Skyrim training and pending class ranks", "[client][api][trainer]") {
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
    const auto fixtures=std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/";
    REQUIRE(luaL_dofile(L,(fixtures+"elunara.lua").c_str())==0);
    REQUIRE(luaL_dofile(L,(fixtures+"hud_states.lua").c_str())==0);
    run("__WBFixture('idle')"); REQUIRE(ui.loadInterface(&err));
    std::vector<std::string> trainerErrors;
    auto errors=[&](const std::string& message) {
        if(message.find("Trainer")!=std::string::npos || message.find("wowapi_trainer")!=std::string::npos)
            trainerErrors.push_back(message);
    };
    ui.engine().setLuaErrorCallback(errors);
    run(R"(
      local recordedInfo=GetTrainerServiceInfo
      __WBLogin() __WBSkyrimMenus()
      __WBTrainer(nil,0) -- load/reset before the first trainer is still the recorded empty client
      assert(GetTrainerServiceInfo==recordedInfo and GetNumTrainerServices()==0)
      events={} local f=CreateFrame('Frame')
      for _,e in ipairs({'TRAINER_SHOW','TRAINER_UPDATE','TRAINER_CLOSED'}) do f:RegisterEvent(e) end
      f:SetScript('OnEvent',function() tinsert(events,event) end)
      __WBTrainerFixture()
      assert(events[1]=='TRAINER_SHOW' and ClassTrainerFrame:IsVisible(),
        table.concat(events,',')..' trainer='..tostring(ClassTrainerFrame:IsVisible())..' talent='..tostring(IsTalentTrainer()))
      assert(ClassTrainerNameText:GetText()=='Wuunferth the Unliving')
      assert(ClassTrainerTrainButton:IsEnabled()==1 and GetTrainerSelectionIndex()==2)
      assert(GetMoney()==2000*10000 and GetNumTrainerServices()==7,
        'money='..GetMoney()..' entries='..GetNumTrainerServices())
      local name,sub,kind=GetTrainerServiceInfo(2)
      assert(name=='Destruction' and sub=='Level 23' and kind=='available')
      assert(GetTrainerServiceCost(2)==270*10000)
      assert(string.find(GetTrainerGreetingText(),'0/5',1,true))
      assert(string.find(GetTrainerServiceDescription(2),'maximum: 50',1,true))
      assert(GetTrainerServiceInfo(4)=='Wrath' and GetTrainerServiceInfo(5)=='Entangling Roots')
      for i=4,7 do local _,_,type=GetTrainerServiceInfo(i) assert(type=='used') end
      ClassTrainerFrame.showSkillDetails=1 ClassTrainer_SetSelection(7)
      assert(ClassTrainerTrainButton:IsEnabled()==0)
      assert(string.find(ClassTrainerSkillDescription:GetText(),'Destruction 72',1,true))
      assert(string.find(ClassTrainerSkillDescription:GetText(),"Beyond this trainer's maximum",1,true))
      BuyTrainerService(7) assert(not __WB.trainerPending)
      ClassTrainerFrame.showSkillDetails=1 ClassTrainer_SetSelection(2)
    )");
    std::vector<std::vector<std::string>> commands;
    auto capture=[&](lua_State* s,const std::string& name) {
        if(name=="trainer") {
            std::vector<std::string> args;
            for(int n=2;n<=lua_gettop(s);++n) args.emplace_back(lua_tostring(s,n));
            commands.push_back(std::move(args));
        }
        return 0;
    };
    wowcore::setHostHandler(L,capture);
    run("ClassTrainerTrainButton:Click('LeftButton') BuyTrainerService(2)");
    REQUIRE(commands.size()==1);
    REQUIRE(commands[0]==std::vector<std::string>{"train","201","22","270","0"});
    run(R"(
      assert(__WB.trainerPending and ClassTrainerTrainButton:IsEnabled()==0)
      __WBTrainerFixture(true)
      assert(not __WB.trainerPending and GetTrainerServiceCost(2)==280*10000)
      assert(ClassTrainerTrainButton:IsEnabled()==1)
      assert(__WB.trainerEntries[4].spell==339 and __WB.trainerEntries[6].spell==5178)
      assert(__WB.trainerEntries[6].threshold==34 and not __WB.unlearned[5177])
      -- Filtering and collapsing stay the actual client controls.
      SetTrainerServiceTypeFilter('used',0) assert(GetNumTrainerServices()==2)
      SetTrainerServiceTypeFilter('used',1) assert(GetNumTrainerServices()==7)
      CollapseTrainerSkillLine(3) assert(GetNumTrainerServices()==3)
      ExpandTrainerSkillLine(0) assert(GetNumTrainerServices()==7)
      local n=table.getn(events)
      ClassTrainerCollapseAllButton:Click('LeftButton') assert(GetNumTrainerServices()==2)
      assert(table.getn(events)==n) -- UI collapse must not recurse through TRAINER_UPDATE
      ClassTrainerCollapseAllButton:Click('LeftButton') assert(GetNumTrainerServices()==7)
      ClassTrainerFrame.showSkillDetails=1 ClassTrainer_SetSelection(2)
      for _,case in ipairs({{trained=5},{current=50},{gold=279}}) do
        local t={} for k,v in pairs(__WB.trainer) do t[k]=v end
        for k,v in pairs(case) do t[k]=v end
        __WBTrainer(t,__WB.trainerRevision+1)
        assert(ClassTrainerTrainButton:IsEnabled()==0)
        BuyTrainerService(2) assert(not __WB.trainerPending)
        -- Restore a fresh quote between distinct refusal cases.
        t.current,t.trained,t.gold=23,1,1730 __WBTrainer(t,__WB.trainerRevision+1)
      end
      local revision=__WB.trainerRevision
      __WBTrainer(nil,revision-1) assert(__WB.trainer)
      reloadState=__WBReloadStateText()
    )");
    lua_getglobal(L,"reloadState"); const std::string state=lua_tostring(L,-1); lua_pop(L,1);
    wowcore::SoftTextures freshTextures;
    wowcore::UiSession fresh(mpq,db,freshTextures,2560,1440);
    REQUIRE(fresh.start(&err)); REQUIRE(fresh.loadInterface(&err));
    fresh.engine().setLuaErrorCallback(errors);
    commands.clear(); wowcore::setHostHandler(fresh.lua(),capture);
    auto freshRun=[&](const char* code) {
        const int result=luaL_dostring(fresh.lua(),code);
        INFO((result ? lua_tostring(fresh.lua(),-1) : "")); REQUIRE(result==0);
    };
    freshRun(state.c_str());
    freshRun(R"(
      -- ReloadUI's recorded closures are replaced at login, before a new host reply.
      assert(GetNumTrainerServices()==0)
      __WBLogin()
      assert(ClassTrainerFrame:IsVisible() and GetNumTrainerServices()==7,
        'visible='..tostring(ClassTrainerFrame:IsVisible())..' count='..GetNumTrainerServices()..' used='..tostring(GetTrainerServiceTypeFilter('used')))
      assert(GetTrainerServiceCost(2)==280*10000 and ClassTrainerTrainButton:IsEnabled()==1)
      CloseTrainer() CloseTrainer()
    )");
    REQUIRE(commands.size()==1); REQUIRE(commands[0][0]=="close");
    freshRun(R"(
      __WBTrainer(nil,__WB.trainerRevision+1)
      assert(not ClassTrainerFrame:IsShown() and GetNumTrainerServices()==0)
      __WBTrainer({session=202,name='Colette Marence',skill='restoration',skillName='Restoration',
        current=25,maximum=75,trained=0,limit=5,cost=300,gold=2000},__WB.trainerRevision+1)
      assert(GetNumTrainerServices()==4 and __WB.trainerEntries[4].spell==5186)
      -- Her class response is authoritative, independent of the trainer's NPC class.
      -- A priest's Restoration preview replaces the druid's Healing Touch.
      __WBClassSpells({class_id=5,lines={['56']={name='Holy',tab=2}},known_spells={},spells={},
        unlearned_spells={{spell=2061,chain=2061,name='Flash Heal',rank=1,rank_name='Rank 1',skill='restoration',
          skill_line=56,tab=2,learn_skill='Restoration',learn_skill_value=43,learn_requirements={}}}})
      assert(GetNumTrainerServices()==4 and __WB.trainerEntries[4].spell==2061)
      assert(GetTrainerServiceInfo(4)=='Flash Heal' and __WB.trainerEntries[4].threshold==43)
      __WBTrainer(nil,__WB.trainerRevision+1)
    )");
    for(const auto& message:trainerErrors) INFO(message);
    REQUIRE(trainerErrors.empty());
}
