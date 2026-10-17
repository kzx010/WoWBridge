// WoWBridge's 1.12 API against the real client's recorded answers, with the whole 1.12 interface loaded over it.
// Skips without the client data, like the other [client] tests.
#include <catch_amalgamated.hpp>
#include <imgui.h>

#include "addons/lua_engine.hpp"
#include "wowcore/apiref.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/framexml.hpp"
#include "wowcore/model.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/wowapi.hpp"
#include "wowcore/ui_session.hpp"
#include "wowcore/soft_raster.hpp"
#include "ui/interface_fonts.hpp"
#include "ui/text_markup.hpp"
#include "ui/text_wrap.hpp"

extern "C" {
#include <lauxlib.h>
}

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <utility>

TEST_CASE("expired cooldowns do not replay their ending flash on resource updates", "[client][api][cooldown-expiry]") {
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
        const int rc = luaL_dostring(L, script);
        INFO((rc ? lua_tostring(L, -1) : ""));
        REQUIRE(rc == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run(R"(
        __WBLogin()
        __cooldownNow=1000 GetTime=function() return __cooldownNow end
        for i=1,12 do __WB.actions[i]={spell=9912} end
        __WBFire('ACTIONBAR_SLOT_CHANGED',0)
        __WBGcd(1.5)
        assert(ActionButton1Cooldown:IsVisible())
    )");
    textures.sync(ui.frame(1.f / 60));
    run("__cooldownNow=1001.6");
    textures.sync(ui.frame(1.f / 60)); // let the normal completion flash start
    // Magicka regeneration repeatedly raises ACTIONBAR_UPDATE_USABLE in game.
    for (int i=0; i<150; ++i) {
        run("__cooldownNow=__cooldownNow+1/60");
        if (i % 6 == 0) run("__WBApplyLive({player={powerType=0,power=100+math.mod(math.floor(__cooldownNow*10),2)}})");
        textures.sync(ui.frame(1.f / 60));
    }
    run(R"(
        for i=1,12 do
            assert(not getglobal('ActionButton'..i..'Cooldown'):IsVisible(), 'expired GCD replayed its flash')
        end
        local start,duration,enabled=GetActionCooldown(1)
        assert(start==0 and duration==0 and enabled==1)
        local index
        for n,id in ipairs(__WB.bookLayout().slots) do if id==9912 then index=n break end end
        assert(index, 'Wrath missing from the fixture spellbook')
        assert(select(2,GetSpellCooldown(index,'spell'))==0)
        __cooldownNow=2000 __WBCooldown(9912,2) __WBGcd(1.5)
        assert(select(2,GetActionCooldown(1))==2)
        __cooldownNow=2001.6
        assert(select(2,GetActionCooldown(1))==2) -- own timer outlasts the GCD
        __cooldownNow=2002
        assert(GetActionCooldown(1)==0 and select(2,GetActionCooldown(1))==0)
        __cooldownNow=3000 __WBCooldown(9912,1) __WBGcd(1.5)
        assert(select(2,GetActionCooldown(1))==1.5)
        __cooldownNow=3001.2
        assert(select(2,GetActionCooldown(1))==1.5) -- GCD outlasts the own timer
        __cooldownNow=3001.5
        __WBFire('ACTIONBAR_UPDATE_USABLE')
        assert(GetActionCooldown(1)==0 and not ActionButton1Cooldown:IsVisible())
        __WBCooldown(9912,0) __WBGcd(0)
        assert(select(2,GetActionCooldown(1))==0)
    )");
}

TEST_CASE("loading another game clears the real action button's auto-attack flash", "[client][api][combat-reset]") {
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
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    REQUIRE(luaL_dostring(L, "__WBFixture('idle')") == 0);
    REQUIRE(ui.loadInterface(&err));
    const int rc = luaL_dostring(L, R"(
        __WBLogin()
        __WB.actions[1]={spell=6603}
        __WBFire('ACTIONBAR_SLOT_CHANGED',0)
        __WBAttacking(true)
        assert(IsCurrentAction(1) and ActionButton1.flashing==1)
        __WBCastStart(9912,2000) __WBGcd(1.5) __WBCooldown(9912,10)
        __WBResetCombat()
        assert(not __WB.attacking and not IsCurrentAction(1), 'auto-attack survived game reset')
        assert(ActionButton1.flashing==0 and not ActionButton1Flash:IsShown())
        assert(not __WB.casting and not __WB.spellTargeting and not __WB.gcd and not next(__WB.cooldowns))
        __WBResetCombat() -- repeated cleanup must remain safe
        assert(not IsCurrentAction(1) and ActionButton1.flashing==0)
        __WBAttacking(true)
        assert(IsCurrentAction(1) and ActionButton1.flashing==1)
        __WBAttacking(false)
        assert(not IsCurrentAction(1) and ActionButton1.flashing==0)
    )");
    INFO((rc ? lua_tostring(L, -1) : ""));
    REQUIRE(rc == 0);
}

TEST_CASE("action range indicators respect a charge's minimum distance", "[client][api][action-range]") {
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
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    REQUIRE(luaL_dostring(L, "__WBFixture('idle')") == 0);
    REQUIRE(ui.loadInterface(&err));
    const int rc = luaL_dostring(L, R"(
        __WBLogin()
        __WB.actions[1]={spell=16979} -- Feral Charge: a real minimum range from SpellRange.dbc
        local info=__WB.spellInfo(16979)
        assert(info.targetsUnit and info.rangeMin>0 and info.rangeMax>info.rangeMin)
        local function at(distance)
            __WBApplyLive({target={guid=4242,name='Wolf',health=100,healthMax=100,distance=distance}})
            return IsActionInRange(1)
        end
        assert(at(info.rangeMin-0.01)==0, 'charge reported in range while too close')
        assert(at(info.rangeMin)==1 and at(info.rangeMax)==1)
        assert(at(info.rangeMax+0.01)==0)
        __WB.spellTooltips=__WB.spellTooltips or {}
        __WB.spellTooltips[16979]={range_max=info.rangeMax+5}
        assert(at(info.rangeMax+1)==1 and at(info.rangeMax+5.01)==0)
        assert(at(info.rangeMin-0.01)==0)
        __WBApplyLive({target='player'}) assert(IsActionInRange(1)==0)
        __WBApplyLive({target=false}) assert(IsActionInRange(1)==nil)
        assert(IsActionInRange(120)==nil)
    )");
    INFO((rc ? lua_tostring(L, -1) : ""));
    REQUIRE(rc == 0);
}

TEST_CASE("cast probes wait for host replies without a missing-global error", "[client][api][cast-probe]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    // Reproduce the DLL's default upstream fallback, even after API tests set it off.
    struct FallbackSetting {
        bool present = std::getenv("WOWEE_LUA_API_FALLBACK") != nullptr;
        std::string saved = present ? std::getenv("WOWEE_LUA_API_FALLBACK") : "";
        FallbackSetting() { setenv("WOWEE_LUA_API_FALLBACK", "1", 1); }
        ~FallbackSetting() {
            if (present) setenv("WOWEE_LUA_API_FALLBACK", saved.c_str(), 1);
            else unsetenv("WOWEE_LUA_API_FALLBACK");
        }
    } fallback;
    REQUIRE(ui.engine().initialize());
    lua_State* L = ui.lua();
    auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    // This is the source of "table" in WowUi::Eval, before any UI error event.
    run("WBTestLastError=nil assert(rawget(_G,'WBTestLastError')==nil) assert(type(WBTestLastError)=='table')");
    std::string err;
    REQUIRE(ui.start(&err));
    run("assert(WBTestLastError==nil, 'absent error must stay nil after installing the 1.12 API')");
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/hud_states.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run(R"(
        __WBLogin()
        __wbNow=100 GetTime=function() return __wbNow end
        WBTestLastError=nil __wbCastProbe={delayed=0,starts=0}
        local f=CreateFrame('Frame')
        for _,e in ipairs({'UI_ERROR_MESSAGE','SPELLCAST_START','SPELLCAST_DELAYED','SPELLCAST_FAILED','SPELLCAST_STOP'}) do f:RegisterEvent(e) end
        f:SetScript('OnEvent',function()
            local p=__wbCastProbe p.event=event
            if event=='UI_ERROR_MESSAGE' then WBTestLastError=arg1
            elseif event=='SPELLCAST_START' then p.starts=p.starts+1 p.duration=arg2
            elseif event=='SPELLCAST_DELAYED' then p.delayed=p.delayed+arg1 end
        end)
        __WB.actions[1]={spell=6603} __WB.actions[2]={spell=9912}
        assert(__WBSlotOf(9912)==2)
        local weapon=__WB.equipment[16]
        assert(GetActionTexture(1)==weapon.texture)
        __WB.equipment[16]=nil
        assert(GetActionTexture(1)=='Interface\\Icons\\INV_Gauntlets_04')
        __WB.equipment[16]={texture=''}
        assert(GetActionTexture(1)=='Interface\\Icons\\INV_Gauntlets_04')
        __WB.equipment[16]=weapon
        assert(GetActionTexture(1)==weapon.texture)
    )");
    CHECK(mpq->exists("Interface\\Icons\\INV_Gauntlets_04.blp"));
    int requests = 0;
    wowcore::setHostHandler(L, [&](lua_State* s, const std::string& cmd) {
        if (cmd == "useAction") {
            ++requests;
            CHECK(lua_tonumber(s, 2) == 2);
            CHECK(lua_tonumber(s, 3) == 9912);
        }
        return 0;
    });
    run("UseAction(__WBSlotOf(9912))");
    REQUIRE(requests == 1);
    textures.sync(ui.frame(1.f / 60));
    run(R"(
        -- Request still pending: the next SelfTest poll must wait, not fail/reset it.
        assert(__WB.casting==nil and not CastingBarFrame:IsVisible())
        assert(__wbCastProbe.event==nil and __wbCastProbe.starts==0)
        assert(WBTestLastError==nil)
        __WBCastStart(9912,2000)
    )");
    textures.sync(ui.frame(1.f / 60));
    run(R"(
        assert(__WB.casting.spell==9912 and CastingBarFrame:IsVisible())
        assert(CastingBarText:GetText()=='Wrath')
        assert(__wbCastProbe.starts==1 and __wbCastProbe.duration==2000)
        assert(WBTestLastError==nil)
        local end0=CastingBarFrame.maxValue
        __wbNow=100.5 __WBCastDelayed(500)
        __wbNow=101.5 __WBCastDelayed(800)
        assert(__wbCastProbe.delayed==1300)
        assert(math.abs(CastingBarFrame.maxValue-end0-1.3)<0.0001)
        __WBCastStop()
        assert(WBTestLastError==nil)
        __WBCastFailed('SPELL_FAILED_LINE_OF_SIGHT')
        assert(type(WBTestLastError)=='string' and WBTestLastError==SPELL_FAILED_LINE_OF_SIGHT)
        __WBCastFailed('SPELL_FAILED_REAGENTS')
        assert(WBTestLastError==ERR_SPELL_FAILED_REAGENTS_GENERIC and not string.find(WBTestLastError,'%%s'))
        assert(__WB.casting==nil)
        -- Rearming the online probe must clear a real failure back to nil too.
        WBTestLastError=nil __WBCastStop()
        assert(WBTestLastError==nil)
    )");
}

TEST_CASE("host nearby combat uses client strings, nine chat arguments and each window's filters", "[client][api][world-combat]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
    auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/hud_states.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run(R"(
        __WBLogin()
        local captured, window1, window2 = {}, {}, {}
        local probe = CreateFrame('Frame')
        for group, events in pairs(ChatTypeGroup) do
            for _, name in ipairs(events) do probe:RegisterEvent(name) end
        end
        probe:SetScript('OnEvent', function()
            tinsert(captured, {event, arg1,arg2,arg3,arg4,arg5,arg6,arg7,arg8,arg9})
        end)
        ChatFrame_RemoveAllMessageGroups(ChatFrame1)
        ChatFrame_RemoveAllMessageGroups(ChatFrame2)
        ChatFrame_AddMessageGroup(ChatFrame1, 'COMBAT_CREATURE_VS_CREATURE_HITS')
        ChatFrame_AddMessageGroup(ChatFrame2, 'COMBAT_PARTY_HITS')
        ChatFrame1.AddMessage = function(self, text) tinsert(window1, text) end
        ChatFrame2.AddMessage = function(self, text) tinsert(window2, text) end
        local wolf = {name='Wolf', category='hostileCreature'}
        local deer = {name='Deer', category='friendlyCreature'}
        local ally = {name='Faendal', category='party'}
        local me = {name='Elunara', category='self'}
        local function check(report, group, text)
            local count = table.getn(captured)
            __WBCombatEvent(report)
            assert(table.getn(captured) == count+1)
            local args = captured[count+1]
            assert(table.getn(args) == 10) -- event plus ALL NINE arguments, including empty strings
            assert(args[1] == 'CHAT_MSG_'..group, args[1]..' expected '..group)
            assert(args[2] == text, args[2]..' expected '..text)
            for i=3,7 do assert(args[i] == '') end
            assert(args[8] == 0 and args[9] == 0 and args[10] == '')
        end
        check({kind='swing', source=wolf, target=deer, amount=12},
              'COMBAT_CREATURE_VS_CREATURE_HITS', 'Wolf hits Deer for 12.')
        check({kind='swing', source=deer, target=wolf, amount=13, outcome='crit'},
              'COMBAT_CREATURE_VS_CREATURE_HITS', 'Deer crits Wolf for 13.')
        assert(table.getn(window1) == 2 and table.getn(window2) == 0)
        check({kind='swing', source=ally, target=wolf, amount=14},
              'COMBAT_PARTY_HITS', 'Faendal hits Wolf for 14.')
        assert(table.getn(window1) == 2 and window2[1] == 'Faendal hits Wolf for 14.')
        check({kind='swing', source=wolf, target=ally, outcome='miss'},
              'COMBAT_CREATURE_VS_PARTY_MISSES', 'Wolf misses Faendal.')
        check({kind='swing', source=wolf, target=me, amount=15, outcome='crit'},
              'COMBAT_CREATURE_VS_SELF_HITS', 'Wolf crits you for 15.')
        check({kind='damage', source=wolf, target=deer, amount=16, school=3},
              'COMBAT_CREATURE_VS_CREATURE_HITS', 'Wolf hits Deer for 16 Nature damage.')
        check({kind='damage', source=wolf, target=ally, amount=17, spell='Bite'},
              'SPELL_CREATURE_VS_PARTY_DAMAGE', "Wolf's Bite hits Faendal for 17.")
        check({kind='damage', source=ally, target=wolf, amount=18, spell='Firebolt', school=2, crit=true},
              'SPELL_PARTY_DAMAGE', "Faendal's Firebolt crits Wolf for 18 Fire damage.")
        check({kind='heal', source=deer, target=ally, amount=19, spell='Mend'},
              'SPELL_CREATURE_VS_PARTY_BUFF', "Deer's Mend heals Faendal for 19.")
        check({kind='heal', source=ally, target=me, amount=20, spell='Mend', crit=true},
              'SPELL_PARTY_BUFF', "Faendal's Mend critically heals you for 20.")
        check({kind='death', target=wolf}, 'COMBAT_HOSTILE_DEATH', 'Wolf dies.')
        check({kind='death', target=deer}, 'COMBAT_FRIENDLY_DEATH', 'Deer dies.')
        check({kind='death', target=ally}, 'COMBAT_FRIENDLY_DEATH', 'Faendal dies.')
        check({kind='death', target=me}, 'COMBAT_FRIENDLY_DEATH', 'You die.')
        check({kind='kill', target=wolf}, 'COMBAT_HOSTILE_DEATH', 'You have slain Wolf!')
        -- Creature source uses the recipient's SELF/PARTY group; both creature reactions otherwise share one group.
        -- Player and party sources always use their own category, independently of the recipient's reaction.
        local categories = {'self','party','friendlyPlayer','hostilePlayer','friendlyCreature','hostileCreature'}
        local groups = {self='SELF', party='PARTY', friendlyPlayer='FRIENDLYPLAYER', hostilePlayer='HOSTILEPLAYER'}
        for _, source in ipairs(categories) do for _, target in ipairs(categories) do
            local a, b = {name='A',category=source}, {name='B',category=target}
            local group = groups[source] or ('CREATURE_VS_'..(target=='self' and 'SELF' or target=='party' and 'PARTY' or 'CREATURE'))
            if source ~= 'self' or target ~= 'self' then
                local text = (source=='self' and 'You hit ' or 'A hits ')..(target=='self' and 'you' or 'B')..' for 1.'
                check({kind='swing',source=a,target=b,amount=1}, 'COMBAT_'..group..'_HITS', text)
                text = (source=='self' and 'You miss ' or 'A misses ')..(target=='self' and 'you' or 'B')..'.'
                check({kind='swing',source=a,target=b,outcome='miss'}, 'COMBAT_'..group..'_MISSES', text)
            end
            local prefix = source=='self' and 'Your Mend' or "A's Mend"
            local recipient = target=='self' and 'you' or 'B'
            check({kind='heal',source=a,target=b,spell='Mend',amount=2},
                  'SPELL_'..group..'_BUFF', prefix..' heals '..recipient..' for 2.')
            prefix = source=='self' and 'Your Bolt' or "A's Bolt"
            check({kind='damage',source=a,target=b,spell='Bolt',amount=3,school=6},
                  'SPELL_'..group..'_DAMAGE', prefix..' hits '..recipient..' for 3 Arcane damage.')
        end end
        check({kind='swing',source=wolf,target=deer,outcome='dodge'},
              'COMBAT_CREATURE_VS_CREATURE_MISSES', 'Wolf attacks. Deer dodges.')
        check({kind='swing',source=wolf,target=me,outcome='parry'},
              'COMBAT_CREATURE_VS_SELF_MISSES', 'Wolf attacks. You parry.')
        check({kind='swing',source=me,target=wolf,outcome='block'},
              'COMBAT_SELF_MISSES', 'You attack. Wolf blocks.')
        check({kind='swing',source=wolf,target=deer,amount=4,outcome='glance',blocked=1,absorbed=2,resisted=3},
              'COMBAT_CREATURE_VS_CREATURE_HITS', 'Wolf hits Deer for 4. (glancing) (1 blocked) (2 absorbed) (3 resisted)')
        check({kind='damage',source=wolf,target=me,spell='Burn',school=2,amount=5,periodic=true},
              'SPELL_PERIODIC_SELF_DAMAGE', "You suffer 5 Fire damage from Wolf's Burn.")
        check({kind='heal',source=ally,target=deer,spell='Renew',amount=6,periodic=true},
              'SPELL_PERIODIC_CREATURE_BUFFS', "Deer gains 6 health from Faendal's Renew.")
        -- Only the two selected chat groups ever reached the two windows.
        for _, text in ipairs(window1) do assert(string.find(text, ' hits ') or string.find(text, ' crits ')) end
        for _, text in ipairs(window2) do assert(string.find(text, 'Faendal hits') or string.find(text, 'A hits')) end
        local count = table.getn(captured)
        assert(not pcall(__WBCombatEvent, {kind='swing',source=wolf,target=deer,amount=-1}))
        assert(not pcall(__WBCombatEvent, {kind='damage',source=wolf,target=deer,spell='Burn',amount=1,school=9}))
        assert(not pcall(__WBCombatEvent, {kind='swing',source={name='X',category='bad'},target=deer,amount=1}))
        assert(table.getn(captured) == count)
        -- The established own-combat host API is still byte-for-byte compatible.
        __WBLog('CHAT_MSG_COMBAT_SELF_HITS', 'COMBATHITSELFOTHER', 'Wolf', 7)
        assert(captured[count+1][2] == 'You hit Wolf for 7.')
    )");
}

TEST_CASE("host talents drive Blizzard TalentFrame, tooltips and validated learning", "[client][api][talents]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
    auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO(script);
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    for (const char* file : {"elunara.lua", "hud_states.lua", "talents.lua"}) {
        REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/" + file).c_str()) == 0);
    }
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    std::vector<std::string> learns;
    int menus = 0, talentRequests = 0;
    wowcore::setHostHandler(L, [&](lua_State* state, const std::string& cmd) {
        if (cmd == "learnTalent") learns.push_back(std::to_string(lua_tointeger(state, 2)) + ":" + std::to_string(lua_tointeger(state, 3)));
        if (cmd == "menu") ++menus;
        if (cmd == "requestTalents") ++talentRequests;
        return 0;
    });
    run(R"(
        __WBLogin()
        __WBPlayerLevel(9)
        __WBLoadBindings(1, 'bind C TOGGLECHARACTER0\n') -- old cache lacks talents
        __WBSkyrimMenus()
        assert(GetBindingAction('N') == 'TOGGLETALENTS')
        assert(not TalentMicroButton:IsVisible())
        __WBApplyLive({player={level=10}}) -- actual Skyrim level, before any talent reply
        assert(TalentMicroButton:IsVisible(), 'level='..UnitLevel('player')..' shown='..tostring(TalentMicroButton:IsShown())..' parent='..tostring(MainMenuBarArtFrame:IsShown()))
        assert(not __WB.talents and GetNumTalentTabs() == 3) -- recorded client until host data
        __WBBinding(GetBindingAction('N'), true)
        __WBBinding(GetBindingAction('N'), false)
        assert(TalentFrame and TalentFrame:IsVisible())
        HideUIPanel(TalentFrame)
        __WBPlayerLevel(40)
        local probe = CreateFrame('Frame')
        __wbTalentEvents = {}
        for _, e in ipairs({'CHARACTER_POINTS_CHANGED','SPELLS_CHANGED','UI_ERROR_MESSAGE'}) do probe:RegisterEvent(e) end
        probe:SetScript('OnEvent', function() __wbTalentEvents[event] = arg1 or true end)
        assert(not __WB.talents)
        assert(__WBTalents(__wbTalentReply))
        assert(GetNumTalentTabs() == 3 and GetNumTalents(2) == 3)
        local name, icon, points, background = GetTalentTabInfo(2)
        assert(name == 'Feral Combat' and points == 0 and background == 'DruidFeralCombat')
        assert(icon == __wbTalentReply.trees[2].icon)
        local name, icon, tier, col, rank, maxRank, exceptional, prereq = GetTalentInfo(2,3)
        assert(name == 'Feline Swiftness' and tier == 3 and col == 1 and rank == 0 and maxRank == 2)
        assert(not exceptional and prereq)
        assert(select('#', GetTalentPrereqs(2,3)) == 0)
        local tier, col, met = GetTalentPrereqs(1,2)
        assert(tier == 1 and col == 2 and not met and select('#', GetTalentPrereqs(1,2)) == 3)
        assert(UnitCharacterPoints('player') == 31 and UnitCharacterPoints('target') == 0)
        assert(__wbTalentEvents.CHARACTER_POINTS_CHANGED == 31)
        TalentMicroButton:Click('LeftButton')
        assert(TalentFrame and TalentFrame:IsVisible())
        assert(TalentFrameTalentPointsText:GetText() == '31')
        this = TalentFrameTab2
        TalentFrameTab_OnClick()
        this = nil
        assert(TalentFrameTalent3Rank:GetText() == '0')
        GameTooltip:SetOwner(TalentFrameTalent3, 'ANCHOR_RIGHT')
        assert(GameTooltip:SetTalent(2,3))
        assert(GameTooltipTextLeft1:GetText() == 'Feline Swiftness')
        assert(GameTooltipTextLeft2:GetText() == 'Rank 0/2')
        assert(GameTooltipTextLeft3:GetText() == 'Requires 10 points in Feral Combat Talents')
        assert(not string.find(GameTooltipTextLeft4:GetText(), '$', 1, true))
        assert(string.find(GameTooltipTextLeft4:GetText(), '15', 1, true))
        TalentFrameTalent3:Click('LeftButton')
        assert(GetTalentInfo(2,3) == 'Feline Swiftness' and select(5,GetTalentInfo(2,3)) == 0)
        assert(not __WBTalents({ok=false,error='BAD_REQUEST: tier_points'}))
        assert(UnitCharacterPoints('player') == 31 and __wbTalentEvents.UI_ERROR_MESSAGE == 'BAD_REQUEST: tier_points')
        local allocation=__WB.talents
        assert(not __WBTalents({ok=false,error='BAD_REQUEST: cannot learn talent: no_points'}))
        assert(__wbTalentEvents.UI_ERROR_MESSAGE==WB_ERR_NO_TALENT_POINTS and __WB.talents==allocation)
        local spent=allocation.points_spent local host=__WB.host local calls=0
        allocation.points_spent=UnitLevel('player')-9 __WB.host=function() calls=calls+1 end
        LearnTalent(2,3)
        assert(calls==0 and __wbTalentEvents.UI_ERROR_MESSAGE==WB_ERR_NO_TALENT_POINTS and __WB.talents==allocation)
        allocation.points_spent=spent __WB.host=host
        __WBBinding(GetBindingAction('N'), true)
        __WBBinding(GetBindingAction('N'), false)
        assert(not TalentFrame:IsVisible())
        __WBBinding(GetBindingAction('N'), true)
        __WBBinding(GetBindingAction('N'), false)
        assert(TalentFrame:IsVisible())
    )");
    CHECK(learns == std::vector<std::string>{"807:1"});
    CHECK(menus == 0);
    CHECK(talentRequests == 1);
    run(R"(
        -- The host publishes only after service validation. No local click spends a point.
        local tree, t = __wbTalentReply.trees[2], __wbTalentReply.trees[2].talents[3]
        tree.points_spent = 11
        __wbTalentReply.points_spent = 11
        t.rank, t.next_rank, t.learnable = 1, 2, true
        __wbTalentReply.points_available, __wbTalentReply.talents['807'] = 20, 1
        __WBTalents(__wbTalentReply)
        assert(select(5,GetTalentInfo(2,3)) == 1 and UnitCharacterPoints('player') == 20)
        assert(TalentFrameTalent3Rank:GetText() == '1')
        GameTooltip:SetTalent(2,3)
        assert(GameTooltipTextLeft2:GetText() == 'Rank 1/2')
        assert(GameTooltipTextLeft5:GetText() == TOOLTIP_TALENT_NEXT_RANK)
        assert(string.find(GameTooltipTextLeft6:GetText(), '30', 1, true))
        LearnTalent(2,3)
        t.rank, t.learnable = 2, false
        __wbTalentReply.points_available = 19
        __wbTalentReply.points_spent = 12
        __WBTalents(__wbTalentReply)
        LearnTalent(2,3) -- maxed ranks cannot request another spend
        assert(select(5,GetTalentInfo(2,3)) == 2 and TalentFrameTalentPointsText:GetText() == '19')
        -- The following known_spells answer adds the granted active spell and raises SPELLS_CHANGED.
        __WBClassSpells({lines={['573']={name='Restoration',tab=3}},known_spells={5570},
          spells={{spell=5570,skill_line=573,tab=3}},unlearned_spells={}})
        assert(__wbTalentEvents.SPELLS_CHANGED)
        local found
        for i,id in ipairs(__WB.spellbook) do if id == 5570 then found = i break end end
        local _, _, offset = GetSpellTabInfo(2)
        assert(found and GetSpellName(offset+1, 'spell') == 'Insect Swarm')
        __WBPlayerLevel(41)
        assert(UnitCharacterPoints('player') == 20) -- budget follows level before the round trip
        __wbTalentReply.level, __wbTalentReply.points_available = 41, 20
        __WBTalents(__wbTalentReply)
        assert(UnitCharacterPoints('player') == 20)
    )");
    CHECK(learns == std::vector<std::string>{"807:1", "807:2"});
    run(R"(
        __WBPlayerLevel(60)
        assert(UnitCharacterPoints('player') == 39 and TalentMicroButton:IsVisible())
        SetBinding('N', 'TOGGLECHARACTER0')
        __WBInstallTalents()
        assert(GetBindingAction('N') == 'TOGGLECHARACTER0') -- preserve customized keys
        SetBinding('N', 'TOGGLETALENTS')
    )");
    run("return __WBReloadStateText()");
    const std::string saved = lua_tostring(L, -1);
    lua_pop(L, 1);
    wowcore::UiSession reloaded(mpq, db, textures, 1280, 720);
    REQUIRE(reloaded.start(&err));
    REQUIRE(reloaded.loadInterface(&err));
    REQUIRE(luaL_dostring(reloaded.lua(), saved.c_str()) == 0);
    REQUIRE(luaL_dostring(reloaded.lua(), R"(
        __WBLogin()
        __WBSkyrimMenus()
        assert(GetNumTalentTabs()==3 and UnitCharacterPoints('player')==39)
        assert(TalentMicroButton:IsVisible() and GetBindingAction('N')=='TOGGLETALENTS')
        __WBBinding(GetBindingAction('N'),true)
        __WBBinding(GetBindingAction('N'),false)
        assert(TalentFrame:IsVisible())
    )") == 0);
    CHECK(reloaded.engine().lastError().empty());
    for (int i = 0; i < 3; ++i) textures.sync(ui.frame(1.0f / 60));
    CHECK(ui.engine().lastError().empty());
}

TEST_CASE("all Feral talents enter the book and live energy rage damage duration and party tooltips", "[client][api][talents][feral][tooltip]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 2560, 1440);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
    for (const char* file : {"elunara.lua", "hud_states.lua", "talents_render.lua", "talent_feedback.lua"})
        REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/" + file).c_str()) == 0);
    REQUIRE(luaL_dostring(L, "__WBFixture('idle')") == 0);
    REQUIRE(ui.loadInterface(&err));
    std::vector<int> casts;
    wowcore::setHostHandler(L, [&](lua_State* state, const std::string& cmd) {
        if (cmd == "castSpell") casts.push_back(static_cast<int>(lua_tointeger(state, 2)));
        return 0;
    });
    const int result = luaL_dostring(L, R"(
        __WBLogin() __WBFeralFeedbackFixture()
        local slots = {}
        for index,id in ipairs(__WB.bookLayout().slots) do slots[id]=index end
        local function text()
          local out=''
          for i=1,GameTooltip:NumLines() do out=out..(getglobal('GameTooltipTextLeft'..i):GetText() or '') end
          return out
        end
        GameTooltip:SetOwner(ActionButton2,'ANCHOR_RIGHT')
        assert(strfind(__WB.spellText(__WB.spellInfo(16940)), '0.5 sec', 1, true))
        assert(strfind(__WB.spellText(__WB.spellInfo(16941)), '1 sec', 1, true))
        for _,t in ipairs(__WB.talents.trees[2].talents) do
          local id=t.rank_spells[t.rank]
          assert(slots[id],t.name..' missing from book')
          assert(GameTooltip:SetSpell(slots[id],'spell'))
          assert(not strfind(text(),'$',1,true),t.name..' unresolved tooltip')
          local passive=t.id~=804 and t.id~=1162
          assert(not not IsSpellPassive(slots[id],'spell')==passive,t.name)
          if passive then
            assert(strfind(text(),'Passive',1,true),t.name)
            ClearCursor() PickupSpell(slots[id],'spell') assert(not __WB.cursor,t.name)
            CastSpell(slots[id],'spell')
          end
        end
        assert(GameTooltip:SetAction(2) and strfind(text(),'10 additional damage',1,true) and strfind(text(),'40 Energy',1,true))
        assert(GameTooltip:SetSpell(slots[9850],'spell') and strfind(text(),'10 additional damage',1,true) and strfind(text(),'40 Energy',1,true))
        assert(GameTooltip:SetAction(4) and strfind(text(),'48 Energy',1,true))
        assert(GameTooltip:SetAction(3) and strfind(text(),'5 sec',1,true))
        assert(__WB.manaCost(__WB.resolve('player'),150,1,9881)==10)
        __WBForm(3,3,false)
        __WB.resolve('player').power=39
        assert(select(2,IsUsableAction(2))==1)
        __WB.resolve('player').power=40
        assert(IsUsableAction(2)==1)
        -- Learned actives follow the same cast/drag path as trainer spells.
        PickupSpell(slots[16857],'spell') assert(__WB.cursor and __WB.cursor.spell==16857) ClearCursor()
        CastSpell(slots[16857],'spell')
        __WBForm(1,1,false) CastSpell(slots[16979],'spell')
        __WBAuraAdd('player',24932,false,0,{effects={{3,3}}})
        assert(GameTooltip:SetPlayerBuff(0) and strfind(text(),'3%',1,true))
        __WBApplyLive({target={guid=4242,name='Wolf',level=60,health=100,healthMax=100,reaction=2}})
        __WBAuraAdd(4242,8983,true,5,{duration_ms=5000})
        assert(GameTooltip:SetUnitDebuff('target',1) and strfind(text(),'Stunned',1,true))
    )");
    INFO((result ? lua_tostring(L,-1) : ""));
    REQUIRE(result == 0);
    CHECK(casts == std::vector<int>{16857,16979});
    for (int i=0;i<3;++i) textures.sync(ui.frame(1.0f/60));
    CHECK(ui.engine().lastError().empty());
}

TEST_CASE("learned talents label passives and update spell and snapshotted buff tooltips", "[client][api][talents][tooltip]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 2560, 1440);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
    auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO(script);
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    for (const char* file : {"elunara.lua", "hud_states.lua", "talents_render.lua", "talent_feedback.lua"})
        REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/" + file).c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    std::vector<int> casts;
    wowcore::setHostHandler(L, [&](lua_State* state, const std::string& cmd) {
        if (cmd == "castSpell") casts.push_back(static_cast<int>(lua_tointeger(state, 2)));
        if (cmd == "useAction") casts.push_back(static_cast<int>(lua_tointeger(state, 3)));
        return 0;
    });
    run(R"(
        __WBLogin()
        __WBAuraAdd('player',9885,false,1800,__WBMarkTooltip(false))
        __WBTalentFeedbackFixture()
        assert(UnitCharacterPoints('player')==40)
        local slots = {}
        for index,id in ipairs(__WB.bookLayout().slots) do slots[id]=index end
        assert(slots[5570] and slots[17055])
        assert(IsSpellPassive(slots[17055],'spell') and not IsSpellPassive(slots[5570],'spell'))
        assert(select(2,GetSpellName(slots[17055],'spell'))==PASSIVE_PARENS)
        ClearCursor()
        PickupSpell(slots[17055],'spell')
        assert(not __WB.cursor)
        CastSpell(slots[17055],'spell') -- no host cast for a passive
        __WB.actions[3]={spell=17055} -- an old saved bar must not activate a passive either
        assert(not IsUsableAction(3)) UseAction(3)
        CastSpell(slots[5570],'spell') -- granted active uses the ordinary host cast
        function __talentTipContains(a,b,c)
          local text=''
          for i=1,GameTooltip:NumLines() do text=text..(getglobal('GameTooltipTextLeft'..i):GetText() or '') end
          return strfind(text,a,1,true) and strfind(text,b,1,true) and strfind(text,c,1,true)
        end
        GameTooltip:SetOwner(ActionButton2,'ANCHOR_RIGHT')
        assert(GameTooltip:SetAction(2) and __talentTipContains('384','16','27'))
        assert(GameTooltip:SetSpell(slots[9885],'spell') and __talentTipContains('384','16','27'))
        assert(GameTooltip:SetSpell(slots[17055],'spell') and __talentTipContains('Passive','35','Mark'))
        assert(GameTooltip:SetPlayerBuff(0) and __talentTipContains('285','12','20')) -- old cast stays old
        __WBAuraAdd('player',9885,false,1800,__WBMarkTooltip(true))
        assert(GameTooltip:SetPlayerBuff(0) and __talentTipContains('384','16','27'))
        __WBSpellTooltips({['9885']=__WBMarkTooltip(false)}) -- reset affects the next cast
        assert(GameTooltip:SetAction(2) and __talentTipContains('285','12','20'))
        assert(GameTooltip:SetPlayerBuff(0) and __talentTipContains('384','16','27')) -- existing effect retained
        __WBApplyLive({target={guid=4242,name='Follower',level=60,health=100,healthMax=100,reaction=5}})
        __WBAuraAdd(4242,9885,false,1800,__WBMarkTooltip(true))
        assert(GameTooltip:SetUnitBuff('target',1) and __talentTipContains('384','16','27'))
        __WB.actions[4]={spell=9912}
        __WB.resolve('target').distance=35
        assert(IsActionInRange(4)==0)
        __WB.classReply.spell_tooltips['9912']={range_max=36}
        __WBSpellTooltips(__WB.classReply.spell_tooltips)
        assert(IsActionInRange(4)==1)
        -- Nature's Grasp's $h token follows learned chance, and a buff keeps
        -- the chance snapshotted when cast. The action and book share it.
        __WB.classReply.spell_tooltips['16689']={proc_chance=100,duration_ms=45000}
        __WBSpellTooltips(__WB.classReply.spell_tooltips)
        __WB.actions[5]={spell=16689}
        assert(GameTooltip:SetAction(5) and __talentTipContains('100%','Entangling','45'))
        __WBAuraAdd('player',16689,false,45,{proc_chance=50,duration_ms=45000})
        local index
        for i,a in ipairs(__WB.resolve('player').buffs) do if a.spell==16689 then index=i-1 break end end
        assert(index and GameTooltip:SetPlayerBuff(index))
        assert(__WB.resolve('player').buffs[index+1].tooltip.proc_chance==50)
        -- The client's aura description does not contain a $h token.
        assert(strfind(GameTooltipTextLeft2:GetText(),'chance to entangle',1,true))
    )");
    CHECK(casts == std::vector<int>{5570});
    run("return __WBReloadStateText()");
    const std::string saved = lua_tostring(L, -1);
    lua_pop(L, 1);
    wowcore::UiSession reloaded(mpq, db, textures, 2560, 1440);
    REQUIRE(reloaded.start(&err));
    REQUIRE(reloaded.loadInterface(&err));
    REQUIRE(luaL_dostring(reloaded.lua(), saved.c_str()) == 0);
    REQUIRE(luaL_dostring(reloaded.lua(), R"(
        __WBLogin()
        assert(UnitCharacterPoints('player')==40)
        GameTooltip:SetOwner(BuffButton0,'ANCHOR_BOTTOMLEFT')
        GameTooltip:SetPlayerBuff(0)
        assert(strfind(GameTooltipTextLeft2:GetText(),'384',1,true))
        GameTooltip:SetAction(2)
        local text=''
        for i=1,GameTooltip:NumLines() do text=text..(getglobal('GameTooltipTextLeft'..i):GetText() or '') end
        assert(strfind(text,'285',1,true)) -- retain the latest profile independently of the old cast
    )") == 0);
    for (int i=0; i<3; ++i) textures.sync(reloaded.frame(1.0f/60));
    CHECK(reloaded.engine().lastError().empty());
    CHECK(ui.engine().lastError().empty());
}

TEST_CASE("host class progression orders locked ranks and uses the client's learning and level chat", "[client][api][progression]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
    auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO(script);
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/hud_states.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run(R"(
        __WBLogin()
        assert(not __WB.classRows and not __WB.unlearned)
        assert(UnitSex('player') == 3)
        __WBApplyLive({target='player', player={sex=2}})
        assert(UnitSex('player') == 2 and UnitSex('target') == 2)
        __WBApplyLive({target=false, player={sex=3}})
        assert(UnitSex('player') == 3)
        __WBSkyrimSpells({{id=-123, name='Flames'}})
        __WBProgressionFixture()
        assert(GetNumSpellTabs() == 5)
        assert(GetSpellTabInfo(1) == 'General')
        assert(GetSpellTabInfo(3) == 'Feral Combat') -- pending-only tabs stay present
        local name, icon, offset, count = GetSpellTabInfo(2)
        assert(name == 'Balance' and count == 4)
        __progressOffset = offset
        assert(__WB.bookSpell(offset+1, 'spell') == 8921) -- learned Moonfire, Wrath
        assert(__WB.bookSpell(offset+2, 'spell') == 5176)
        assert(__WB.bookSpell(offset+3, 'spell') == 16914) -- then pending Hurricane, Wrath II
        assert(__WB.bookSpell(offset+4, 'spell') == 5177)
        assert(select(3, GetSpellCooldown(offset+3, 'spell')) == 0)
        assert(select(3, GetSpellCooldown(offset+2, 'spell')) == 1)
        assert(not pcall(__WBLearnedSpell, 5177))
        local _, _, skyrimOffset = GetSpellTabInfo(5)
        assert(__WB.bookSpell(skyrimOffset+1, 'spell') == -123)
        -- A later Skyrim scan preserves the class snapshot and pending entries.
        __WBSkyrimSpells({{id=-123, name='Flames'}, {id=-124, name='Frostbite'}})
        assert(select(4, GetSpellTabInfo(2)) == 4 and select(4, GetSpellTabInfo(5)) == 2)
        ToggleSpellBook(BOOKTYPE_SPELL)
        this = SpellBookSkillLineTab2
        SpellBookSkillLineTab_OnClick()
        this = nil
    )");
    for (int i = 0; i < 3; ++i) textures.sync(ui.frame(1.0f / 60));
    run(R"(
        UpdateSpells()
        assert(SpellButton1SpellName:GetText() == 'Moonfire')
        assert(SpellButton3SubSpellName:GetText() == 'Rank 1')
        assert(SpellButton5SpellName:GetText() == 'Hurricane')
        local r, g, b = SpellButton5SpellName:GetTextColor()
        assert(r == GRAY_FONT_COLOR.r and g == GRAY_FONT_COLOR.g and b == GRAY_FONT_COLOR.b)
        GameTooltip:SetOwner(SpellButton5, 'ANCHOR_RIGHT')
        assert(GameTooltip:SetSpell(__progressOffset+3, 'spell'))
        assert(getglobal('GameTooltipTextLeft'..GameTooltip:NumLines()):GetText() == 'Learned at Destruction 72')
        -- Prerequisite explanations use the same red requirement rows.
        __WB.unlearned[16914].learn_requirements = {'Requires quest-earned prerequisite'}
        GameTooltip:SetSpell(__progressOffset+3, 'spell')
        assert(getglobal('GameTooltipTextLeft'..GameTooltip:NumLines()):GetText() == 'Requires quest-earned prerequisite')
        __progressMessages, __progressEvents = {}, {}
        local add = ChatFrame1.AddMessage
        ChatFrame1.AddMessage = function(self, text, ...)
            tinsert(__progressMessages, text)
            add(self, text, unpack(arg))
        end
        local probe = CreateFrame('Frame')
        probe:RegisterEvent('PLAYER_LEVEL_UP')
        probe:RegisterEvent('LEARNED_SPELL_IN_TAB')
        probe:RegisterEvent('CHAT_MSG_SYSTEM')
        probe:SetScript('OnEvent', function()
            __progressEvents[event] = {arg1,arg2,arg3,arg4,arg5,arg6,arg7,arg8,arg9}
        end)
    )");
    const auto& tree = ui.engine().widgets();
    const auto* lockedIcon = tree.findByName("SpellButton5IconTexture");
    REQUIRE(lockedIcon);
    CHECK(lockedIcon->color[0] == 0.4f);
    CHECK(lockedIcon->color[1] == 0.4f);
    CHECK(lockedIcon->color[2] == 0.4f);
    const auto* lockedButton = tree.findByName("SpellButton5");
    REQUIRE(lockedButton);
    CHECK_FALSE(lockedButton->dragLeft);
    CHECK_FALSE(lockedButton->dragRight);
    std::vector<std::string> commands;
    wowcore::setHostHandler(L, [&](lua_State* state, const std::string& cmd) {
        commands.push_back(cmd == "playSound" ? cmd + ":" + lua_tostring(state, 2) : cmd);
        return 0;
    });
    run(R"(
        __WB.cursor = nil
        CastSpell(__progressOffset+3, 'spell')
        PickupSpell(__progressOffset+3, 'spell')
        assert(not CursorHasSpell())
        SpellButton5:Click('LeftButton')
        __WB.actions[120] = {spell=16914}
        assert(not IsUsableAction(120))
        UseAction(120)
    )");
    CHECK(commands.empty());
    run(R"(
        __WBProgressionFixture(true)
        __WBLearnedSpell(5177)
        assert(__progressMessages[1] == 'You have learned a new spell: Wrath (Rank 2).')
        assert(__progressEvents.LEARNED_SPELL_IN_TAB[1] == 2)
        local chat = __progressEvents.CHAT_MSG_SYSTEM
        assert(table.getn(chat) == 9 and chat[1] == __progressMessages[1])
        for i=2,6 do assert(chat[i] == '') end
        assert(chat[7] == 0 and chat[8] == 0 and chat[9] == '')
        __WBLearnedSpell(1082)
        assert(__progressMessages[2] == 'You have learned a new ability: Claw (Rank 1).')
        assert(__progressEvents.LEARNED_SPELL_IN_TAB[1] == 3)
        __WBLearnedSpell(5177, 'recipe')
        assert(__progressMessages[3] == format(ERR_LEARN_RECIPE_S, 'Wrath (Rank 2)'))
        local _, _, offset, count = GetSpellTabInfo(2)
        assert(count == 4 and __WB.bookSpell(offset+3, 'spell') == 5177)
        assert(__WB.bookSpell(offset+4, 'spell') == 16914)
        CastSpell(offset+3, 'spell')
        PickupSpell(offset+3, 'spell')
        assert(CursorHasSpell())
        __WBApplyLive({player={level=7}})
        assert(UnitLevel('player') == 7 and PlayerLevelText:GetText() == '7')
        assert(not __progressEvents.PLAYER_LEVEL_UP)
        assert(__WBLevelUp(8, {health=10, magicka=20, stamina=5}))
        assert(PlayerLevelText:GetText() == '8')
        local e = __progressEvents.PLAYER_LEVEL_UP
        assert(table.getn(e) == 9 and e[1] == 8 and e[2] == 10 and e[3] == 20 and e[7] == 0)
        assert(e[4] == 0 and e[5] == 0 and e[6] == 0 and e[8] == 0 and e[9] == 0)
        assert(__progressMessages[4] == 'Congratulations, you have reached level 8!')
        assert(__progressMessages[5] == 'You have gained 10 hit points and 20 mana.')
        assert(__progressMessages[6] == 'Your Stamina increases by 5.')
        assert(not __WBLevelUp(8, {health=10}) and not __WBLevelUp(7))
        assert(table.getn(__progressMessages) == 6)
        assert(__WBLevelUp(9, {health=10}))
        assert(__progressMessages[8] == 'You have gained 10 hit points.')
        assert(__WBLevelUp(10, {}))
        assert(table.getn(__progressMessages)==9 and __progressMessages[9]==format(LEVEL_UP,10))
        assert(__WBLevelUp(11, {magicka=10}))
        assert(__progressMessages[11]==format(LEVEL_UP_STAT,MANA,10))
        assert(__WBLevelUp(12, {stamina=10}))
        assert(__progressMessages[13]==format(LEVEL_UP_STAT,SPELL_STAT2_NAME,10))
        __WBPlayerLevel(81) -- Skyrim overall level is independent of the skill curve's level-60 cap
        assert(UnitLevel('player') == 81 and PlayerLevelText:GetText() == '81')
    )");
    CHECK(commands == std::vector<std::string>{"castSpell", "cursor", "levelUp", "playSound:LEVELUPSOUND",
                                              "levelUp", "playSound:LEVELUPSOUND", "levelUp", "playSound:LEVELUPSOUND",
                                              "levelUp", "playSound:LEVELUPSOUND", "levelUp", "playSound:LEVELUPSOUND"});
    CHECK(lockedButton->dragLeft); // the same visible button is draggable once Wrath II is learned
}

TEST_CASE("host Skyrim shouts and powers share the book and drive real FrameXML cooldowns", "[client][api][skyrim-shouts]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
    auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/hud_states.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run(R"(
        __WBLogin()
        __abilityNow = 1000
        GetTime = function() return __abilityNow end
        __originalTabs = GetNumSpellTabs()
        assert(not __WB.skyrimShouts and not __WB.skyrimPowers and not __WB.skyrimSpells)
        assert(__WB.actions[1].spell == 9912 and GetActionCooldown(1) == 0)
        __abilityModels, __abilityEvents = {}, {}
        local publish = __WBModelState
        __WBModelState = function(f, file, anim, ms)
            __abilityModels[f:GetName()] = {anim, ms}
            publish(f, file, anim, ms)
        end
        local probe = CreateFrame('Frame')
        for _, name in ipairs({'SPELLS_CHANGED', 'ACTIONBAR_SLOT_CHANGED', 'ACTIONBAR_UPDATE_COOLDOWN', 'SPELL_UPDATE_COOLDOWN'}) do
            probe:RegisterEvent(name)
        end
        probe:SetScript('OnEvent', function() __abilityEvents[event] = true end)
        __WBSkyrimSpells({{id=-0x012FCD, name='Flames', cost=10, icon='Interface\\Icons\\WB_Skyrim_012FCD'}})
        __WBShoutFixture(true)
        assert(__abilityEvents.SPELLS_CHANGED and __abilityEvents.ACTIONBAR_SLOT_CHANGED)
        assert(GetNumSpellTabs() == __originalTabs + 1)
        local name, icon, offset, count = GetSpellTabInfo(GetNumSpellTabs())
        assert(name == 'Skyrim' and count == 4)
        __abilityOffset = offset
        assert(GetSpellName(offset+1, 'spell') == 'Battle Cry')
        assert(GetSpellName(offset+2, 'spell') == 'Flames')
        local shoutName, rank = GetSpellName(offset+3, 'spell')
        assert(shoutName == 'Unrelenting Force' and rank == '2 Words')
        assert(GetSpellName(offset+4, 'spell') == 'Whirlwind Sprint')
        assert(GetSpellTexture(offset+3, 'spell') == 'Interface\\Icons\\WB_Skyrim_013E07')
        assert(GetActionTexture(1) == GetSpellTexture(offset+3, 'spell'))
        assert(__WB.spellInfo(-0x013E07).wordSpell == -0x013F39)
        assert(__WB.spellInfo(-0x013E07).recovery == 20000)
        assert(__WB.spellInfo(-0x013E07).gcd == false)
        assert(GetActionCooldown(1) == 0 and GetActionCooldown(3) == 0)
        __WBGcd(1.5)
        assert(GetActionCooldown(1) == 0 and GetActionCooldown(3) == 0) -- no WoW GCD for either kind
        ToggleSpellBook(BOOKTYPE_SPELL)
        this = getglobal('SpellBookSkillLineTab'..GetNumSpellTabs())
        SpellBookSkillLineTab_OnClick()
        this = nil
    )");
    for (int i = 0; i < 3; ++i) textures.sync(ui.frame(1.0f / 60));
    run(R"(
        local offset = __abilityOffset
        UpdateSpells()
        assert(SpellButton5SpellName:GetText() == 'Unrelenting Force')
        assert(SpellButton5SubSpellName:GetText() == '2 Words')
        __WBSkyrimShoutUsed(-0x013E07)
        __WBSkyrimPowerUsed(-0x0E40C3)
        assert(__abilityEvents.ACTIONBAR_UPDATE_COOLDOWN and __abilityEvents.SPELL_UPDATE_COOLDOWN)
        local start, seconds, enabled = GetActionCooldown(1)
        assert(start == 1000 and seconds == 20 and enabled == 1)
        assert(GetActionCooldown(2) == start) -- shared shout recovery
        local bookStart, bookSeconds = GetSpellCooldown(offset+3, 'spell')
        assert(bookStart == start and bookSeconds == seconds)
        local powerStart, powerSeconds = GetActionCooldown(3)
        assert(powerStart == 1000 and powerSeconds == 86400)
        assert(ActionButton1Cooldown.start == 1000 and ActionButton1Cooldown.duration == 20)
        assert(SpellButton5Cooldown.start == 1000 and SpellButton5Cooldown.duration == 20)
        assert(ActionButton3Cooldown.duration == 86400 and SpellButton1Cooldown.duration == 86400)
        function __abilityTooltip()
            local lines = {}
            for i=1, GameTooltip:NumLines() do
                tinsert(lines, getglobal('GameTooltipTextLeft'..i):GetText() or '')
                local right = getglobal('GameTooltipTextRight'..i)
                if right:IsShown() then tinsert(lines, right:GetText() or '') end
            end
            return table.concat(lines, '\n')
        end
        GameTooltip:SetAction(1)
        local tip = __abilityTooltip()
        assert(strfind(tip, 'Words learned: Fus %(15 sec recovery%), Ro %(20 sec recovery%)'))
        assert(not strfind(tip, 'D4') and strfind(tip, '20 sec recovery') and strfind(tip, 'Cooldown remaining: 20 sec'))
        GameTooltip:SetSpell(offset+3, 'spell')
        assert(__abilityTooltip() == tip)
        GameTooltip:SetAction(3)
        tip = __abilityTooltip()
        assert(strfind(tip, 'Greater Power %- Once per Skyrim day') and strfind(tip, '1 day cooldown'))
        assert(strfind(tip, 'Cooldown remaining: 1 day'))
        __abilityNow = 1010
    )");
    for (int i = 0; i < 3; ++i) textures.sync(ui.frame(1.0f / 60));
    run(R"(
        assert(ActionButton1Cooldown:IsVisible() and SpellButton5Cooldown:IsVisible())
        assert(__abilityModels.ActionButton1Cooldown[1] == 0 and __abilityModels.ActionButton1Cooldown[2] == 500)
        assert(__abilityModels.SpellButton5Cooldown[1] == 0 and __abilityModels.SpellButton5Cooldown[2] == 500)
        local words = __WB.skyrimShouts[-0x013E07].words
        words[3].learned = true
        __WBSkyrimShoutWords(-0x013E07, words)
        assert(__WB.spellInfo(-0x013E07).wordCount == 2)
        GameTooltip:SetAction(1)
        assert(strfind(__abilityTooltip(), 'D4 %(locked%) %(45 sec recovery%)'))
        words[3].unlocked = true
        words[3].description = 'The full force of three words.'
        __WBSkyrimShoutWords(-0x013E07, words)
        assert(__WB.actions[1].spell == -0x013E07 and __WB.spellInfo(-0x013E07).wordSpell == -0x013F3A)
        assert(SpellButton5SubSpellName:GetText() == '3 Words')
        assert(__WB.spellInfo(-0x013E07).recovery == 45000 and ActionButton1Cooldown.duration == 20)
        GameTooltip:SetAction(1)
        assert(strfind(__abilityTooltip(), '45 sec recovery') and strfind(__abilityTooltip(), 'The full force of three words.'))
        __WBSkyrimSpells({}) -- each host list replaces only its own category
        assert(select(4, GetSpellTabInfo(GetNumSpellTabs())) == 3 and __WB.spellInfo(-0x013E07))
        __WBSkyrimPowers({{id=-123, name='Lesser test power', kind='lesser'}})
        assert(not __WB.cooldowns[-0x0E40C3])
        __WB.actions[3] = {spell=-123}
        __WBSkyrimPowerUsed(-123)
        local _, seconds = GetActionCooldown(3)
        assert(seconds == 0)
        GameTooltip:SetAction(3)
        assert(strfind(__abilityTooltip(), 'Lesser Power %- At will') and not strfind(__abilityTooltip(), 'cooldown'))
        __WBSkyrimPowerUsed(-123, 5) -- the host can report a lesser power's actual recovery
        local _, seconds = GetActionCooldown(3)
        assert(seconds == 5)
        __WBSkyrimShoutCooldown(7) -- resync remaining time after waiting or loading
        __WBSkyrimPowerCooldown(-123, 3)
        assert(ActionButton1Cooldown.duration == 7)
        __abilityNow = 1018
        local _, seconds = GetActionCooldown(1)
        assert(seconds == 0)
        local _, seconds = GetActionCooldown(3)
        assert(seconds == 0)
        __WBSkyrimShoutUsed(-0x013E07)
        local _, seconds = GetActionCooldown(1)
        assert(seconds == 45) -- newly unlocked highest word used by default
        __WBResetCombat()
        assert(not __WB.skyrimShoutCooldown and GetActionCooldown(1) == 0)
        assert(GetActionCooldown(3) == 0 and __WB.spellInfo(-0x013E07).wordCount == 3)
        __WBSkyrimShouts({{id=-321, name='Locked shout', words={{name='Word', learned=true, recovery=10}}}})
        assert(not __WB.spellInfo(-321) and not __WB.spellInfo(-0x013E07))
        local words = {{name='Word', learned=true, unlocked=true, recovery=10}}
        __WBSkyrimShoutWords(-321, words)
        assert(__WB.spellInfo(-321).wordCount == 1)
        __WBSkyrimShouts({})
        __WBSkyrimPowers({})
        assert(GetNumSpellTabs() == __originalTabs)
    )");
    std::string command;
    int id = 0;
    wowcore::setHostHandler(L, [&](lua_State* state, const std::string& cmd) {
        command = cmd;
        id = static_cast<int>(lua_tointeger(state, cmd == "useAction" ? 3 : 2));
        return 0;
    });
    run("__WBShoutFixture(true); UseAction(1)");
    CHECK(command == "useAction");
    CHECK(id == -0x013E07); // dispatch uses SHOU identity, not the selected word spell
    run("CastSpell(select(3, GetSpellTabInfo(GetNumSpellTabs())) + 2, 'spell')");
    CHECK(command == "castSpell");
    CHECK(id == -0x013E07);
    run("UseAction(3)");
    CHECK(command == "useAction");
    CHECK(id == -0x0E40C3);
    run(R"(
        -- Older action caches wrapped negative native IDs to unsigned numbers.
        __WB.actions[8]={spell=4294967296-77777}
        __WB.actions[9]={spell=6603}
        __WB.actions[10]={spell=4294967296-88888}
        __WB.actions[11]={spell=4294967296-99999}
        __WB.actions[12]={spell=4294967296-11111}
        __WBSkyrimSpells({{id=-77777,name='Recovered spell'},{id=6603-4294967296,name='High FormID collision'}})
        __WBSkyrimPowers({{id=-88888,name='Recovered power',kind='lesser'}})
        __WBSkyrimShouts({{id=-99999,name='Recovered shout',words={{name='Word',learned=true,unlocked=true,recovery=10}}}})
        assert(__WB.actions[8].spell==-77777 and __WB.actions[10].spell==-88888 and __WB.actions[11].spell==-99999)
        assert(__WB.actions[9].spell==6603, 'valid WoW actions must keep their IDs')
        assert(__WB.actions[12].spell==4294967296-11111, 'unidentified entries must remain untouched')
    )");
}

TEST_CASE("finisher failures use the client error frame font text and archived audio", "[client][api][forms]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 2560, 1440);
    std::string err;
    REQUIRE(ui.start(&err)); REQUIRE(ui.loadInterface(&err));
    std::string played;
    wowcore::setHostHandler(ui.lua(), [&](lua_State* L, const std::string& cmd) {
        if (cmd == "playSound") played = lua_tostring(L, 2);
        return 0;
    });
    REQUIRE(luaL_dostring(ui.lua(), "__WBLogin() UIErrorsFrame:Clear() __WBUiError('SPELL_FAILED_NO_COMBO_POINTS')") == 0);
    CHECK(played == "SpellFizzleNature");
    const auto* sound = db.sound(played);
    REQUIRE(sound); CHECK(sound->id == 1426); CHECK_FALSE(sound->files.empty());
    REQUIRE(mpq->read(sound->files.front()));
    const auto* frame = ui.engine().widgets().findByName("UIErrorsFrame");
    REQUIRE(frame); REQUIRE(frame->messages.size() == 1);
    CHECK(frame->messages.front().text == "That ability requires combo points");
    CHECK(frame->messages.front().color[0] == 1);
    CHECK(frame->messages.front().color[1] == .1f);
    CHECK(frame->messages.front().color[2] == .1f);
    CHECK(frame->fontHeight == 16); CHECK(frame->justifyH == "CENTER");
    CHECK(frame->fontFace == "Fonts\\FRIZQT__.TTF");
}

TEST_CASE("1.12 druid forms drive power, shapeshift, bonus actions and target combo frames", "[client][api][forms]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
    auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run("__WBLogin()");
    auto settle = [&] { for (int i = 0; i < 120; ++i) textures.sync(ui.frame(1.0f / 60)); };
    auto color = [&](float r, float g, float b) {
        const auto* bar = ui.engine().widgets().findByName("PlayerFrameManaBar");
        REQUIRE(bar);
        CHECK(bar->barColor[0] == r);
        CHECK(bar->barColor[1] == g);
        CHECK(bar->barColor[2] == b);
    };
    run(R"(
        assert(GetNumShapeshiftForms() == 0)
        __WBForm(0)
        assert(GetNumShapeshiftForms() == 4)
        local spells = {9634, 1066, 768, 783}
        for i, spell in ipairs(spells) do
            local icon, name, active, castable = GetShapeshiftFormInfo(i)
            assert(name == __WBSpellInfo(spell).name and icon == __WBSpellInfo(spell).icon)
            assert(active == nil and castable == 1)
        end
        assert(__WB.forms[1].spell == 9634) -- replaces 5487 in the original Bear slot
        assert(ShapeshiftBarFrame:IsVisible())
        __WBTestEvents = {}
        local probe = CreateFrame('Frame')
        for _, eventName in ipairs({'UNIT_DISPLAYPOWER', 'UNIT_ENERGY', 'UNIT_MAXENERGY', 'UNIT_RAGE',
                                   'UNIT_MAXRAGE', 'UNIT_MANA', 'UNIT_MAXMANA', 'PLAYER_COMBO_POINTS',
                                   'UPDATE_SHAPESHIFT_FORMS', 'UPDATE_BONUS_ACTIONBAR'}) do
            probe:RegisterEvent(eventName)
        end
        probe:SetScript('OnEvent', function() __WBTestEvents[event .. ':' .. tostring(arg1)] = true end)
        __WBTestMana = UnitMana('player')
        for i = 1, 12 do
            __WB.actions[72+i] = {spell = 9850} -- Claw
            __WB.actions[84+i] = {spell = 9867} -- Ravage while prowling
            __WB.actions[96+i] = {spell = 9881} -- Maul
        end
        __WBApplyLive({form = 3, player = {powerType = 3, power = 65, powerMax = 100},
                       target = {guid = 4242, name = 'Wolf', level = 60, health = 80, healthMax = 100,
                                 reaction = 2, attackable = true}, comboPoints = 3})
        assert(UnitPowerType('player') == 3 and UnitMana('player') == 65 and UnitManaMax('player') == 100)
        assert(PlayerFrameManaBar:GetValue() == 65)
        assert(ShapeshiftButton3:GetChecked() == 1 and not ShapeshiftButton1:GetChecked())
        assert(GetBonusBarOffset() == 1 and GetComboPoints() == 3)
        assert(__WBTestEvents['UNIT_DISPLAYPOWER:player'] and __WBTestEvents['UNIT_ENERGY:player'])
        assert(__WBTestEvents['UNIT_MAXENERGY:player'] and not __WBTestEvents['UNIT_MANA:player'])
        assert(__WBTestEvents['PLAYER_COMBO_POINTS:nil'] and __WBTestEvents['UPDATE_BONUS_ACTIONBAR:nil'])
    )");
    settle();
    color(1, 1, 0);
    run(R"(
        assert(BonusActionBarFrame:IsVisible() and ComboFrame:IsVisible())
        for i = 1, 12 do
            local button = getglobal('BonusActionButton'..i)
            assert(button:IsVisible() and ActionButton_GetPagedID(button) == 72+i)
            assert(getglobal(button:GetName()..'Icon'):GetTexture() == GetActionTexture(72+i))
        end
        for i = 1, 5 do assert(getglobal('ComboPoint'..i..'Highlight'):GetAlpha() == (i <= 3 and 1 or 0)) end
        -- Same-count points must be restored after changing target, including a
        -- switch that arrives in the same host snapshot as the new points.
        __WBApplyLive({target={guid=4243,name='Bear',health=100,healthMax=100},comboPoints=3})
        assert(GetComboPoints()==3 and ComboFrame:IsShown())
        __WBApplyLive({target=false,comboPoints=0})
        assert(GetComboPoints()==0 and not ComboFrame:IsShown())
        __WBApplyLive({target={guid=4242,name='Wolf',health=80,healthMax=100},comboPoints=3})
        assert(GetComboPoints()==3 and ComboFrame:IsShown())
        __WBTestEvents = {}
        __WBApplyLive({player = {power = 42}})
        assert(PlayerFrameManaBar:GetValue() == 42 and __WBTestEvents['UNIT_ENERGY:player'])
        assert(not __WBTestEvents['UNIT_DISPLAYPOWER:player'] and not __WBTestEvents['UNIT_MANA:player'])
        __WBApplyLive({prowling = true})
        assert(GetBonusBarOffset() == 2)
    )");
    settle();
    run(R"(
        for i = 1, 12 do assert(ActionButton_GetPagedID(getglobal('BonusActionButton'..i)) == 84+i) end
        __WBApplyLive({prowling = false})
        assert(GetBonusBarOffset() == 1)
        __WBTestEvents = {}
        __WBForm(1, 1)
        __WBApplyLive({player = {power = 32.5, powerType = 1, powerMax = 100}})
        assert(UnitPowerType('player') == 1 and UnitMana('player') == 32 and UnitManaMax('player') == 100)
        assert(PlayerFrameManaBar:GetValue() == 32 and ShapeshiftButton1:GetChecked() == 1)
        assert(not ShapeshiftButton3:GetChecked() and GetBonusBarOffset() == 3)
        assert(__WBTestEvents['UNIT_DISPLAYPOWER:player'] and __WBTestEvents['UNIT_RAGE:player'])
        assert(__WBTestEvents['UNIT_MAXRAGE:player'] and not __WBTestEvents['UNIT_MANA:player'])
        assert(IsUsableAction(97) == 1) -- Maul costs 15 client rage, stored as 150 in Spell.dbc
        assert(__WB.manaCost(__WB.units[1], 150, 1) == 15)
        __WBApplyLive({player = {power = 14}})
        local usable, noPower = IsUsableAction(97)
        assert(not usable and noPower == 1)
        __WBApplyLive({player = {power = 32.5}})
    )");
    settle();
    color(1, 0, 0);
    run(R"(
        for i = 1, 12 do
            local button = getglobal('BonusActionButton'..i)
            assert(button:IsVisible() and ActionButton_GetPagedID(button) == 96+i)
            assert(getglobal(button:GetName()..'Icon'):GetTexture() == GetActionTexture(96+i))
        end
        __WBApplyLive({target = {guid = 4243, name = 'Bear', level = 60, health = 100, healthMax = 100}})
        assert(GetComboPoints() == 0 and not ComboFrame:IsShown())
        __WBComboPoints(5)
        assert(GetComboPoints() == 5)
        __WBApplyLive({target = false})
        assert(GetComboPoints() == 0 and not ComboFrame:IsShown())
        __WBTestEvents = {}
        __WBForm(0, 0)
        assert(UnitPowerType('player') == 0 and UnitMana('player') == __WBTestMana and UnitManaMax('player') == 5284)
        assert(GetBonusBarOffset() == 0 and not ShapeshiftButton1:GetChecked())
        assert(__WBTestEvents['UNIT_DISPLAYPOWER:player'] and __WBTestEvents['UNIT_MANA:player'])
    )");
    settle();
    color(0, 0, 1);
    run(R"(
        assert(not BonusActionBarFrame:IsShown())
        for i = 1, 12 do assert(ActionButton_GetPagedID(getglobal('ActionButton'..i)) == i) end
        __WBDruidForms(5487)
        __WBForm(1)
        assert(__WB.forms[1].spell == 5487 and GetBonusBarOffset() == 3 and UnitPowerType('player') == 1)
        __WBForm(2)
        assert(UnitPowerType('player') == 0 and GetBonusBarOffset() == 0) -- Aquatic
        __WBForm(4)
        assert(UnitPowerType('player') == 0 and GetBonusBarOffset() == 0) -- Travel
        __WBForm(0)
        __WBApplyLive({target = 'player', player = {power = 0}})
        local _, _, _, castable = GetShapeshiftFormInfo(3)
        assert(not castable)
        __WBForm(3)
        __WBApplyLive({player = {power = 40}})
        assert(TargetFrameManaBar:GetValue() == 40 and __WBTestEvents['UNIT_ENERGY:target'])
        __WBComboPoints(3)
        __WBResetCombat()
        assert(UnitPowerType('player') == 0 and GetComboPoints() == 0 and GetBonusBarOffset() == 0)
    )");
    std::string command;
    int index = -1;
    wowcore::setHostHandler(L, [&](lua_State* state, const std::string& cmd) {
        command = cmd;
        index = static_cast<int>(lua_tointeger(state, 2));
        return 0;
    });
    run(R"(
        __WBFormSpell(0,0,false) __WBDruidForms(5487,0)
        assert(GetNumShapeshiftForms()==0)
        __WBFormSpell(768,3,true)
        assert(GetNumShapeshiftForms()==0 and __WB.formIndex==0 and __WB.activeFormSpell==768)
        assert(UnitPowerType('player')==3 and GetBonusBarOffset()==2)
        assert(not __WB.spellFormError(__WB.spellInfo(5215)))
        __WBApplyLive({prowling=false}) assert(GetBonusBarOffset()==1 and __WB.activeFormSpell==768)
        __WBFormSpell(5487,1,false)
        assert(UnitPowerType('player')==1 and GetBonusBarOffset()==3)
        __WBDruidForms(9634,4)
        assert(__WB.formIndex==1 and __WB.forms[1].active and __WB.activeFormSpell==5487)
        __WBFormSpell(0,0,false)
        assert(GetBonusBarOffset()==0 and UnitPowerType('player')==0 and not __WB.forms[1].active)
    )");
    run("CastShapeshiftForm(3)");
    CHECK(command == "shapeshift");
    CHECK(index == 3);
    run("CastShapeshiftForm(0)");
    CHECK(index == 0);
}

TEST_CASE("1.12 spells drag through mouse events between the book and paged bars", "[client][api][drag]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    const int height = GENERATE(720, 1080);
    wowcore::UiSession ui(mpq, db, textures, height * 16 / 9, height);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
    auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO((result ? lua_tostring(L, -1) : ""));
        INFO(ui.engine().lastError());
        REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run(R"(
        __WBLogin()
        __WBDruidForms()
        __WBDefaultFormActions()
        __WBSkyrimSpells({{id=-123, name='Flames', icon='Interface\\Icons\\Spell_Fire_Fire', cost=5}})
        __WBSkyrimShouts({{id=-456, name='Unrelenting Force', icon='Interface\\Icons\\Spell_Nature_ThunderClap',
            words={{name='Fus', spell=-457, learned=true, unlocked=true, recovery=15}}}})
        __WBSkyrimPowers({{id=-789, kind='greater', name='Voice of the Emperor', icon='Interface\\Icons\\Spell_Holy_MindVision'}})
        __starts, __stops, __drops, __clicks = 0, 0, 0, 0
        for _, name in ipairs({'ActionButton1','ActionButton2','ActionButton3','BonusActionButton1',
                               'BonusActionButton2','BonusActionButton12','MultiBarBottomLeftButton12','SpellButton1','SpellButton3','SpellButton5'}) do
            local f = getglobal(name)
            f:HookScript('OnDragStart', function() __starts = __starts + 1 end)
            f:HookScript('OnDragStop', function() __stops = __stops + 1 end)
            f:HookScript('OnReceiveDrag', function() __drops = __drops + 1 end)
            f:HookScript('OnClick', function() __clicks = __clicks + 1 end)
        end
    )");
    auto frames = [&]() { for (int i = 0; i < 3; ++i) textures.sync(ui.frame(1.0f / 60)); };
    frames();
    auto centre = [&](const char* name) {
        const auto& tree = ui.engine().widgets();
        const auto* w = tree.findByName(name);
        REQUIRE(w != nullptr);
        const float scale = tree.uiScale();
        return std::pair{(w->left + w->rectW / 2) * scale,
                         ui.height() - (w->bottom + w->rectH / 2) * scale};
    };
    auto mouse = [&](float x, float y, bool down, bool right = false) {
        ui.pointer(x, y);
        ui.engine().dispatchMouse(x, y, ui.height(), {down && !right, down && right, false});
        frames();
    };
    auto begin = [&](const char* name, bool right = false) {
        const auto [x, y] = centre(name);
        const auto& tree = ui.engine().widgets();
        const auto hit = tree.hitTest(x / tree.uiScale(), (ui.height() - y) / tree.uiScale());
        INFO("drag source " << name << " hit " << (tree.get(hit) ? tree.get(hit)->name : "nothing"));
        REQUIRE(hit == tree.findByName(name)->id);
        mouse(x, y, false, right);
        mouse(x, y, true, right);
        mouse(x + 5, y, true, right);
    };
    auto drop = [&](const char* name, bool right = false) {
        const auto [x, y] = centre(name);
        mouse(x, y, true, right);
        mouse(x, y, false, right);
    };
    auto click = [&](const char* name) {
        const auto [x, y] = centre(name);
        mouse(x, y, false);
        mouse(x, y, true);
        mouse(x, y, false);
    };

    SECTION("a drag interrupted by a native menu cannot turn the next click into a pickup") {
        run("__other=__WB.actions[2].spell");
        begin("ActionButton1");
        REQUIRE(ui.engine().holdsMousePress());
        // The native menu owns the release; no pointer dispatch occurs meanwhile.
        ui.engine().cancelMousePress();
        REQUIRE_FALSE(ui.engine().holdsMousePress());
        run("ClearCursor()");
        const auto [x,y]=centre("ActionButton2");
        mouse(x,y,true);
        mouse(x,y,false);
        run("assert(__clicks==1 and not CursorHasSpell() and __WB.actions[2].spell==__other)");
    }
    SECTION("stationary and small-motion presses remain clicks") {
        click("ActionButton1");
        const auto [x, y] = centre("ActionButton1");
        mouse(x, y, true);
        mouse(x + 3, y, true);
        mouse(x + 3, y, false);
        run("assert(__clicks == 2 and __starts == 0 and __stops == 0 and not CursorHasSpell() and __WB.actions[1])");
    }
    SECTION("threshold, slot refresh, occupied drop and click placement") {
        run("__source = __WB.actions[1].spell; __other = __WB.actions[2].spell");
        const auto [x, y] = centre("ActionButton1");
        mouse(x, y, false);
        mouse(x, y, true);
        mouse(x + 3, y, true);
        run("assert(__starts == 0 and __WB.actions[1].spell == __source and not CursorHasSpell())");
        mouse(x + 4.1f, y, true);
        run(R"(
            assert(__starts == 1 and __clicks == 0 and not __WB.actions[1])
            assert(__WB.cursor.spell == __source and __WB.cursorTexture() == __WB.spellInfo(__source).icon)
            assert(not ActionButton1Icon:IsShown() and ActionButton12:IsShown())
        )");
        drop("ActionButton2");
        run(R"(
            assert(__WB.actions[2].spell == __source and __WB.cursor.spell == __other)
            assert(ActionButton2Icon:GetTexture() == __WB.spellInfo(__source).icon)
            assert(__starts == 1 and __stops == 1 and __drops == 1 and __clicks == 0)
        )");
        click("ActionButton1"); // the displaced spell stays on the cursor, as in 1.12
        run("assert(__WB.actions[1].spell == __other and not CursorHasSpell() and not ActionButton12:IsShown())");
    }
    SECTION("empty target, return to source, outside drop and release motion") {
        run("__source = __WB.actions[1].spell; __WB.actions[3] = nil; __WBFire('ACTIONBAR_SLOT_CHANGED', 3)");
        frames();
        begin("ActionButton1");
        drop("ActionButton3");
        run("assert(not __WB.actions[1] and __WB.actions[3].spell == __source and not CursorHasSpell())");
        begin("ActionButton3");
        drop("ActionButton3");
        run("assert(__WB.actions[3].spell == __source and not CursorHasSpell())");
        const auto [x, y] = centre("ActionButton3");
        mouse(x, y, true);
        mouse(900, 300, false); // a coalesced move/release still crosses the threshold
        run("assert(not __WB.actions[3] and not CursorHasSpell() and __clicks == 0 and __starts == __stops)");
    }
    SECTION("locked bars and Shift-drag") {
        run("LOCK_ACTIONBAR = '1'; __source = __WB.actions[1].spell; __other = __WB.actions[2].spell");
        begin("ActionButton1");
        drop("ActionButton2");
        run("assert(__WB.actions[1].spell == __source and __WB.actions[2].spell == __other and not CursorHasSpell())");
        run("__WB.modifiers = {shift=1}");
        begin("ActionButton1");
        drop("ActionButton2");
        run("assert(LOCK_ACTIONBAR == '1' and not __WB.actions[1] and __WB.actions[2].spell == __source and __WB.cursor.spell == __other)");
        click("ActionButton1");
        run("ClearCursor(); __WB.modifiers = {}; LOCK_ACTIONBAR = '0'");
    }
    SECTION("world clicks release swapped and book cursors without another drag") {
        for (bool right : {false, true}) {
            run("__source = __WB.actions[1].spell; __other = __WB.actions[2].spell");
            begin("ActionButton1");
            drop("ActionButton2");
            run("assert(__WB.cursor.spell == __other and not __WB.actions[1])");
            mouse(900, 300, false, right);
            mouse(900, 300, true, right);
            mouse(900, 300, false, right);
            run("assert(not CursorHasSpell() and not __WB.actions[1] and __WB.actions[2].spell == __source)");
            run("__WB.actions[1]={spell=__source}; __WB.actions[2]={spell=__other}; __WBFire('ACTIONBAR_SLOT_CHANGED',0)");
            frames();
        }
        run("ShowUIPanel(SpellBookFrame); __bookSwap=__WB.actions[2].spell");
        frames();
        click("SpellBookSkillLineTab1");
        begin("SpellButton1"); // the reported case: book Attack replaces bar Wrath
        drop("ActionButton2");
        run("assert(__WB.actions[2].spell==6603 and __WB.cursor.spell==__bookSwap and __WB.bookSpell(1,'spell')==6603)");
        click("SpellBookSkillLineTab1"); // a frame click keeps the displaced spell
        run("assert(__WB.cursor.spell==__bookSwap)");
        mouse(900, 300, true);
        mouse(900, 300, false);
        run("assert(not CursorHasSpell() and __WB.actions[2].spell==6603 and __WB.bookSpell(1,'spell')==6603)");
    }
    SECTION("General Attack, every class tab and Skyrim spells shouts and powers") {
        run(R"(
            ShowUIPanel(SpellBookFrame)
            assert(GetSpellTabInfo(1) == 'General' and __WB.bookSpell(1, 'spell') == 6603)
            assert(GetSpellName(1, 'spell') == 'Attack')
            local n = GetNumSpellTabs()
            local name, _, _, count = GetSpellTabInfo(n)
            assert(name == 'Skyrim' and count == 3)
            __WB.actions[12] = nil
            __WBFire('ACTIONBAR_SLOT_CHANGED', 12)
        )");
        frames();
        for (int tab = 1; tab <= 5; ++tab) {
            const auto tabName = "SpellBookSkillLineTab" + std::to_string(tab);
            click(tabName.c_str());
            frames();
            const int count = tab == 5 ? 3 : 1;
            for (int spell = 1; spell <= count; ++spell) {
                const auto expected = "__expected = __WB.bookSpell(SpellBook_GetSpellID(" + std::to_string(spell) + "), 'spell')";
                run(expected.c_str());
                // 1.12 lays its buttons across rows: names 1,3,5 carry book IDs 1,2,3.
                const auto name = "SpellButton" + std::to_string(spell * 2 - 1);
                begin(name.c_str());
                run("assert(__WB.cursor and __WB.cursor.spell == __expected and ActionButton12:IsShown())");
                drop("ActionButton12");
                run("assert(__WB.actions[12].spell == __expected and not CursorHasSpell())");
                run("__WB.actions[12] = nil; __WBFire('ACTIONBAR_SLOT_CHANGED', 12)");
                frames();
            }
            if (tab == 2) {
                click("SpellBookNextPageButton");
                run("assert(SpellBook_GetCurrentPage() == 2); __expected = __WB.bookSpell(SpellBook_GetSpellID(1), 'spell')");
                begin("SpellButton1");
                drop("ActionButton12");
                run("assert(__WB.actions[12].spell == __expected and not CursorHasSpell())");
                run("__WB.actions[12] = nil; __WBFire('ACTIONBAR_SLOT_CHANGED', 12)");
                frames();
            }
        }
    }
    SECTION("drags to and from Cat Prowl and Bear pages") {
        run("SHOW_MULTI_ACTIONBAR_1 = 1; MultiActionBar_Update(); UIParent_ManageFramePositions(); "
            "__WB.actions[72] = {spell=9912}; __WBFire('ACTIONBAR_SLOT_CHANGED', 72)");
        for (const char* form : {"__WBForm(3)", "__WBForm(3, nil, true)", "__WBForm(1)"}) {
            run(form);
            frames();
            // Wait for the bonus bar's slide animation before its icons update.
            for (int i = 0; i < 60; ++i) textures.sync(ui.frame(1.0f / 60));
            run("__first = GetBonusBarOffset()*12 + 61; __source = __WB.actions[__first].spell");
            begin("BonusActionButton1");
            drop("MultiBarBottomLeftButton12");
            run("assert(not __WB.actions[__first] and __WB.actions[72].spell == __source and __WB.cursor.spell == 9912)");
            run("ClearCursor()");
            begin("MultiBarBottomLeftButton12");
            drop("BonusActionButton1");
            run("assert(__WB.actions[__first].spell == __source and not __WB.actions[72] and not CursorHasSpell())");
            run("__WB.actions[72] = {spell=9912}; __WBFire('ACTIONBAR_SLOT_CHANGED', 72)");
        }
    }
    SECTION("right button registered for dragging") {
        run("__source = __WB.actions[1].spell; __WB.actions[12] = nil; __WBFire('ACTIONBAR_SLOT_CHANGED', 12)");
        begin("ActionButton1", true);
        drop("ActionButton12", true);
        run("assert(__WB.actions[12].spell == __source and not __WB.actions[1] and not CursorHasSpell() and __clicks == 0)");
    }
    CHECK(ui.engine().lastError().empty());
    CHECK_FALSE(ui.engine().holdsMousePress());
}

TEST_CASE("1.12 tooltip FadeOut holds, fades its alpha and hides; fresh owners cancel it", "[client][tooltip][fade]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
    auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run("__WBLogin(); __WBSkyrimMenus(); SHOW_NEWBIE_TIPS = '0'");
    textures.sync(ui.frame(0));
    auto* tip = ui.engine().widgets().findByName("GameTooltip");
    REQUIRE(tip != nullptr);
    // The real unit frame's OnLeave calls FadeOut; explicit Hide still hides immediately.
    run("this = PlayerFrame; UnitFrame_OnEnter(); UnitFrame_OnLeave(); this = nil");
    CHECK(tip->shown);
    CHECK(tip->alpha == 1);
    textures.sync(ui.frame(0)); // show the timer's driver without consuming elapsed time
    textures.sync(ui.frame(0.5f));
    CHECK(tip->shown);
    CHECK(tip->alpha == 1);
    textures.sync(ui.frame(0.75f));
    CHECK(tip->shown);
    CHECK(tip->alpha == Catch::Approx(191.0f / 255));
    textures.sync(ui.frame(0.5f));
    CHECK(tip->shown);
    CHECK(tip->alpha == Catch::Approx(63.0f / 255));
    textures.sync(ui.frame(0.25f));
    CHECK_FALSE(tip->shown);
    CHECK(tip->alpha == 1);
    run("assert(not GameTooltip:GetOwner())");

    run("GameTooltip:SetOwner(PlayerFrame, 'ANCHOR_RIGHT'); GameTooltip:SetText('old'); GameTooltip:FadeOut()");
    textures.sync(ui.frame(0));
    textures.sync(ui.frame(1.5f));
    CHECK(tip->alpha < 1);
    run("GameTooltip:SetOwner(TargetFrame, 'ANCHOR_LEFT'); GameTooltip:SetText('new')");
    textures.sync(ui.frame(1));
    CHECK(tip->shown);
    CHECK(tip->alpha == 1);
    run("assert(GameTooltipTextLeft1:GetText() == 'new'); GameTooltip:FadeOut(); GameTooltip:Hide()");
    CHECK_FALSE(tip->shown);
    textures.sync(ui.frame(3));
    CHECK_FALSE(tip->shown);
    run("GameTooltip:SetOwner(PlayerFrame, 'ANCHOR_CURSOR'); GameTooltip:SetText('cursor'); GameTooltip:FadeOut()");
    CHECK_FALSE(tip->shown);

    // Changing a harvested object's action must rebuild its tooltip even if
    // the display name stays unchanged; interface-owned tooltips remain intact.
    run(R"(
      __WBApplyLive({mouseover={object='Mountain Flower',action='Right-click: Harvest Blue Mountain Flower'}})
      assert(GameTooltipTextLeft1:GetText()=='Mountain Flower')
      assert(GameTooltipTextLeft2:GetText()=='Right-click: Harvest Blue Mountain Flower')
      __WBApplyLive({mouseover={object='Mountain Flower'}})
      assert(GameTooltip:NumLines()==1)
      __WBApplyLive({mouseover={object='Chest',action='Right-click: Open',locked=true,steal=true}})
      assert(GameTooltipTextLeft2:GetText()=='Right-click: Open' and GameTooltipTextLeft3:GetText()==LOCKED)
      GameTooltip:SetOwner(PlayerFrame,'ANCHOR_RIGHT') GameTooltip:SetText('UI owner')
      __WBApplyLive({mouseover=false})
      assert(GameTooltip:IsOwned(PlayerFrame) and GameTooltipTextLeft1:GetText()=='UI owner')
    )");
    // The host's world hover uses the same timer when its owner is left.
    run("__WBApplyLive({mouseover={object='Chest'}}); __WBApplyLive({mouseover=false})");
    CHECK(tip->shown);
    textures.sync(ui.frame(0));
    textures.sync(ui.frame(1.5f));
    CHECK(tip->alpha == Catch::Approx(127.0f / 255));
    textures.sync(ui.frame(0.5f));
    CHECK_FALSE(tip->shown);
}

TEST_CASE("user-placed frames survive a fresh interface login through layout-cache.txt", "[client][layout]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    std::string err;
    auto ui = std::make_unique<wowcore::UiSession>(mpq, db, textures, 1280, 720);
    auto run = [&](const char* script) {
        const int result = luaL_dostring(ui->lua(), script);
        INFO(script);
        INFO((result ? lua_tostring(ui->lua(), -1) : ""));
        REQUIRE(result == 0);
    };
    auto start = [&] {
        REQUIRE(ui->start(&err));
        REQUIRE(luaL_dofile(ui->lua(), (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
        run("__WBFixture('idle')");
        REQUIRE(ui->loadInterface(&err));
    };
    start();
    REQUIRE(luaL_dofile(ui->lua(), (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/hud_states.lua").c_str()) == 0);
    run(R"(
        __WBLogin()
        WBUnlockedFrames = 'PlayerFrame TargetFrame'
        -- Configure through the user's lock state; drive the same native movement API as OnDragStart.
        __WBSkyrimMenus()
        __WBProgressionFixture()
        __WBDruidForms(5487, 1)
        __WB.actions[1] = {spell=5176}
        __WB.units[__WB.tokens.player].health = 1234
        PlayerFrame:SetMovable(true)
        TargetFrame:SetMovable(true)
        local f = CreateFrame('Frame', 'LayoutScaledFrame', UIParent)
        f:SetWidth(80); f:SetHeight(40); f:SetScale(1.25)
        f:SetPoint('TOPRIGHT', UIParent, 'TOPRIGHT', -300, -200)
        f:SetUserPlaced(true)
        local ignored = CreateFrame('Frame', 'LayoutDefaultFrame', UIParent)
        ignored:SetPoint('CENTER'); ignored:SetWidth(40); ignored:SetHeight(40)
        CreateFrame('Frame', nil, UIParent):SetUserPlaced(true)
    )");
    textures.sync(ui->frame(0));
    auto& tree = ui->engine().widgets();
    run("PlayerFrame:StartMoving()");
    const auto playerId = tree.findByName("PlayerFrame")->id;
    tree.nudge(playerId, 177, -113);
    run("PlayerFrame:StopMovingOrSizing(); TargetFrame:StartMoving()");
    tree.nudge(tree.findByName("TargetFrame")->id, 91, -187);
    run("TargetFrame:StopMovingOrSizing(); assert(PlayerFrame:IsUserPlaced() and TargetFrame:IsUserPlaced())");
    textures.sync(ui->frame(0));
    const float left = tree.get(playerId)->left, bottom = tree.get(playerId)->bottom;
    const float targetLeft = tree.findByName("TargetFrame")->left, targetBottom = tree.findByName("TargetFrame")->bottom;
    const float scaledLeft = tree.findByName("LayoutScaledFrame")->left, scaledBottom = tree.findByName("LayoutScaledFrame")->bottom;
    run("__WBFire('PLAYER_LOGOUT'); return __WBLayoutCacheText()");
    const std::string cache = lua_tostring(ui->lua(), -1);
    lua_pop(ui->lua(), 1);
    CHECK(cache.find("Frame: PlayerFrame\n") != std::string::npos);
    CHECK(cache.find("Frame: TargetFrame\n") != std::string::npos);
    CHECK(cache.find("Frame: LayoutDefaultFrame\n") == std::string::npos);
    run("return __WBReloadStateText()");
    const std::string reloadState = lua_tostring(ui->lua(), -1);
    lua_pop(ui->lua(), 1);
    run("PlayerFrame:SetUserPlaced(false); assert(not PlayerFrame:IsUserPlaced()); assert(not string.find(__WBLayoutCacheText(), 'Frame: PlayerFrame'))");
    // Destroy the Lua state, as ReloadUI/new sessions do, rather than restoring over the old frame.
    ui.reset();
    ui = std::make_unique<wowcore::UiSession>(mpq, db, textures, 1280, 720);
    start();
    run(reloadState.c_str());
    lua_getglobal(ui->lua(), "__WBLoadLayoutCache");
    const std::string pending = cache + "Frame: BattlefieldMinimapTab\nFrameLevel: 1\nX: 250\nY: 480\nW: 128\nH: 32\n";
    lua_pushlstring(ui->lua(), pending.data(), pending.size());
    REQUIRE(lua_pcall(ui->lua(), 1, 0, 0) == 0);
    run(R"(
        local f = CreateFrame('Frame', 'LayoutScaledFrame', UIParent)
        f:SetScale(1.25); f:SetWidth(1); f:SetHeight(1); f:SetPoint('CENTER')
        __WBLogin(); __WBSkyrimMenus()
        assert(PlayerFrame:IsUserPlaced() and TargetFrame:IsUserPlaced() and f:IsUserPlaced())
        UIParent_ManageFramePositions() -- the interface must respect the restored user-placed flag
        assert(f:GetWidth() == 80 and f:GetHeight() == 40)
        assert(__WB.actions[1].spell == 5176 and UnitHealth('player') == 1234)
        assert(GetNumShapeshiftForms() == 1 and __WB.forms[1].spell == 5487)
        assert(__WB.classRows and __WB.unlearned[5177])
        assert(string.find(__WBLayoutCacheText(), 'Frame: BattlefieldMinimapTab'))
    )");
    textures.sync(ui->frame(0));
    CHECK(ui->engine().widgets().findByName("PlayerFrame")->left == Catch::Approx(left));
    CHECK(ui->engine().widgets().findByName("PlayerFrame")->bottom == Catch::Approx(bottom));
    CHECK(ui->engine().widgets().findByName("TargetFrame")->left == Catch::Approx(targetLeft));
    CHECK(ui->engine().widgets().findByName("TargetFrame")->bottom == Catch::Approx(targetBottom));
    CHECK(ui->engine().widgets().findByName("LayoutScaledFrame")->left == Catch::Approx(scaledLeft));
    CHECK(ui->engine().widgets().findByName("LayoutScaledFrame")->bottom == Catch::Approx(scaledBottom));
    REQUIRE(ui->loadAddOn("Blizzard_BattlefieldMinimap", &err));
    REQUIRE(ui->engine().widgets().findByName("BattlefieldMinimapTab") != nullptr);
    run(R"(
        assert(BattlefieldMinimapTab:IsUserPlaced())
        local _, _, _, x, y = BattlefieldMinimapTab:GetPoint(1)
        assert(x == 250 and y == 480)
    )");
    run(R"(
        __WBLoadLayoutCache('Frame: PlayerFrame\nFrameLevel: 1\nX: nan\nY: 10\nW: 100\nH: 100\n')
        __WBRestoreLayout()
        assert(PlayerFrame:IsUserPlaced())
        PlayerFrame:SetUserPlaced(false)
        assert(not string.find(__WBLayoutCacheText(), 'Frame: PlayerFrame'))
    )");
}

TEST_CASE("1.12 docked chat tabs fade in and select their windows", "[client][chat]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    const int height = GENERATE(720, 1440);
    wowcore::UiSession ui(mpq, db, textures, height * 16 / 9, height);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    REQUIRE(luaL_dostring(L, "__WBFixture('idle')") == 0);
    REQUIRE(ui.loadInterface(&err));
    REQUIRE(luaL_dostring(L, "__WBLogin(); __WBSkyrimMenus()") == 0);
    auto frames = [&](int count) {
        for (int i = 0; i < count; ++i) textures.sync(ui.frame(1.0f / 60));
    };
    frames(120);
    ui.pointer(200 * height / 720.f, 560 * height / 720.f);
    frames(120);
    auto check = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    check(R"(
        assert(MouseIsOver(ChatFrame1))
        assert(ChatFrame1Tab:IsVisible() and ChatFrame1Tab:GetAlpha() > 0)
        assert(ChatFrame2Tab:IsVisible() and ChatFrame2Tab:GetAlpha() > 0)
        assert(SELECTED_DOCK_FRAME == ChatFrame1)
        __tabClicks, __tabDrags, __tabStops = 0, 0, 0
        ChatFrame2Tab:HookScript('OnClick', function() __tabClicks = __tabClicks + 1 end)
        ChatFrame2Tab:HookScript('OnDragStart', function() __tabDrags = __tabDrags + 1 end)
        ChatFrame2Tab:HookScript('OnDragStop', function() __tabStops = __tabStops + 1 end)
    )");
    auto tabCentre = [&](const char* name) {
        const auto& tree = ui.engine().widgets();
        const auto* tab = tree.findByName(name);
        REQUIRE(tab != nullptr);
        const float scale = tree.uiScale();
        return std::pair{(tab->left + tab->rectW / 2) * scale,
                         ui.height() - (tab->bottom + tab->rectH / 2) * scale};
    };
    auto mouse = [&](float x, float y, bool down) {
        ui.pointer(x, y);
        ui.engine().dispatchMouse(x, y, ui.height(), {down, false, false});
        frames(2);
    };
    auto clickTab = [&](const char* name) {
        const auto [x, y] = tabCentre(name);
        mouse(x, y, false);
        frames(60); // match the in-game probe's hover before each tab click
        mouse(x, y, true);
        mouse(x, y, true);   // host dispatches the held state each frame
        mouse(x, y, false);
    };
    clickTab("ChatFrame2Tab");
    check(R"(
        assert(SELECTED_DOCK_FRAME == ChatFrame2)
        assert(ChatFrame2:IsShown() and not ChatFrame1:IsShown())
        assert(__tabClicks == 1 and __tabDrags == 0)
    )");
    clickTab("ChatFrame1Tab");
    check(R"(
        assert(SELECTED_DOCK_FRAME == ChatFrame1)
        assert(ChatFrame1:IsShown() and not ChatFrame2:IsShown())
    )");
    // Drag registration does not consume a stationary click. Crossing the
    // threshold while held does run the real FrameXML drag and suppress OnClick.
    const auto [x, y] = tabCentre("ChatFrame2Tab");
    mouse(x, y, false);
    mouse(x, y, true);
    mouse(x + 1, y, true);
    check("assert(__tabDrags == 0 and __tabClicks == 1)");
    mouse(x + 100, y - 100, true);
    check(R"(
        assert(__tabDrags == 1 and __tabClicks == 1)
        assert(MOVING_CHATFRAME == ChatFrame2 and not ChatFrame2.isDocked)
        assert(SELECTED_DOCK_FRAME == ChatFrame1)
    )");
    mouse(x + 100, y - 100, false);
    check(R"(
        assert(__tabStops == 1 and __tabClicks == 1)
        assert(MOVING_CHATFRAME == nil and SELECTED_DOCK_FRAME == ChatFrame1)
    )");
    CHECK(ui.engine().widgets().movingWidget() == 0);
}

TEST_CASE("1.12 interface loads; the API and tooltips answer as the reference client did", "[client][api]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    setenv("WOWEE_LUA_API_FALLBACK", "0", 1);    // 1.12 has no fallback: a missing function must fail here

    const wowcore::Mpq mpq(data);
    const wowcore::ClientDb db(mpq);
    REQUIRE(db.ok());
    wowee::addons::LuaEngine engine;
    REQUIRE(engine.initialize());
    std::string err;
    REQUIRE(wowcore::installWowApi(engine, db, &err));
    lua_State* L = engine.getState();
    wowee::ui::setInterfacePixelScale(720.0f / 768.0f);   // the reference client: 1280x720, default interface scale
    const std::string root = WOWBRIDGE_ROOT;
    REQUIRE(luaL_dofile(L, (root + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    REQUIRE(luaL_dostring(L, "__WBFixture('idle')") == 0);

    const auto load = wowcore::loadFrameXml(engine, mpq);
    for (const auto& [file, why] : load.failures) UNSCOPED_INFO(file << ": " << why);
    CHECK(load.failures.empty());
    CHECK(load.luaFiles + load.xmlFiles == 90);
    // The client's login burst, as the DLL raises it after loading the interface.
    const bool login = luaL_dostring(L, "__WBLogin()") == 0;
    INFO((login ? "" : lua_tostring(L, -1)));
    CHECK(login);

    const auto r = wowcore::checkApiReference(L, root + "/evidence/apiref/WBOracle_api.lua", {"idle", "target", "self"});
    INFO(r.error << r.summary);
    REQUIRE(r.error.empty());
    CHECK(r.probes > 10000);
    CHECK(r.matches == r.probes);

    const auto tips = wowcore::checkTooltipReference(L, {"idle", "target", "self"});
    INFO(tips.error << tips.summary << "\n" << tips.details.substr(0, 2000));
    REQUIRE(tips.error.empty());
    CHECK(tips.tooltips > 400);
    CHECK(tips.sameLines == tips.tooltips);

    // Live state from the host: a new target shows TargetFrame, its health is in percent, losing it hides it.
    REQUIRE(luaL_dostring(L, "__WBFixture('idle')") == 0);
    const char* live = R"(
        __WBApplyLive({ player = { health = 1500, healthMax = 3213 },
                        target = { guid = 4242, name = "Bandit", level = 9, health = 30, healthMax = 120, reaction = 2,
                                    attackable = true, creatureType = "Humanoid" },
                        world = { zone = "Whiterun Hold", subzone = "Riverwood", hour = 14, minute = 5 } })
        local shown = TargetFrame:IsShown()
        local a = { UnitName("target"), UnitHealth("target"), UnitHealth("player"), GetSubZoneText(), shown }
        __WBApplyLive({ target = false })
        a[6] = TargetFrame:IsShown()
        return a[1] .. "|" .. a[2] .. "|" .. a[3] .. "|" .. a[4] .. "|" .. tostring(a[5]) .. "|" .. tostring(a[6]))";
    REQUIRE(luaL_dostring(L, live) == 0);
    CHECK(std::string(lua_tostring(L, -1)) == "Bandit|25|1500|Riverwood|1|nil");
    lua_pop(L, 1);
    engine.shutdown();
}

TEST_CASE("Skyrim spell tooltips convert health amounts and preserve the client elsewhere", "[client][tooltip]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    const wowcore::Mpq mpq(data);
    const wowcore::ClientDb db(mpq);
    wowee::addons::LuaEngine engine;
    REQUIRE(engine.initialize());
    std::string err;
    REQUIRE(wowcore::installWowApi(engine, db, &err));
    lua_State* L = engine.getState();
    auto run = [&](const std::string& script) {
        const int result = luaL_dostring(L, script.c_str());
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(wowcore::loadFrameXml(engine, mpq).failures.empty());
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/skyrim_health_tooltips.lua").c_str()) == 0);
    lua_setglobal(L, "__nativeHealthCases");
    run(R"(
        __WBLogin()
        function testDescription(slot, book)
            if book then
                local id = __WB.actions[slot].spell
                for index, spell in ipairs(__WB.spellbook) do
                    if spell == id then GameTooltip:SetSpell(index, 'spell') break end
                end
            else GameTooltip:SetAction(slot) end
            return getglobal('GameTooltipTextLeft' .. GameTooltip:NumLines()):GetText()
        end
    )");
    struct Example {
        int slot, low, high, skill, ticks;   // ticks: a total over time is converted tick by tick, as the service does
        const char* prefix;
        const char* suffix;
    };
    // Today's client text at the fixture's level, including Rejuvenation's duration modifier.
    const Example examples[] = {
        {1, 248, 277, 30, 1, "Causes ", " Nature damage to the target."},
        {4, 2267, 2677, 60, 1, "Heals a friendly target for ", "."},
        {3, 1110, 1110, 60, 5, "Heals the target for ", " over 15 sec."},
        {5, 270, 270, 10, 9, "Roots the target in place and causes ",
         " Nature damage over 27 sec.  Damage caused may interrupt the effect.  Only useable outdoors."},
    };
    for (bool missingAlteration : {false, true}) {
        for (const auto& ex : examples) {
            auto description = [&](bool skyrim) {
                const int skill = missingAlteration && ex.slot == 5 ? 50 : ex.skill;
                const double factor = skyrim ? 0.25 * (0.5 + skill / 100.0) : 1.0;
                // nearbyint uses ties-to-even, like Python round in service/skyrim.py.
                const auto low = std::to_string(static_cast<int>(std::nearbyint(ex.low / ex.ticks * factor)) * ex.ticks);
                const auto high = std::to_string(static_cast<int>(std::nearbyint(ex.high / ex.ticks * factor)) * ex.ticks);
                return std::string(ex.prefix) + low + (low == high ? "" : " to " + high) + ex.suffix;
            };
            for (bool book : {false, true}) {
                const std::string probe = "testDescription(" + std::to_string(ex.slot) + ", " + (book ? "true" : "false") + ")";
                auto checkDescription = [&](bool skyrim) {
                    run("return " + probe);
                    CHECK(std::string(lua_tostring(L, -1)) == description(skyrim));
                    lua_pop(L, 1);
                };
                run("__WB.resolve('player').skyrim = nil");
                checkDescription(false);
                run(missingAlteration ? "__WBApplyLive({player={skyrim={Destruction=30, Restoration=60}}})"
                                     : "__WBApplyLive({player={skyrim={Destruction=30, Restoration=60, Alteration=10}}})");
                checkDescription(true);
                run("__WB.resolve('player').skyrim = nil");
                checkDescription(false);
            }
        }
    }
    run(R"(
        local p = __WB.resolve('player')
        local armor = __WB.spellInfo(9885)
        local original = __WB.spellText(armor)
        p.skyrim = {}
        assert(__WB.spellText(armor) == original)
        -- A total is its converted tick times the ticks (the service converts each tick); timing and stat magnitudes stay.
        local info = {effects={{effect=6, aura=3, base=9, dice=1, sides=1, perLevel=0, period=1000},
                              {effect=6, aura=29, base=13, dice=1, sides=1, perLevel=0}},
                      spellLevel=1, maxLevel=0, duration=3000}
        assert(__WB.spellText(info, '$s1 / $o1 / $s2 / $t1 / $d') == '2 / 6 / 14 / 1 / 3 sec')
        assert(__WB.spellText(info, '$*3;s1') == '8')
        info.effects[1].base = 13
        assert(__WB.spellText(info, '$m1 / $M1') == '4 / 4')
        p.skyrim = nil
        assert(__WB.spellText(info, '$s1 / $o1 / $s2') == '14 / 42 / 14')
        -- Service-backed fixed budgets must agree with actual native spell events,
        -- even when skill changes. Keep captured aura profiles independent of later updates.
        local moonfire=__WB.spellInfo(9835)
        local savedProfiles=__WB.spellTooltips
        local fixed={effects={{99,99},{198,230},{0,0}},duration_ms=12000,health_factor=70/611,minimum_health=false}
        __WBSpellTooltips({[9835]=fixed})
        p.skyrim={Destruction=30}
        assert(__WB.spellText(moonfire,'$s2 / $o1 / $d')=='23 to 26 / 44 / 12 sec')
        p.skyrim.Destruction=100
        assert(__WB.spellText(moonfire,'$s2 / $o1')=='23 to 26 / 44')
        __WBSpellTooltips({[9835]={effects={{99,99},{198,230},{0,0}},health_factor=1/611,minimum_health=true}})
        assert(__WB.spellText(moonfire,'$s2 / $o1')=='1 / 4')
        assert(__WB.spellText(moonfire,'$s2 / $o1',fixed)=='23 to 26 / 44')
        p.skyrim=nil
        assert(__WB.spellText(moonfire,'$s2 / $o1')=='198 to 230 / 396')
        -- Rip and Ferocious Bite list their per-point damage as prose: converted as their tokens are.
        local rip, bite = __WB.spellInfo(9896), __WB.spellInfo(31018)
        __WBSpellTooltips({[9896]={effects={{0,0},{0,0},{0,0}},health_factor=0.1,minimum_health=false},
                           [31018]={effects={{0,0},{0,0},{0,0}},health_factor=0.1,minimum_health=false}})
        p.skyrim={OneHanded=50}
        local ripText, biteText = __WB.spellText(rip), __WB.spellText(bite)
        assert(string.find(ripText, '1 point  : 27 damage', 1, true) and string.find(ripText, '5 points: 94 damage', 1, true), ripText)
        assert(string.find(biteText, '1 point  : 20-26 damage', 1, true) and string.find(biteText, '5 points: 79-85 damage', 1, true), biteText)
        p.skyrim=nil
        assert(string.find(__WB.spellText(rip), '5 points: 942 damage', 1, true))
        -- Their Skyrim skill for the spell: one grey line before its description, in Skyrim only.
        __WBSpellTooltips({[__WB.actions[1].spell]={effects={{0,0},{0,0},{0,0}},skill='Destruction'}})
        p.skyrim={Destruction=30}
        GameTooltip:SetOwner(UIParent,'ANCHOR_NONE') GameTooltip:SetAction(1)
        local found
        for i=1,GameTooltip:NumLines() do if getglobal('GameTooltipTextLeft'..i):GetText()=='Destruction' then found=i end end
        assert(found and found==GameTooltip:NumLines()-1, 'skill line '..tostring(found))
        p.skyrim=nil
        GameTooltip:SetAction(1)
        for i=1,GameTooltip:NumLines() do assert(getglobal('GameTooltipTextLeft'..i):GetText()~='Destruction') end
        __WB.spellTooltips=savedProfiles
        -- The service suite regenerates these profiles and expected amounts
        -- from real resolve replies, including the converted aura/queue fields.
        p.skyrim={}
        for _, case in ipairs(__nativeHealthCases) do
            __WBSpellTooltips({[case.spell]=case.profile})
            local info=__WB.spellInfo(case.spell)
            local actual=__WB.spellText(info,case.text)
            assert(actual==case.actual, info.name..': '..actual..' ~= '..case.actual)
            assert(__WB.spellText(info,case.text,case.profile)==case.actual)
        end
        __WBSpellTooltips({[9830]={health_factor=7/96.5},[9867]={health_factor=7/96.5},
                           [9850]={health_factor=7/96.5},[31018]={health_factor=7/96.5}})
        assert(__WB.spellText(__WB.spellInfo(9830),'$s1 / $s2 / $s3')=='6 / 225 / 1')
        assert(__WB.spellText(__WB.spellInfo(9867),'$s1 / $s2 / $s3')=='7 / 350 / 1')
        assert(__WB.spellText(__WB.spellInfo(9850),'$s1 / $s2')=='8 / 1')
        assert(__WB.spellText(__WB.spellInfo(31018),'$f1')=='0.2')
        assert(__WB.spellText(__WB.spellInfo(9846),'$d')=='6 sec')
        assert(__WB.spellText(__WB.spellInfo(5229),'$/10;s1')=='2') -- rage
        assert(__WB.spellText(__WB.spellInfo(29166),'$s2% / $s1%')=='400% / 100%') -- mana regen
        __WB.spellTooltips=savedProfiles
        p.skyrim=nil
        -- Regrowth's catalog uses the same heal/range/period tokens as the other
        -- spells. Preserve its spelling in both modes, through action and book APIs.
        for _, id in ipairs({8936,8938,8939,8940,8941,9750,9856,9857,9858}) do
            local s = __WB.spellInfo(id)
            assert(s.name == 'Regrowth')
            assert(s.description == 'Heals a friendly target for $s1 and another $o2 over $d.')
            for _, skyrim in ipairs({false, true}) do
                p.skyrim = skyrim and {Restoration=60} or nil
                local desc = __WB.spellText(s)
                assert(not strfind(desc, 'W', 1, true) and not strfind(desc, '$', 1, true))
                if id == 9858 then
                    assert(desc == (skyrim and 'Heals a friendly target for 276 to 308 and another 294 over 21 sec.'
                        or 'Heals a friendly target for 1003 to 1119 and another 1064 over 21 sec.'))
                end
                __WB.actions[9] = {spell=id}
                GameTooltip:SetAction(9)
                assert(GameTooltipTextLeft1:GetText() == 'Regrowth' and GameTooltipTextLeft4:GetText() == desc)
                for index, spell in ipairs(__WB.spellbook) do
                    if spell == id then
                        GameTooltip:SetSpell(index, 'spell')
                        assert(GameTooltipTextLeft1:GetText() == 'Regrowth' and GameTooltipTextLeft4:GetText() == desc)
                        break
                    end
                end
            end
        end
        -- Audit every other recorded spell's substitutions in both modes as well.
        for _, skyrim in ipairs({false, true}) do
            p.skyrim = skyrim and {Restoration=60, Destruction=30, Alteration=10} or nil
            for _, id in ipairs(__WB.spellbook) do
                assert(not strfind(__WB.spellText(__WB.spellInfo(id)), '$', 1, true))
            end
        end
    )");
    engine.shutdown();
}

TEST_CASE("form defaults start with Attack and preserve an arranged page", "[client][api][forms][defaults]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    const wowcore::Mpq mpq(data);
    const wowcore::ClientDb db(mpq);
    wowee::addons::LuaEngine engine;
    REQUIRE(engine.initialize());
    std::string err;
    REQUIRE(wowcore::installWowApi(engine, db, &err));
    lua_State* L = engine.getState();
    const int result = luaL_dostring(L, R"(
        __WB.actions = {[1]={spell=6603}}
        __WBDefaultFormActions()
        for _, first in ipairs({73,85,97}) do
            assert(__WB.actions[first].spell == 6603 and IsAttackAction(first) == 1)
        end
        assert(__WB.actions[74].spell == 9850 and __WB.actions[86].spell == 9867 and __WB.actions[98].spell == 9881)
        assert(__WB.actions[83].spell == 9913 and not __WB.actions[84])
        assert(__WB.actions[89].spell == 9904 and not __WB.actions[90])
        assert(__WB.actions[105].spell == 17392 and not __WB.actions[106])
        __WB.actions[73] = {spell=9913} -- the player replaced Attack
        __WB.actions[74] = nil -- and left an intentional gap
        __WB.actions[85] = nil -- an arranged page can have its first slot empty
        local cat, prowl = __WB.actions[73], __WB.actions[86]
        __WBDefaultFormActions()
        assert(__WB.actions[73] == cat and not __WB.actions[74])
        assert(not __WB.actions[85] and __WB.actions[86] == prowl)
        assert(__WB.actions[1].spell == 6603)
        __WB.actions = {[84]={spell=9913}} -- any of the twelve slots marks a populated page
        __WBDefaultFormActions()
        assert(not __WB.actions[73] and not __WB.actions[74] and __WB.actions[84].spell == 9913)
    )");
    INFO((result ? lua_tostring(L, -1) : ""));
    REQUIRE(result == 0);
}

TEST_CASE("shapeshift paragraphs retain source text and reserve every drawn row", "[client][tooltip][forms]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
    auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run("__WBLogin() __WBForm(0)");
    const auto& tree = ui.engine().widgets();
    for (int index = 1; index <= 4; ++index) {
        const std::string script = "GameTooltip_SetDefaultAnchor(GameTooltip, UIParent) GameTooltip:SetShapeshift(" +
            std::to_string(index) + ")";
        run(script.c_str());
        run(R"(
            assert(GameTooltip:NumLines() == 4)
            assert(GameTooltipTextRight1:GetText() == 'Shapeshift')
            assert(GameTooltipTextLeft3:GetText() == 'Instant cast')
            assert(strfind(GameTooltipTextLeft4:GetText(), '\r\n\r\n', 1, true))
            GameTooltip:AddLine('After the paragraph', 1, 1, 1)
            GameTooltip:Show()
        )");
        for (int i = 0; i < 3; ++i) textures.sync(ui.frame(1.0f / 60));
        const auto* paragraph = tree.findByName("GameTooltipTextLeft4");
        const auto* after = tree.findByName("GameTooltipTextLeft5");
        REQUIRE(paragraph);
        REQUIRE(after);
        CHECK(paragraph->text.find('\r') == std::string::npos);
        CHECK(paragraph->text.find('?') == std::string::npos);
        CHECK(paragraph->wrappedLines > 3);
        CHECK(paragraph->rectH == Catch::Approx(paragraph->wrappedLines * wowee::ui::interfaceLineHeight(paragraph->fontHeight)));
        CHECK(after->bottom + after->rectH <= paragraph->bottom - 2 + 0.01f);
    }
    // Hard breaks count even without word wrapping, and markup breaks use the same parser.
    run(R"(
        GameTooltip:ClearLines()
        GameTooltip:AddLine('First\r\nSecond|nThird', 1, 1, 1)
        GameTooltip:AddLine('Last', 1, 1, 1)
        GameTooltip:Show()
    )");
    for (int i = 0; i < 3; ++i) textures.sync(ui.frame(1.0f / 60));
    const auto* first = tree.findByName("GameTooltipTextLeft1");
    const auto* last = tree.findByName("GameTooltipTextLeft2");
    CHECK(first->wrappedLines == 3);
    CHECK(last->bottom + last->rectH <= first->bottom - 2 + 0.01f);
}

TEST_CASE("FrameXML repeats textures without relying on the host sampler", "[client][forms][render]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
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
        __WBDruidForms()
        -- Five buttons exercise three middle tiles, independently of which fifth form is learned.
        __WB.forms[5] = {spell=24858, icon=__WB.spellInfo(24858).icon, name='Moonkin Form', castable=true}
        __WBFire('UPDATE_SHAPESHIFT_FORMS')
        assert(ShapeshiftBarMiddle:GetWidth() == 114)
    )");
    const auto& tree = ui.engine().widgets();
    auto* draw = ui.frame(1.0f / 60);
    textures.sync(draw);
    const auto* middle = tree.findByName("ShapeshiftBarMiddle");
    REQUIRE(middle);
    CHECK(middle->texCoord[1] == 3); // the client's UVs remain untouched
    auto checkUVs = [&](const ImDrawData* dd) {
        bool outside = false, invalidIndex = false;
        float lo = 0, hi = 1;
        for (const auto* list : dd->CmdLists) {
            for (const auto& cmd : list->CmdBuffer) {
                if (cmd.UserCallback) continue;
                for (unsigned int i = 0; i < cmd.ElemCount; ++i) {
                    const unsigned int index = cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i];
                    if (index >= static_cast<unsigned int>(list->VtxBuffer.Size)) { invalidIndex = true; continue; }
                    const auto& uv = list->VtxBuffer[index].uv;
                    lo = std::min(lo, std::min(uv.x, uv.y));
                    hi = std::max(hi, std::max(uv.x, uv.y));
                    outside |= uv.x < -0.00001f || uv.x > 1.00001f || uv.y < -0.00001f || uv.y > 1.00001f;
                }
            }
        }
        INFO("UV range " << lo << " .. " << hi << "; vertices " << dd->TotalVtxCount);
        CHECK_FALSE(outside);
        CHECK_FALSE(invalidIndex);
    };
    checkUVs(draw);
    // The appearance manager also runs when forms arrive above an already enabled bottom bar.
    run("__WB.barToggles={1} SHOW_MULTI_ACTIONBAR_1=1 MultiActionBar_Update() __WBDruidForms()");
    textures.sync(ui.frame(1.0f / 60));
    run(R"(
        assert(not ShapeshiftBarLeft:IsShown() and not ShapeshiftBarMiddle:IsShown() and not ShapeshiftBarRight:IsShown())
        for i=1,4 do assert(getglobal('ShapeshiftButton'..i..'NormalTexture'):GetWidth() == 50) end
        SHOW_MULTI_ACTIONBAR_1=nil MultiActionBar_Update() __WBDruidForms()
        assert(ShapeshiftBarLeft:IsShown() and ShapeshiftBarMiddle:IsShown() and ShapeshiftBarRight:IsShown())
        for i=1,4 do assert(getglobal('ShapeshiftButton'..i..'NormalTexture'):GetWidth() == 64) end
    )");
    // A two-axis repeat, including negative UVs, must preserve pixels across every tile.
    const uint8_t pixels[] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255};
    const auto tex = textures.uploadImGuiTexture(pixels, 2, 2);
    draw = ui.frame(1.0f / 60, [&](ImDrawList* list) {
        list->AddImage(reinterpret_cast<ImTextureID>(tex), {500,300}, {590,390}, {-1,-1}, {2,2});
    });
    textures.sync(draw);
    checkUVs(draw);
    wowcore::Image image(1280, 720);
    wowcore::rasterize(draw, textures, image, wowcore::Filter::Bilinear);
    auto pixel = [&](int x, int y, int c) { return image.rgba[(y * image.w + x) * 4 + c]; };
    for (int c = 0; c < 4; ++c) {
        CHECK(pixel(508,308,c) == pixel(538,308,c));
        CHECK(pixel(508,308,c) == pixel(508,338,c));
    }
    // Enough tiles to cross the 16-bit vertex limit; command-local offsets remain valid.
    draw = ui.frame(1.0f / 60, [&](ImDrawList* list) {
        list->AddImage(reinterpret_cast<ImTextureID>(tex), {500,300}, {600,400}, {120,120}, {0,0});
    });
    textures.sync(draw);
    checkUVs(draw);
    CHECK(draw->TotalVtxCount > 65535);
}

TEST_CASE("1.12 aura tooltips place, wrap and fill the player's buff as the client does", "[client][tooltip][aura]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
    auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run("__WBLogin()");
    auto settle = [&] { for (int i = 0; i < 3; ++i) textures.sync(ui.frame(1.0f / 60)); };
    settle();
    // Reuse the action tooltip's one-line fields: they must grow into a wrapped aura paragraph.
    run("GameTooltip:SetAction(1)");
    settle();
    run(R"(
        __WBAuraAdd('player', 9885, false, 1800)
        GameTooltip:SetOwner(BuffButton0, 'ANCHOR_BOTTOMLEFT')
        GameTooltip:SetPlayerBuff(0)
        assert(GameTooltip:NumLines() == 3)
        assert(GameTooltipTextLeft1:GetText() == 'Mark of the Wild')
        assert(GameTooltipTextRight1:GetText() == 'Magic')
        assert(GameTooltipTextLeft2:GetText() == 'Increases armor by 285, all attributes by 12 and all resistances by 20.')
        assert(GameTooltipTextLeft3:GetText() == '30 minutes remaining')
        local r, g, b = GameTooltipTextLeft3:GetTextColor()
        assert(r == 1 and math.abs(g - 210/255) < 0.00001 and b == 0)
    )");
    settle();
    const auto& tree = ui.engine().widgets();
    const auto* tip = tree.findByName("GameTooltip");
    const auto* owner = tree.findByName("BuffButton0");
    const auto* description = tree.findByName("GameTooltipTextLeft2");
    const auto* dispel = tree.findByName("GameTooltipTextRight1");
    REQUIRE(tip);
    REQUIRE(owner);
    REQUIRE(description);
    REQUIRE(dispel);
    REQUIRE(tip->anchors.size() == 1);
    CHECK(tip->anchors[0].point == "TOPRIGHT");
    CHECK(tip->anchors[0].relativeTo == owner->id);
    CHECK(tip->anchors[0].relativePoint == "BOTTOMLEFT");
    CHECK(tip->left + tip->rectW == Catch::Approx(owner->left));
    CHECK(tip->bottom + tip->rectH == Catch::Approx(owner->bottom));
    CHECK(dispel->left + dispel->rectW == Catch::Approx(tip->left + tip->rectW - 10));
    CHECK(description->wrappedLines == 2); // updated by the actual draw; one would mean truncation with "..."
    auto* font = wowee::ui::interfaceFaceOrDefault(description->fontFace);
    REQUIRE(font);
    const auto rows = wowee::ui::wrapText(wowee::ui::parseMarkup(description->text), description->width, false,
        [&](const std::string& piece) {
            return wowee::ui::interfaceMeasure(font, wowee::ui::interfaceFontSize(description->fontHeight), piece);
        });
    REQUIRE(rows.size() == 2);
    std::vector<std::string> lines;
    for (const auto& row : rows) {
        std::string line;
        for (const auto& part : row) line += part.text;
        while (!line.empty() && line.back() == ' ') line.pop_back();
        lines.push_back(line);
    }
    CHECK(lines[0] == "Increases armor by 285, all attributes");
    CHECK(lines[1] == "by 12 and all resistances by 20.");

    run(R"(
        -- Units expose name, dispel and description, but no duration in the 1.12 aura APIs.
        __WB.tokens.target = __WB.tokens.player
        GameTooltip:SetUnitBuff('target', 1)
        assert(GameTooltip:NumLines() == 2 and GameTooltipTextRight1:GetText() == 'Magic')
        __WBAuraAdd('player', 9853, true, 27)
        GameTooltip:SetUnitDebuff('target', 1)
        assert(GameTooltip:NumLines() == 2 and GameTooltipTextRight1:GetText() == 'Magic')
        GameTooltip:SetPlayerBuff(16)
        assert(GameTooltipTextLeft3:GetText() == '27 seconds remaining')
        local a = __WB.resolve('player').buffs[1]
        for _, example in ipairs({{1, '1 second'}, {59.1, '60 seconds'}, {60, '1 minute'},
                                  {61, '2 minutes'}, {3600, '1 hour'}, {3601, '2 hours'},
                                  {86400, '1 day'}, {86401, '2 days'}}) do
            a.expires = GetTime() + example[1]
            GameTooltip:SetPlayerBuff(0)
            assert(GameTooltipTextLeft3:GetText() == example[2] .. ' remaining')
        end
        a.untilCancelled = true
        GameTooltip:SetPlayerBuff(0)
        assert(GameTooltip:NumLines() == 2)
        a.untilCancelled, a.expires = nil, GetTime() - 1
        GameTooltip:SetPlayerBuff(0)
        assert(GameTooltip:NumLines() == 2)
        -- No helpful aura may fall through to a debuff slot; invalid indices clear and hide the tooltip.
        GameTooltip:SetPlayerBuff(-1)
        assert(not GameTooltip:IsShown() and GameTooltip:NumLines() == 0)
        GameTooltip:SetUnitBuff('target', 99)
        assert(not GameTooltip:IsShown() and GameTooltip:NumLines() == 0)
        __WBSkyrimAura(123, 'Skyrim blessing', 'Interface\\Icons\\Spell_Nature_Regeneration', false, 120,
                       string.rep('A long Skyrim effect description wraps completely without truncation. ', 6), 'Magic')
        GameTooltip:SetPlayerBuff(1)
        assert(GameTooltipTextRight1:GetText() == 'Magic')
        assert(GameTooltipTextLeft3:GetText() == '2 minutes remaining')
    )");
    settle();
    CHECK(description->wrappedLines > 6);
    const auto* time = tree.findByName("GameTooltipTextLeft3");
    REQUIRE(time);
    CHECK(time->bottom + time->rectH <= description->bottom);
    CHECK(tip->left >= 0);
    CHECK(tip->left + tip->rectW <= owner->left + 0.01f);
    run(R"(
        __WB.resolve('player').buffs[2].dispel = nil
        GameTooltip:SetUnitBuff('player', 2)
        assert(GameTooltip:NumLines() == 2 and not GameTooltipTextRight1:IsShown())
        -- Reusing a wrapped description for a short row must also shrink its height.
        GameTooltip:SetAction(1)
    )");
    settle();
    CHECK(description->wrappedLines == 1);
    struct AnchorExample { const char* name; const char* point; const char* relativePoint; };
    for (const auto& example : {
        AnchorExample{"ANCHOR_TOPLEFT", "BOTTOMRIGHT", "TOPLEFT"},
        AnchorExample{"ANCHOR_TOPRIGHT", "BOTTOMLEFT", "TOPRIGHT"},
        AnchorExample{"ANCHOR_BOTTOMLEFT", "TOPRIGHT", "BOTTOMLEFT"},
        AnchorExample{"ANCHOR_BOTTOMRIGHT", "TOPLEFT", "BOTTOMRIGHT"},
        AnchorExample{"ANCHOR_LEFT", "BOTTOMRIGHT", "TOPLEFT"},
        AnchorExample{"ANCHOR_RIGHT", "BOTTOMLEFT", "TOPRIGHT"},
        AnchorExample{"ANCHOR_TOP", "BOTTOM", "TOP"},
        AnchorExample{"ANCHOR_BOTTOM", "TOP", "BOTTOM"}}) {
        const auto script = std::string("GameTooltip:SetOwner(BuffButton0, '") + example.name + "', 15, -25)";
        run(script.c_str());
        REQUIRE(tip->anchors.size() == 1);
        CHECK(tip->anchors[0].point == example.point);
        CHECK(tip->anchors[0].relativePoint == example.relativePoint);
        CHECK(tip->anchors[0].x == 15);
        CHECK(tip->anchors[0].y == -25);
    }
    run(R"(
        GameTooltip:SetOwner(BuffButton0, 'ANCHOR_PRESERVE')
    )");
    CHECK(tip->anchors.size() == 1);
    run("GameTooltip:SetOwner(UIParent, 'ANCHOR_NONE')");
    CHECK(tip->anchors.empty());
}

TEST_CASE("SoundEntries: the interface's named sounds resolve to the client's files", "[client][dbc]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    const wowcore::Mpq mpq(data);
    const wowcore::ClientDb db(mpq);
    const auto* open = db.sound("IGMAINMENUOPEN");
    REQUIRE(open);
    CHECK(open->files == std::vector<std::string>{ "Sound\\Interface\\uEscapeScreenOpen.wav" });
    CHECK(open->volume == 0.5f);
    CHECK(mpq.exists(open->files[0]));
    const auto* lost = db.sound("INTERFACESOUND_LOSTTARGETUNIT");   // two rows of this name: the first
    REQUIRE(lost);
    CHECK(lost->id == 684);
    // Wrath's visual: the LifeDrainLoop precast (a loop), Nature Cast, Lightning Bolt Impact; its missile's sound.
    const auto wrath = db.spellSounds(9912);
    CHECK((wrath.precast == 3087 && wrath.cast == 2561 && wrath.impact == 1506 && wrath.missile == 3087));
    CHECK(db.sound(wrath.precast)->loop);
    CHECK((wrath.precastAnim == 51 && wrath.castAnim == 53));   // ReadySpellDirected, SpellCastDirected
    CHECK(db.spellSounds(17402).channelAnim == 125);            // Hurricane: ChannelCastOmni
    // A staff (2, subclass 10) of wood: Mace1H on flesh; heavy whooshes.
    CHECK(db.weaponImpactSound(10, 0, 0, false) == db.sound("Mace1H_ArmorFlesh")->id);
    CHECK(db.weaponImpactSound(10, 0, 0, true) == db.sound("Mace1H_ArmorFleshCritical")->id);
    CHECK(db.weaponSwingSound(2, false) == db.sound("HeavyWeaponNormal")->id);
    CHECK(db.spellVisualKits(17402).area == "Spells\\LightningStorm_Cloud_State.mdx");   // its storm, stood where it lands
    const auto kits = db.spellVisualKits(9912);                 // Wrath: a glow on both hands, a missile, a chest impact
    REQUIRE(kits.precast.size() == 2);
    CHECK((kits.precast[0].slot == 3 && kits.precast[1].slot == 4 && kits.precast[0].model == "spells\\wrath_precast_hand.mdx"));
    CHECK(kits.missile == "spells\\wrath_missile.mdx");
    REQUIRE(kits.impact.size() == 1);
    CHECK(kits.impact[0].slot == 1);
    CHECK_FALSE(db.sound(wrath.impact)->loop);
    const auto rejuv = db.spellVisualKits(25299);               // Rejuvenation: their swirl at the player's feet as it lands
    REQUIRE(rejuv.impact.size() == 1);
    const auto swirl = wowcore::loadModel(mpq, rejuv.impact[0].model);   // all particles and ribbons, no geometry
    REQUIRE(swirl);
    CHECK((swirl->vertices.empty() && !swirl->particleEmitters.empty()));
    CHECK((rejuv.impact[0].slot == 2 && rejuv.impact[0].model == "Spells\\Rejuvenation_Impact_Base.mdx"));
}

TEST_CASE("1.12 cooldown sweep turns about a 36 pixel action button centre", "[client][cooldown]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    REQUIRE(luaL_dostring(L, "__WBFixture('idle')") == 0);
    REQUIRE(ui.loadInterface(&err));
    // Isolate the real FrameXML cooldown model on an action-sized button so its alpha gives the dark wedge mask.
    REQUIRE(luaL_dostring(L, R"(
        for _, f in ipairs({UIParent:GetChildren()}) do
            if f:GetName() then f:Hide() end
        end
        SweepButton = CreateFrame("Button", "SweepButton", UIParent)
        SweepButton:SetWidth(38.4) SweepButton:SetHeight(38.4)
        SweepButton:SetPoint("CENTER", UIParent, "CENTER", 0, 0)
        SweepCooldown = CreateFrame("Model", "SweepCooldown", SweepButton, "CooldownFrameTemplate")
        SweepCooldown:SetAllPoints(SweepButton)
        __sweepNow = 1000
        GetTime = function() return __sweepNow end
        local publish = __WBModelState
        __WBModelState = function(f, file, anim, ms)
            if f == SweepCooldown then __sweepMs = ms end
            publish(f, file, anim, ms)
        end
    )") == 0);
    textures.sync(ui.frame(1.0f / 60)); // establish visibility before dispatching model updates
    for (double duration : {60.0, 1.5}) {
        for (double progress : {0.10, 0.20, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90}) {
            char script[256];
            std::snprintf(script, sizeof(script),
                "__sweepNow = %.9f; CooldownFrame_SetTimer(SweepCooldown, 1000, %.9f, 1)",
                1000 + duration * progress, duration);
            REQUIRE(luaL_dostring(L, script) == 0);
            textures.sync(ui.frame(1.0f / 60)); // publish Show through the interface visibility update
            ImDrawData* dd = ui.frame(1.0f / 60);
            textures.sync(dd);
            wowcore::Image img(1280, 720);
            wowcore::rasterize(dd, textures, img);
            lua_getglobal(L, "__sweepMs");
            CHECK(std::abs(lua_tonumber(L, -1) - progress * 1000) < 0.001);
            lua_pop(L, 1);
            const auto& tree = ui.engine().widgets();
            const auto* button = tree.findByName("SweepButton");
            REQUIRE(button);
            const double scale = tree.uiScale();
            const double cx = (button->left + button->rectW / 2) * scale;
            const double cy = ui.height() - (button->bottom + button->rectH / 2) * scale;
            REQUIRE(std::abs(button->rectW * scale - 36) < 0.001);
            REQUIRE(std::abs(button->rectH * scale - 36) < 0.001);
            auto alpha = [&](int x, int y) {
                return double(img.rgba[(size_t(y) * img.w + x) * 4 + 3]);
            };
            // Fit the half-opacity contour of the dark wedge, away from its apex and the button's border.
            // In each quadrant a runs clockwise from the centre and b runs along the starting radius.
            // Choose the shallower slope, then extrapolate that edge to the centre seam (the wedge's apex).
            const int quadrant = int(progress * 4);
            const bool scanA = progress * 4 - quadrant < 0.5;
            auto at = [&](int a, int b) {
                const int x = int(std::lround(cx)), y = int(std::lround(cy));
                switch (quadrant) {
                case 0: return alpha(x + a, y - 1 - b);
                case 1: return alpha(x + b, y + a);
                case 2: return alpha(x - 1 - a, y + b);
                default: return alpha(x - 1 - b, y - 1 - a);
                }
            };
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            int count = 0;
            for (int i = 2; i < 16; ++i) {
                for (int k = 1; k < 16; ++k) {
                    // At the half turn the boundary lies on the centre seam; sample across both halves.
                    const double a = progress == 0.5 ? alpha(int(cx) + k - 9, int(cy) + i)
                        : scanA ? at(k, i) : at(i, k);
                    const double b = progress == 0.5 ? alpha(int(cx) + k - 8, int(cy) + i)
                        : scanA ? at(k + 1, i) : at(i, k + 1);
                    constexpr double halfAlpha = 153.0 / 2; // cooldown.blp's dark side is alpha 153
                    const bool crosses = progress != 0.5 && scanA ? a < halfAlpha && b >= halfAlpha
                        : a >= halfAlpha && b < halfAlpha;
                    if (!crosses) continue;
                    const double x = i + 0.5;
                    const double y = k + 0.5 + (halfAlpha - a) / (b - a) - (progress == 0.5 ? 9 : 0);
                    sx += x; sy += y; sxx += x * x; sxy += x * y;
                    ++count;
                    break;
                }
            }
            INFO("duration=" << duration << " progress=" << progress << " centre=" << cx << "," << cy);
            REQUIRE(count >= 8);
            const double slope = (count * sxy - sx * sy) / (count * sxx - sx * sx);
            const double apex = (sy - slope * sx) / count;
            INFO("apex offset from button centre=" << apex << " pixels");
            CHECK(std::abs(apex) <= 1.0);
        }
    }
}

TEST_CASE("complete host talents render client art, desaturation and scroll children", "[client][api][talents][talent-render]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 2560, 1440);
    std::string err;
    REQUIRE(ui.start(&err));
    auto* L = ui.lua();
    auto run = [&](const char* script) {
        const int rc = luaL_dostring(L, script);
        INFO(script);
        INFO((rc ? lua_tostring(L, -1) : ""));
        REQUIRE(rc == 0);
    };
    for (const char* file : {"elunara.lua", "hud_states.lua", "talents_render.lua"})
        REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/" + file).c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    auto& tree = ui.engine().widgets();
    auto widget = [&](const std::string& name) -> const wowee::ui::Widget& {
        const auto* w = tree.findByName(name);
        REQUIRE(w);
        return *w;
    };
    auto render = [&] {
        ImDrawData* draw = nullptr;
        for (int i = 0; i < 90; ++i) { draw = ui.frame(1.0f / 60); textures.sync(draw); }
        CHECK(ui.engine().lastError().empty());
        wowcore::Image image(2560, 1440);
        wowcore::rasterize(draw, textures, image);
        return image;
    };
    run(R"(
        __WBLogin()
        -- A host answer can arrive before the addon, and later refresh one already open.
        local probe = CreateFrame('Frame')
        probe:RegisterEvent('CHARACTER_POINTS_CHANGED')
        probe:SetScript('OnEvent', function() assert(type(arg1)=='number' and arg2==0) end)
        __WBTalentFixture(false)
        ToggleTalentFrame()
        assert(GetNumTalents(1)==16 and UnitCharacterPoints('player')==51)
    )");
    const auto initial = render();
    for (int i = 1; i <= 16; ++i) {
        const auto& w = widget("TalentFrameTalent" + std::to_string(i) + "IconTexture");
        CHECK(w.visible);
        CHECK(w.clipTo == widget("TalentFrameScrollFrame").id);
        CHECK(w.desaturated == (i > 2));
    }
    for (const char* corner : {"TopLeft", "TopRight", "BottomLeft", "BottomRight"}) {
        const auto& w = widget(std::string("TalentFrameBackground") + corner);
        CHECK(w.visible);
        CHECK(w.texturePath == std::string("Interface\\TalentFrame\\DruidBalance-") + corner);
    }
    CHECK(widget("TalentFrameTab2").left > widget("TalentFrameTab1").left + 50);
    CHECK(widget("TalentFrameTab3").left > widget("TalentFrameTab2").left + 80);
    CHECK(widget("TalentFrameScrollFrameScrollBarScrollUpButton").clipTo == 0);
    CHECK(widget("TalentFrameScrollFrameScrollBarScrollDownButton").visible);
    CHECK(widget("TalentFrameCancelButton").visible);
    CHECK(widget("TalentFrameArrow1").visible);
    CHECK(widget("TalentFrameArrow1").texCoord[0] != widget("TalentFrameArrow2").texCoord[0]);
    CHECK(widget("TalentFrameBranch1").visible);
    // The region template's 32x32 size and TOPLEFT anchor must survive the
    // derived XML node (which has no Anchors of its own). Otherwise branches
    // stretch across the tree and vertical branches collapse to zero height.
    for (int i = 1; i <= 7; ++i) {
        const auto& branch = widget("TalentFrameBranch" + std::to_string(i));
        CHECK(branch.rectW == Catch::Approx(32));
        CHECK(branch.rectH == Catch::Approx(32));
    }
    const auto& thumb = widget("TalentFrameScrollFrameScrollBarThumbTexture");
    CHECK(thumb.rectW == Catch::Approx(16));
    CHECK(thumb.rectH == Catch::Approx(16));
    // Both use Nature's Grasp's icon: one colored, the other grey. The shared source must remain colored.
    auto coloredPixels = [&](const wowee::ui::Widget& w, const wowcore::Image& image) {
        int count = 0;
        const float scale = tree.uiScale();
        const int x0 = static_cast<int>((w.left + 4) * scale);
        const int y0 = static_cast<int>(1440 - (w.bottom + w.rectH - 4) * scale);
        for (int y = y0; y < y0 + 30; ++y) for (int x = x0; x < x0 + 30; ++x) {
            const auto at = (static_cast<size_t>(y) * image.w + x) * 4;
            if (std::max({image.rgba[at], image.rgba[at+1], image.rgba[at+2]}) -
                std::min({image.rgba[at], image.rgba[at+1], image.rgba[at+2]}) > 8) ++count;
        }
        return count;
    };
    CHECK(coloredPixels(widget("TalentFrameTalent2IconTexture"), initial) > 100);
    CHECK(coloredPixels(widget("TalentFrameTalent3IconTexture"), initial) == 0);
    auto luminance = [&](const wowee::ui::Widget& w) {
        double sum = 0;
        const float scale = tree.uiScale();
        const int x0 = static_cast<int>((w.left + 4) * scale);
        const int y0 = static_cast<int>(1440 - (w.bottom + w.rectH - 4) * scale);
        for (int y = y0; y < y0 + 30; ++y) for (int x = x0; x < x0 + 30; ++x) {
            const auto at = (static_cast<size_t>(y) * initial.w + x) * 4;
            sum += (299 * initial.rgba[at] + 587 * initial.rgba[at+1] + 114 * initial.rgba[at+2]) / 1000.0;
        }
        return sum / 900;
    };
    // Blizzard passes a 0.65 RGB tint too, but the client's grey shader keeps the source luminance.
    CHECK(luminance(widget("TalentFrameTalent3IconTexture")) /
          luminance(widget("TalentFrameTalent2IconTexture")) == Catch::Approx(1.0).margin(0.05));
    run(R"(
        __WBTalentFixture(true)
        assert(TalentFrameTalentPointsText:GetText()=='27')
        assert(TalentFrameTalent1Rank:GetText()=='5')
        assert(TalentFrameTalent11Rank:GetText()=='3')
        assert(TalentFrameTalent3IconTexture:SetDesaturated(nil))
        assert(TalentFrameTalent3IconTexture:SetDesaturated(1))
    )");
    render();
    CHECK_FALSE(widget("TalentFrameTalent9IconTexture").desaturated);
    CHECK(widget("TalentFrameTalent9Rank").visible);
    CHECK(widget("TalentFrameTalent11Rank").visible);
    run("__WBTalentFixture(false)");
    render();
    CHECK(widget("TalentFrameTalent9IconTexture").desaturated);
    CHECK_FALSE(widget("TalentFrameTalent9Rank").visible);
    // Same session: use the host's two Skyrim quests and then a long description to exercise the viewport.
    run("HideUIPanel(TalentFrame); assert(loadstring(HUD_STATES.quest_log.run))()");
    const auto quests = render();
    const auto& description = widget("QuestLogQuestDescription");
    CHECK(description.clipTo == widget("QuestLogDetailScrollFrame").id);
    CHECK(widget("QuestFrameExitButton").visible);
    CHECK_FALSE(widget("QuestLogFrameAbandonButton").enabled);
    // The lower parchment must be drawn, not merely present in the widget tree.
    const auto at = (static_cast<size_t>(900) * quests.w + 300) * 4;
    CHECK(quests.rgba[at + 3] > 240);
    CHECK(quests.rgba[at] > 100);
    run(R"(
        __WB.quests[1].text = string.rep('This description must stay inside the quest scroll area. ', 80)
        QuestLog_UpdateQuestDetails()
    )");
    render();
    run(R"(
        assert(QuestLogDetailScrollFrame:GetVerticalScrollRange()>100)
        QuestLogDetailScrollFrame:SetVerticalScroll(150)
        assert(QuestLogDetailScrollFrame:GetVerticalScroll()==150)
    )");
    render();
    CHECK(widget("QuestLogQuestDescription").clipTo == widget("QuestLogDetailScrollFrame").id);
}


TEST_CASE("vanilla spellbook selection and live clock retain client visual states", "[client][api][fidelity]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 2560, 1440);
    std::string err;
    REQUIRE(ui.start(&err));
    auto* L = ui.lua();
    auto run = [&](const char* code) {
        const int rc = luaL_dostring(L, code);
        INFO((rc ? lua_tostring(L, -1) : ""));
        REQUIRE(rc == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run(R"(
        __WBLogin() ToggleSpellBook(BOOKTYPE_SPELL)
        -- Execute the unchanged client handler, including its string booleans.
        for n=1,12 do
          this=getglobal('SpellButton'..n) SpellButton_UpdateSelection()
          assert(not this:GetChecked(), 'idle spell '..n..' must not glow')
        end
        this=nil
        SpellButton1:SetChecked('true') assert(SpellButton1:GetChecked()==1)
        SpellButton1:SetChecked('false') assert(not SpellButton1:GetChecked())
        SpellButton1:SetChecked(1) assert(SpellButton1:GetChecked()==1)
        SpellButton1:SetChecked(0) assert(not SpellButton1:GetChecked())
        SpellButton1:SetChecked() assert(SpellButton1:GetChecked()==1)
        SpellButton1:SetChecked(nil) assert(not SpellButton1:GetChecked())
        -- Midnight must select the moon atlas, not retain the whole sun/moon sheet.
        local sentinel={} this=sentinel
        __WBApplyLive({world={hour=0,minute=0}}) assert(this==sentinel)
        this=nil
    )");
    const auto* clock = ui.engine().widgets().findByName("GameTimeTexture");
    REQUIRE(clock);
    CHECK(clock->texCoord[0] == Catch::Approx(0.5));
    CHECK(clock->texCoord[1] == Catch::Approx(0.890625));
    CHECK(clock->texCoord[3] == Catch::Approx(0.78125));
    run("__WBApplyLive({world={hour=12,minute=0}})");
    CHECK(clock->texCoord[0] == Catch::Approx(0));
    run("__WBApplyLive({world={hour=23,minute=59}})");
    CHECK(clock->texCoord[0] == Catch::Approx(0.5));
    run("HideUIPanel(SpellBookFrame) __WBInstallWorldMap()");
    CHECK_FALSE(ui.engine().widgets().findByName("WBMinimapWorldMapButton"));
}

TEST_CASE("panel children initialize before final tab padding and tooltip width floors survive layout", "[client][api][fidelity]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 2560, 1440);
    std::string err;
    REQUIRE(ui.start(&err));
    auto* L = ui.lua();
    auto run = [&](const char* code) {
        const int rc = luaL_dostring(L, code);
        INFO((rc ? lua_tostring(L, -1) : ""));
        REQUIRE(rc == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/talents_render.lua").c_str()) == 0);
    run(R"(
        __WBLogin() __WBTalentFixture(false) LoadAddOn('Blizzard_TalentUI')
        ShowUIPanel(TalentFrame)
        function __checkTabPadding()
          for n=1,3 do
            local tab=getglobal('TalentFrameTab'..n)
            local label=getglobal(tab:GetName()..'Text')
            local left=getglobal(tab:GetName()..'Left')
            assert(math.abs(tab:GetWidth()-label:GetStringWidth()-10-2*left:GetWidth())<0.05)
          end
        end
        __checkTabPadding()
        GameTooltip:SetOwner(UIParent,'ANCHOR_NONE')
        GameTooltip:SetMinimumWidth(320)
        GameTooltip:SetText('Short')
        assert(GameTooltip:GetWidth()==320)
        UnplacedFidelityProbe=CreateFrame('Frame','UnplacedFidelityProbe',UIParent)
        UnplacedFidelityProbe:Hide()
        UnplacedFidelityProbe:SetWidth(32) UnplacedFidelityProbe:SetHeight(32)
        __fidelityShows=0
        UnplacedFidelityProbe:SetScript('OnShow',function() __fidelityShows=__fidelityShows+1 end)
        UnplacedFidelityProbe:Show() assert(__fidelityShows==1)
    )");
    for (int i = 0; i < 3; ++i) textures.sync(ui.frame(1.0f/60));
    run(R"(
        -- The deferred visibility pass must not repeat child OnShow and erase ten units.
        __checkTabPadding()
        -- An explicitly shown driver can acquire geometry later without a second OnShow.
        UnplacedFidelityProbe:SetPoint('CENTER',UIParent,'CENTER',0,0)
        assert(GameTooltip:GetWidth()==320)
        GameTooltip:AddLine(string.rep('A long line ',8),1,1,1)
        GameTooltip:Show() assert(GameTooltip:GetWidth()>320)
        GameTooltip:SetText('Short again') assert(GameTooltip:GetWidth()==320)
        GameTooltip:SetMinimumWidth(0) GameTooltip:Show()
        assert(GameTooltip:GetWidth()<320)
        GameTooltip:Hide() HideUIPanel(TalentFrame)
    )");
    for (int i = 0; i < 3; ++i) textures.sync(ui.frame(1.0f/60));
    run("assert(__fidelityShows==1) UnplacedFidelityProbe:Hide() ShowUIPanel(TalentFrame) __checkTabPadding()");
    for (int i = 0; i < 3; ++i) textures.sync(ui.frame(1.0f/60));
    run("__checkTabPadding()");
}

TEST_CASE("Skyrim experience drives the client bar, hover and coalesced level-up snapshots", "[client][api][xp]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    lua_State* L = ui.lua();
    auto run = [&](const char* script) {
        const int result = luaL_dostring(L, script);
        INFO(script);
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run(R"(
        __WBLogin()
        assert(not MainMenuExpBar:IsShown()) -- original level-60 fixture
        local p = __WB.resolve('player')
        p.restXP, p.rested = 200, true -- stale recorded rest must not leak into Skyrim
        __xpEvents, __xpLevels = {}, 0
        local probe = CreateFrame('Frame')
        probe:RegisterEvent('PLAYER_XP_UPDATE')
        probe:RegisterEvent('PLAYER_LEVEL_UP')
        probe:SetScript('OnEvent', function()
            if event == 'PLAYER_XP_UPDATE' then
                tinsert(__xpEvents, {arg1, UnitXP('player'), UnitXPMax('player')})
            else
                __xpLevels = __xpLevels + 1
                assert(UnitLevel('player') == arg1)
            end
        end)
        __WBApplyLive({player={level=60,xp=375.5,xpMax=1000}})
        assert(MainMenuExpBar:IsShown() and not MainMenuBarMaxLevelBar:IsShown())
        assert(UnitXP('player') == 375.5 and UnitXPMax('player') == 1000)
        assert(MainMenuExpBar:GetValue() == 375.5 and select(2, MainMenuExpBar:GetMinMaxValues()) == 1000)
        assert(__xpEvents[1][1] == 'player' and __xpEvents[1][2] == 375.5 and __xpEvents[1][3] == 1000)
        assert(not GetXPExhaustion() and GetRestState() == 2)
        assert(not ExhaustionTick:IsShown() and not ExhaustionLevelFillBar:IsShown())
        __WBApplyLive({player={xp=375.5,xpMax=1000}})
        __WBApplyLive({player={health=101}})
        assert(table.getn(__xpEvents) == 1) -- unchanged and unrelated snapshots are quiet
        __WBApplyLive({player={xpMax=1100}})
        assert(table.getn(__xpEvents) == 2 and __xpEvents[2][2] == 375.5 and __xpEvents[2][3] == 1100)
        assert(MainMenuBarExpText:GetText() == 'XP 375.5 / 1100')
        __WBApplyLive({player={xp=825}})
        assert(table.getn(__xpEvents) == 3 and MainMenuBarExpText:GetText() == 'XP 825 / 1100')
        assert(not MainMenuBarExpText:IsShown())
    )");
    for (int i = 0; i < 3; ++i) textures.sync(ui.frame(1.0f / 60));
    const auto& tree = ui.engine().widgets();
    const auto* bar = tree.findByName("MainMenuExpBar");
    REQUIRE(bar != nullptr);
    CHECK(bar->barColor[0] == 0.58f);
    CHECK(bar->barColor[1] == 0.0f);
    CHECK(bar->barColor[2] == 0.55f);
    const float x = (bar->left + bar->rectW / 2) * tree.uiScale();
    const float y = ui.height() - (bar->bottom + bar->rectH / 2) * tree.uiScale();
    ui.pointer(x, y);
    ui.engine().dispatchMouse(x, y, ui.height(), {});
    for (int i = 0; i < 120; ++i) textures.sync(ui.frame(1.0f / 60));
    run(R"(
        assert(MainMenuBarExpText:IsShown() and MainMenuBarExpText:GetText() == 'XP 825 / 1100')
        assert(GameTooltip:IsShown() and GameTooltipTextLeft1:GetText() == XPBAR_LABEL)
        assert(GameTooltip:NumLines() == 2) -- no delayed "Normal / 100%" rest line
        assert(GameTooltipTextLeft2:GetText() == string.match(NEWBIE_TOOLTIP_XPBAR, '^.-%.') .. "\nClick to open Skyrim's Skills menu.")
        __WBApplyLive({player={xp=1100,xpMax=1100}})
        assert(MainMenuExpBar:GetValue() == 1100) -- pending Skyrim level-up: full bar
        __WBApplyLive({player={xp=1250}})
        assert(UnitXP('player') == 1250 and MainMenuExpBar:GetValue() == 1250) -- banked XP stays exact in the API
        this=MainMenuExpBar MainMenuExpBar:GetScript('OnEnter')() this=nil
        assert(GameTooltipTextLeft1:GetText() == 'Level up available')
        assert(string.find(GameTooltipTextLeft2:GetText(),'Health, Magicka or Stamina',1,true))
        assert(UnitLevel('player') == 60 and __xpLevels == 0) -- pending XP alone cannot claim a native level
        SHOW_NEWBIE_TIPS='0'
        this=MainMenuExpBar MainMenuExpBar:GetScript('OnEnter')() this=nil
        ExhaustionToolTipText()
        assert(GameTooltip:IsShown() and GameTooltipTextLeft1:GetText() == 'Level up available')
        assert(string.find(GameTooltipTextLeft2:GetText(),'Health, Magicka or Stamina',1,true))
        SHOW_NEWBIE_TIPS='1'
        __WBApplyLive({player={level=61,xp=25,xpMax=1200}})
        assert(__xpLevels == 0 and UnitLevel('player') == 61 and PlayerLevelText:GetText() == '61')
        __WBApplyLive({player={level=61,xp=25,xpMax=1200,levelUp={level=61,gains={health=10}}}})
        assert(__xpLevels == 1 and UnitLevel('player') == 61 and PlayerLevelText:GetText() == '61')
        assert(MainMenuExpBar:GetValue() == 25 and MainMenuBarExpText:GetText() == 'XP 25 / 1200')
        __WBApplyLive({player={level=61,xp=50,xpMax=1200,levelUp={level=61,gains={health=10}}}})
        assert(__xpLevels == 1) -- durable repeated notification celebrates once
        __WBApplyLive({player={level=81,xp=10,xpMax=2000}}) -- silent load, above WoW's cap
        assert(__xpLevels == 1 and MainMenuExpBar:IsShown())
        __WBApplyLive({player={level=5,xp=0,xpMax=200}}) -- older save, no celebration
        assert(__xpLevels == 1 and MainMenuExpBar:GetValue() == 0)
        local watched = GetWatchedFactionInfo
        GetWatchedFactionInfo = function() return 'Darnassus', 5, 0, 6000, 100 end
        __WBApplyLive({player={level=60,xp=50,xpMax=100}})
        ReputationWatchBar_Update(60)
        assert(MainMenuExpBar:IsShown() and not MainMenuBarMaxLevelBar:IsShown())
        assert(ReputationWatchStatusBar:GetHeight() == 8)
        GetWatchedFactionInfo = watched
        ReputationWatchBar_Update()
        __WBApplyLive({player={level=5,xp=0,xpMax=200}})
        SHOW_NEWBIE_TIPS='0'
        this=MainMenuExpBar MainMenuExpBar:GetScript('OnLeave')()
        MainMenuExpBar:GetScript('OnEnter')() this=nil
        ExhaustionToolTipText()
        assert(not GameTooltip:IsShown()) -- no rest tooltip when newbie tips are disabled
        SHOW_NEWBIE_TIPS='1'
        SetCVar('statusBarText', '1')
    )");
    std::vector<std::string> xpMenus;
    wowcore::setHostHandler(L, [&](lua_State* state, const std::string& command) {
        if(command=="menu") xpMenus.emplace_back(lua_tostring(state,2));
        return 0;
    });
    ui.pointer(x,y);
    ui.engine().dispatchMouse(x,y,ui.height(),{true,false,false});
    ui.engine().dispatchMouse(x,y,ui.height(),{});
    CHECK(xpMenus==std::vector<std::string>{"StatsMenu"});
    ui.engine().dispatchMouse(x,y,ui.height(),{false,true,false});
    ui.engine().dispatchMouse(x,y,ui.height(),{});
    CHECK(xpMenus==std::vector<std::string>{"StatsMenu"}); // right-click must not enter native Skills
    ui.pointer(0, 360);
    ui.engine().dispatchMouse(0, 360, ui.height(), {});
    run("assert(MainMenuBarExpText:IsShown() and not GameTooltip:IsShown())");
    run("__xpReload = __WBReloadStateText()");
    lua_getglobal(L, "__xpReload");
    const std::string saved = lua_tostring(L, -1);
    lua_pop(L, 1);
    // Fresh Lua and FrameXML, matching the plugin's ReloadUI order.
    wowcore::UiSession reloaded(mpq, db, textures, 1280, 720);
    REQUIRE(reloaded.start(&err));
    REQUIRE(reloaded.loadInterface(&err));
    REQUIRE(luaL_dostring(reloaded.lua(), saved.c_str()) == 0);
    REQUIRE(luaL_dostring(reloaded.lua(), R"(
        __WBLogin()
        assert(UnitXP('player') == 0 and UnitXPMax('player') == 200)
        assert(MainMenuExpBar:IsShown() and not ExhaustionTick:IsShown())
        __WBApplyLive({player={level=60,xp=50,xpMax=100}})
        assert(MainMenuExpBar:IsShown())
        this=MainMenuExpBar MainMenuExpBar:GetScript('OnEnter')() this=nil
        ExhaustionToolTipText()
        assert(GameTooltip:NumLines() == 2)
    )") == 0);
    std::vector<std::string> reloadMenus;
    wowcore::setHostHandler(reloaded.lua(), [&](lua_State* state, const std::string& command) {
        if (command == "menu") reloadMenus.emplace_back(lua_tostring(state, 2));
        return 0;
    });
    REQUIRE(luaL_dostring(reloaded.lua(), R"(
        __WBApplyLive({player={xp=101,xpMax=100}})
        SHOW_NEWBIE_TIPS='0'
        this=MainMenuExpBar MainMenuExpBar:GetScript('OnEnter')()
        assert(GameTooltip:IsShown() and GameTooltipTextLeft1:GetText() == 'Level up available')
        assert(string.find(GameTooltipTextLeft2:GetText(),'Health, Magicka or Stamina',1,true))
        arg1='LeftButton' MainMenuExpBar:GetScript('OnMouseUp')() this=nil arg1=nil
        assert(UnitLevel('player') == 60) -- the native menu, not the Lua click, owns the advance
    )") == 0);
    CHECK(reloadMenus == std::vector<std::string>{"StatsMenu"});
    CHECK(ui.engine().lastError().empty());
    CHECK(reloaded.engine().lastError().empty());
}

TEST_CASE("authoritative spell books reconcile unknown bar and cursor ranks on every page", "[client][api][spell-feedback]") {
    const auto data = WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(std::filesystem::path(data) / "interface.MPQ")) SKIP("WoW client absent");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    REQUIRE(luaL_dofile(ui.lua(), (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    REQUIRE(luaL_dostring(ui.lua(), "__WBFixture('idle')") == 0);
    REQUIRE(ui.loadInterface(&err));
    REQUIRE(luaL_dostring(ui.lua(), "__WBLogin()") == 0);
    auto run = [&](const char* script) {
        const int rc = luaL_dostring(ui.lua(), script);
        INFO((rc ? lua_tostring(ui.lua(), -1) : ""));
        REQUIRE(rc == 0);
    };
    run(R"(
        function book(healing, starfire)
          local r={lines={['573']={name='Restoration',tab=3},['574']={name='Balance',tab=1}},
            known_spells={}, spells={}, unlearned_spells={}, highest_ranks={}}
          for _, s in ipairs({{healing,5185,573,3},{starfire,2912,574,1}}) do
            if s[1] then
              table.insert(r.known_spells,s[1])
              table.insert(r.spells,{spell=s[1],chain=s[2],skill_line=s[3],tab=s[4]})
              r.highest_ranks[tostring(s[2])]=s[1]
            end
          end
          return r
        end
        __WB.actions={[1]={spell=25297},[12]={spell=25298},[61]={spell=25297},[108]={spell=25298},
                      [2]={spell=6603},[3]={spell=-77772}}
        __WB.setCursor({spell=25297})
        -- First quiet reply: item-book top ranks have never appeared in the current book.
        __WBClassSpells(book(5185,2912))
        for _,slot in ipairs({1,61}) do assert(__WB.actions[slot].spell==5185) end
        for _,slot in ipairs({12,108}) do assert(__WB.actions[slot].spell==2912) end
        assert(__WB.cursor.spell==5185 and __WB.actions[2].spell==6603 and __WB.actions[3].spell==-77772)
        local high=book(25297,25298)
        high.replacements={{old_spell=5185,new_spell=25297},{old_spell=2912,new_spell=25298}}
        __WBClassSpells(high)
        assert(__WB.actions[1].spell==25297 and __WB.actions[108].spell==25298 and __WB.cursor.spell==25297)
        -- Level/skill decrease + wb unlearn: removed ranks need not be in unlearned_spells.
        __WBClassSpells(book(5185,2912))
        assert(__WB.actions[1].spell==5185 and __WB.actions[108].spell==2912)
        __WBClassSpells(book(nil,nil))
        assert(not __WB.actions[1] and not __WB.actions[61] and not __WB.actions[12] and not __WB.actions[108])
        assert(not CursorHasSpell() and __WB.actions[2].spell==6603 and __WB.actions[3].spell==-77772)
    )");
}

TEST_CASE("action usability follows client stance and exclusion attributes for all forms", "[client][api][spell-feedback]") {
    const auto data = WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(std::filesystem::path(data) / "interface.MPQ")) SKIP("WoW client absent");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    REQUIRE(luaL_dofile(ui.lua(), (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    REQUIRE(luaL_dostring(ui.lua(), "__WBFixture('idle')") == 0);
    REQUIRE(ui.loadInterface(&err));
    REQUIRE(luaL_dostring(ui.lua(), "__WBLogin()") == 0);
    const int rc = luaL_dostring(ui.lua(), R"(
      __WB.units[__WB.tokens.player].power=10000
      __WBSkyrimSpells({{id=-77772,name='Healing',icon='Interface\\Icons\\WB_Skyrim_012FCC',cost=1}})
      __WB.actions={[1]={spell=5185},[2]={spell=5176},[3]={spell=1082},[4]={spell=5487},[5]={spell=-77772},[6]={spell=6603}}
      for _,form in ipairs({768,783,1066,5487,9634,24858}) do
        __WB.forms={{spell=form}} __WB.formIndex=1
        assert(not IsUsableAction(1) and not IsUsableAction(5))
        assert(IsUsableAction(4) and IsUsableAction(6))
        assert((IsUsableAction(2)~=nil)==(form==24858))
        assert((IsUsableAction(3)~=nil)==(form==768))
      end
      __WB.formIndex=0
      assert(IsUsableAction(1) and IsUsableAction(2) and IsUsableAction(5) and not IsUsableAction(3))
      -- Tooltip requirements must follow the same live stance as action usability,
      -- including Dire Bear and transitions back to humanoid.
      local function requiresForm(id)
        GameTooltip:SetOwner(UIParent,'ANCHOR_NONE')
        __WB.actions[7]={spell=id}
        assert(GameTooltip:SetAction(7))
        for i=1,GameTooltip:NumLines() do
          local text=getglobal('GameTooltipTextLeft'..i):GetText() or ''
          if string.find(text,'Requires ',1,true)==1 then return true end
        end
        return false
      end
      for _,form in ipairs({0,768,5487,9634,783,1066,24858}) do
        __WB.forms={{spell=form}} __WB.formIndex=form==0 and 0 or 1
        assert(requiresForm(1082)==(form~=768))
        assert(requiresForm(6807)==(form~=5487 and form~=9634))
      end
      __WB.formIndex=0
      -- Learned forms make single-form abilities available from every other form.
      __WBDruidForms(9634,4,true)
      for _,id in ipairs({5215,6783,9913,1850,9821,1082,9850}) do
        for _,mask in ipairs({0,4,8,16,128,1073741824}) do
          assert(__WB.autoFormSpell(__WB.spellInfo(id),mask)==768)
        end
        assert(__WB.autoFormSpell(__WB.spellInfo(id),1)==nil)
      end
      for _,id in ipairs({6807,9881,779,9908,5229,5211,8983}) do
        assert(__WB.autoFormSpell(__WB.spellInfo(id),1)==9634)
      end
      assert(__WB.autoFormSpell(__WB.spellInfo(16857),0)==768)
      assert(__WB.autoFormSpell(__WB.spellInfo(16857),128)==nil)
      assert(__WB.autoFormSpell(__WB.spellInfo(5185),1)==nil)
      __WB.formIndex=0 __WB.actions[7]={spell=9913}
      assert(IsUsableAction(7))
      __WB.units[__WB.tokens.player].power=0
      local usable,oom=IsUsableAction(7) assert(not usable and oom)
      __WB.units[__WB.tokens.player].power=10000
      __WBDruidForms(5487,1)
      assert(__WB.autoFormSpell(__WB.spellInfo(6807),1)==5487)
      assert(__WB.autoFormSpell(__WB.spellInfo(16857),0)==5487)
      assert(__WB.autoFormSpell(__WB.spellInfo(9913),0)==nil)
      assert(not IsUsableAction(7))
      -- Newly learned caster spells allowed in Moonkin belong on caster bars, not bear bars.
      for _,id in ipairs({5176,9912,8921,9835,5185,25297,774,25299,2912,25298}) do
        assert(__WBClassActionSlots(__WB.spellInfo(id))[1]==1)
      end
      for _,id in ipairs({1082,9850,22568,31018}) do
        assert(__WBClassActionSlots(__WB.spellInfo(id))[1]==74)
      end
      for _,id in ipairs({6807,9881,779,9908}) do
        assert(__WBClassActionSlots(__WB.spellInfo(id))[1]==98)
      end
      assert(__WBClassActionSlots({stances=128})[1]==98)
      assert(__WBSlotOf(25299)==nil and select('#',__WBSlotOf(25299))==1)
    )");
    INFO((rc ? lua_tostring(ui.lua(), -1) : ""));
    REQUIRE(rc == 0);
    CHECK(db.spell(5185)->shapeshiftExclude == (1u << 30));
    CHECK(db.spell(5176)->shapeshiftMask == (1u << 30));
}

TEST_CASE("Skyrim quest log counts without a client cap and marks failed objectives", "[client][api][quests]") {
    const char* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env?env:WOWBRIDGE_ROOT "/../WoW/client/Data";
    if(!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq=std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq,db,textures,2560,1440);
    std::string error; REQUIRE(ui.start(&error));
    auto* L=ui.lua();
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    REQUIRE(luaL_dostring(L,"__WBFixture('idle')")==0);
    REQUIRE(ui.loadInterface(&error));
    const auto run=[&](const char* code) {
        const auto status=luaL_dostring(L,code);
        INFO((status?lua_tostring(L,-1):code)); REQUIRE(status==0);
    };
    run(R"(
      __WBLogin() __WBSkyrimMenus()
      local quests={}
      for i=1,25 do tinsert(quests,{id=i,title='Quest '..i,header=i<20 and 'Main Quest' or 'Miscellaneous',
        objectives={{text='Objective '..i,failed=i==1}}}) end
      __WBQuests(quests) ShowUIPanel(QuestLogFrame)
      assert(QuestLogQuestCount:GetText()=='25')
      assert(GetQuestLogTitle(1)=='Main Quest')
      QuestLog_SetSelection(2)
      assert(select(6,GetQuestLogTitle(2))==-1 and IsCurrentQuestFailed())
      assert(GetQuestLogLeaderBoard(1,2)=='[Failed] Objective 1')
      AddQuestWatch(2) QuestWatch_Update()
      assert(QuestWatchLine2:GetText()==' - [Failed] Objective 1')
      CollapseQuestHeader(1)
      assert(QuestLogQuestCount:GetText()=='25')
      ExpandQuestHeader(1)
      assert(QuestLogQuestCount:GetText()=='25' and QuestLogFrameAbandonButton:IsEnabled()==0)
      __questReload=__WBReloadStateText()
      __WBQuests({}) assert(QuestLogQuestCount:GetText()=='0')
    )");
    lua_getglobal(L,"__questReload"); const std::string saved=lua_tostring(L,-1); lua_pop(L,1);
    wowcore::UiSession fresh(mpq,db,textures,2560,1440);
    REQUIRE(fresh.start(&error)); REQUIRE(fresh.loadInterface(&error));
    REQUIRE(luaL_dostring(fresh.lua(),saved.c_str())==0);
    REQUIRE(luaL_dostring(fresh.lua(),"__WBLogin(); __WBSkyrimMenus(); ShowUIPanel(QuestLogFrame); assert(QuestLogQuestCount:GetText()=='25')")==0);
    CHECK(ui.engine().lastError().empty());
}

TEST_CASE("Interface diagnostic counts classic helpful and harmful auras without UnitAura", "[client][api][interface-probe]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("no client data");
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    REQUIRE(ui.loadInterface(&err));
    auto* L = ui.lua();
    const auto fixture = std::filesystem::path(WOWBRIDGE_ROOT) / "wowcore/tests/fixtures/elunara.lua";
    REQUIRE(luaL_dofile(L, fixture.string().c_str()) == 0);
    REQUIRE(luaL_dostring(L, R"(
      __WBFixture('idle')
      __WBAuraAdd('player',9885,false,1800)
      __WBAuraAdd('player',9910,false,600)
      __WBAuraAdd('player',9835,true,12)
      assert(UnitAura==nil, 'the 1.12 API must not need UnitAura')
      __probeMessages={}
      __WoweeWarn=function(message) table.insert(__probeMessages,message) end
    )") == 0);
    std::vector<std::string> errors;
    ui.engine().setLuaErrorCallback([&](const std::string& message) { errors.push_back(message); });
    ui.engine().runInterfaceProbe();
    if (!errors.empty()) INFO(errors.front());
    CHECK(errors.empty());
    CHECK(luaL_dostring(L, "assert(string.find(__probeMessages[1] or '', 'player auras=3', 1, true))") == 0);
    // Keep the original upstream API path working where that API exists.
    REQUIRE(luaL_dostring(L, R"(
      __probeMessages={}
      UnitAura=function(unit,index) if unit=='player' and index<=2 then return 'sample' end end
    )") == 0);
    errors.clear();
    ui.engine().runInterfaceProbe();
    CHECK(errors.empty());
    CHECK(luaL_dostring(L, "assert(string.find(__probeMessages[1] or '', 'player auras=2', 1, true))") == 0);
}
