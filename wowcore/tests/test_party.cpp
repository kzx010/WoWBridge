#include <catch_amalgamated.hpp>
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/ui_session.hpp"
#include "wowcore/soft_raster.hpp"
#include "addons/lua_engine.hpp"
#include "ui/widget_tree.hpp"
extern "C" {
#include <lauxlib.h>
}
#include <cstdlib>
#include <filesystem>

TEST_CASE("Skyrim party snapshots drive client frames, aliases, auras and targeting", "[client][api][party]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    auto* L = ui.lua();
    auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run(R"(
        __WBLogin()
        assert(GetNumPartyMembers()==0 and not GetPartyMember(1))
        assert(not PartyMemberFrame1:IsShown() and not PartyMemberFrame1Level)
        partyEvents={}
        local probe=CreateFrame('Frame')
        for _,e in ipairs({'PARTY_MEMBERS_CHANGED','UNIT_HEALTH','UNIT_MAXHEALTH','UNIT_LEVEL',
          'UNIT_DISPLAYPOWER','UNIT_MANA','UNIT_ENERGY','UNIT_AURA','UNIT_NAME_UPDATE'}) do probe:RegisterEvent(e) end
        probe:SetScript('OnEvent',function()
          local key=event..':'..tostring(arg1)
          partyEvents[key]=(partyEvents[key] or 0)+1
          if event=='PARTY_MEMBERS_CHANGED' then
            for i=1,GetNumPartyMembers() do assert(UnitExists('party'..i) and UnitInParty('party'..i)) end
          end
        end)
        local function member(id,name,powerType)
          return {guid=id,name=name,level=24,health=245,healthMax=310,power=72,powerMax=150,
            powerType=powerType,reaction=5,isPlayer=false,connected=true,creatureType='Humanoid',auras={}}
        end
        partySample={member(10,'Lydia',3),member(11,'Marcurio',0),member(12,'Meeko',3),member(13,'Jenassa',3),member(14,'Overflow',3)}
        partySample[3].creatureType='Beast'
        partySample[1].auras={{skyrim=123,name='Poison',texture='Interface\\Icons\\Spell_Nature_CorrosiveBreath',
          text='Lose health.',harmful=true,dispel='Poison',count=1}}
        __WBApplyLive({party=partySample,target=partySample[1]})
        assert(GetNumPartyMembers()==4 and IsPartyLeader()==1 and GetPartyLeaderIndex()==0)
        assert(UnitIsPartyLeader('player') and not UnitIsPartyLeader('party1'))
        assert(UnitName('party1')=='Lydia' and UnitName('PARTY3')=='Meeko' and not UnitExists('party5'))
        assert(UnitIsUnit('target','party1') and UnitInParty('target'))
        assert(UnitHealth('target')==245 and UnitHealthMax('target')==310)
        assert(UnitMana('party2')==72 and UnitManaMax('party2')==150 and UnitPowerType('party2')==0)
        for i=1,4 do
          local frame=getglobal('PartyMemberFrame'..i)
          assert(frame:IsShown() and frame.name:GetText()==partySample[i].name)
          assert(frame.healthbar:GetValue()==245 and frame.manabar:GetValue()==72)
          assert(getglobal('PartyMemberFrame'..i..'Level'):GetText()=='24')
        end
        assert(PartyMemberFrame1Debuff1:IsShown() and select(3,UnitDebuff('party1',1))=='Poison')
        GameTooltip:SetOwner(UIParent,'ANCHOR_NONE') GameTooltip:SetUnitDebuff('party1',1)
        assert(GameTooltipTextLeft1:GetText()=='Poison')
        local joins=partyEvents['PARTY_MEMBERS_CHANGED:nil']
        __WBApplyLive({party=partySample})
        assert(partyEvents['PARTY_MEMBERS_CHANGED:nil']==joins)
        partySample[1].health=199 partySample[1].healthMax=333 partySample[1].level=25 partySample[1].name='Lydia the Brave'
        partySample[1].power=48 partySample[1].powerMax=160 partySample[1].powerType=0
        __WBApplyLive({party=partySample})
        assert(PartyMemberFrame1Name:GetText()=='Lydia the Brave' and PartyMemberFrame1Level:GetText()=='25')
        assert(PartyMemberFrame1.healthbar:GetValue()==199 and TargetFrame.healthbar:GetValue()==199)
        assert(PartyMemberFrame1.manabar:GetValue()==48 and UnitPowerType('party1')==0)
        assert(partyEvents['UNIT_MANA:party1'] and partyEvents['UNIT_MANA:target'])
        -- The live spell service shares auras across every token for the same actor.
        __WBAuraAdd(10,9885,false,1800)
        assert(UnitBuff('party1',1) and partyEvents['UNIT_AURA:party1'] and partyEvents['UNIT_AURA:target'])
        partySample[1].auras={}
        __WBApplyLive({party=partySample})
        assert(not UnitDebuff('party1',1) and UnitBuff('party1',1)) -- Skyrim diff retains service buffs
        assert(not PartyMemberFrame1Debuff1:IsShown())
        __WBAuraRemove(10,9885)
        assert(not UnitBuff('party1',1))
        -- A follower out of loaded range stays in the roster, with client disconnected presentation.
        partySample[3].connected=false
        __WBApplyLive({party=partySample})
        assert(GetNumPartyMembers()==4 and PartyMemberFrame3:IsShown() and not UnitIsConnected('party3'))
        assert(PartyMemberFrame3Disconnect:IsShown())
        partySample[3].connected=true
        partySample[3].dead=true partySample[3].health=0
        __WBApplyLive({party=partySample})
        assert(UnitIsDead('party3') and PartyMemberFrame3.healthbar:GetValue()==0 and not PartyMemberFrame3Disconnect:IsShown())
        partySample[3].dead=false partySample[3].health=270
        -- Reorder/removal completes before events; the still-targeted departed follower now answers percent.
        __WBApplyLive({party={partySample[2],partySample[3]}})
        assert(GetNumPartyMembers()==2 and UnitName('party1')=='Marcurio' and UnitName('party2')=='Meeko')
        assert(not PartyMemberFrame3:IsShown() and not PartyMemberFrame4:IsShown())
        assert(not UnitInParty('target') and UnitHealth('target')==60 and UnitHealthMax('target')==100)
        __WBApplyLive({party=partySample})
        partySample[1].auras={{skyrim=456,name='Disease',texture='Interface\\Icons\\Spell_Shadow_CallofBone',
          text='Weakened.',harmful=true,dispel='Disease',count=1}}
        __WBApplyLive({party=partySample})
        __WBResetCombat()
        assert(not UnitDebuff('party1',1))
        __WBApplyLive({party=partySample})
        -- Reset clears the aura signature too; the same known effects can be republished after loading.
        assert(UnitDebuff('party1',1) and partyEvents['UNIT_AURA:party1'])
        partySample[1].auras=nil
        __WBApplyLive({party=partySample})
        assert(UnitDebuff('party1',1)) -- unknown effect data retains the last known list
        partyCommands={}
        __WBHost=function(...) tinsert(partyCommands,{...}) end
        TargetUnit('PARTY1')
        this=PartyMemberFrame2 arg1='LeftButton' PartyMemberFrame_OnClick() this=nil arg1=nil
        assert(partyCommands[1][1]=='command' and partyCommands[1][2]=='TargetUnit' and partyCommands[1][3]=='party1' and partyCommands[1][4]==10)
        assert(partyCommands[2][3]=='party2' and partyCommands[2][4]==11)
        SendChatMessage('Ignored','PARTY') assert(table.getn(partyCommands)==2)
        __WBApplyLive({target=false}) -- the binding toggles to partypet when that follower is already selected
    )");
    for (int i=1; i<=4; ++i) {
        const std::string script="assert(GetBindingKey('TARGETPARTYMEMBER"+std::to_string(i)+"')=='F"+std::to_string(i+1)+"')";
        run(script.c_str());
        const std::string binding="RunBinding(GetBindingAction('F"+std::to_string(i+1)+"'),'down')";
        run(binding.c_str());
    }
    run(R"(
        for i=1,4 do assert(partyCommands[i+2][3]=='party'..i and partyCommands[i+2][4]==9+i) end
        local text=__WBReloadStateText()
        __WB.tokens={} __WB.units={} __WB.liveParty=nil __WB.partyRoster=nil
        assert(loadstring(text))()
        __WBLogin()
        assert(GetNumPartyMembers()==4 and PartyMemberFrame4:IsShown() and UnitName('party3')=='Meeko')
        __partyReload=__WBReloadStateText()
        __WBApplyLive({party={}})
        assert(GetNumPartyMembers()==0 and not IsPartyLeader() and not GetPartyMember(1))
        for i=1,4 do assert(not getglobal('PartyMemberFrame'..i):IsShown() and not UnitExists('party'..i)) end
        local n=table.getn(partyCommands)
        TargetUnit('party1') assert(table.getn(partyCommands)==n)
    )");
    for (int i=1; i<=4; ++i) {
        const auto ids = ui.engine().widgets().portraitsFor("party"+std::to_string(i));
        REQUIRE_FALSE(ids.empty());
    }
    CHECK(ui.engine().lastError().empty());
    lua_getglobal(L,"__partyReload");
    const std::string saved=lua_tostring(L,-1);
    lua_pop(L,1);
    // A new session rebuilds the live-only level labels and all party frames after rehydration.
    wowcore::UiSession reloaded(mpq,db,textures,1280,720);
    REQUIRE(reloaded.start(&err));
    REQUIRE(reloaded.loadInterface(&err));
    REQUIRE(luaL_dostring(reloaded.lua(),saved.c_str())==0);
    const int result=luaL_dostring(reloaded.lua(),R"(
        __WBLogin()
        assert(GetNumPartyMembers()==4 and UnitName('party3')=='Meeko')
        for i=1,4 do
          assert(getglobal('PartyMemberFrame'..i):IsShown())
          assert(getglobal('PartyMemberFrame'..i..'Level'):GetText()==tostring(UnitLevel('party'..i)))
        end
        assert(UnitDebuff('party1',1))
    )");
    INFO((result ? lua_tostring(reloaded.lua(),-1) : ""));
    REQUIRE(result==0);
    CHECK(reloaded.engine().lastError().empty());
}
