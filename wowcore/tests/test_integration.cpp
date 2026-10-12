#include <catch_amalgamated.hpp>
#include "../../skse/src/IntegrationState.h"
#include "wowcore/ui_session.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/soft_raster.hpp"
#include "addons/lua_engine.hpp"
extern "C" {
#include <lauxlib.h>
}
#include <cstdlib>
#include <filesystem>

TEST_CASE("new-game integration waits for the creator, a choice and unlocked controls", "[integration]")
{
    Integration::State s;
    CHECK(s.ShouldActivate(true, false, true, true)); // old saves and coc
    s.NewGame();
    CHECK_FALSE(s.Offer(false));
    CHECK_FALSE(s.ShouldActivate(true, false, true, true));
    REQUIRE(s.Offer(true));
    CHECK_FALSE(s.Offer(true)); // one automatic popup
    CHECK_FALSE(s.Finish());
    REQUIRE(s.ChooseSex(0));
    CHECK_FALSE(s.ChooseSex(2));
    CHECK_FALSE(s.Offer(true, true)); // double-click/start cannot replace a pending setup
    REQUIRE(s.Finish());
    CHECK(s.mode == Integration::Mode::WoW);
    CHECK_FALSE(s.ShouldActivate(true, true, true, true));
    CHECK_FALSE(s.ShouldActivate(true, false, false, true));
    CHECK_FALSE(s.ShouldActivate(true, false, true, false));
    CHECK_FALSE(s.ShouldActivate(false, false, true, true));
    CHECK(s.ShouldActivate(true, false, true, true));
}

TEST_CASE("native choice, legacy loads and repeated new games do not share setup state", "[integration]")
{
    Integration::State s;
    s.NewGame(); REQUIRE(s.Offer(true)); s.ChooseSkyrim();
    CHECK_FALSE(s.Offer(true));
    CHECK_FALSE(s.ShouldActivate(true, false, true, true));
    REQUIRE(s.Offer(false, true)); // wb start after finishing native creation
    REQUIRE(s.ChooseSex(1)); REQUIRE(s.Finish());
    s.Load(Integration::Mode::Skyrim);
    CHECK_FALSE(s.selectedSex);
    CHECK_FALSE(s.Offer(true));
    s.Load(); CHECK(s.ShouldActivate(true, false, true, true));
    s.NewGame(); REQUIRE(s.Offer(true));
    REQUIRE(s.ChooseSex(0));
    s.NewGame(); CHECK_FALSE(s.selectedSex); REQUIRE(s.Offer(true));
    s.Load(Integration::Mode::Intro); REQUIRE(s.Offer(true)); // save during intro
    CHECK(Integration::State::Valid(0)); CHECK(Integration::State::Valid(2));
    CHECK_FALSE(Integration::State::Valid(3)); CHECK_FALSE(Integration::State::Valid(0xffffffff));
}

TEST_CASE("display names retain player spelling and use Dragonborn for unnamed players", "[integration]")
{
    CHECK(Integration::DisplayName("") == "Dragonborn");
    CHECK(Integration::DisplayName(" \t\r\n") == "Dragonborn");
    CHECK(Integration::DisplayName("Prisoner") == "Dragonborn");
    CHECK(Integration::DisplayName("Player") == "Dragonborn");
    CHECK(Integration::DisplayName(" Dragonborn ") == "Dragonborn");
    CHECK(Integration::DisplayName("  Aria's Druid  ") == "Aria's Druid");
    CHECK(Integration::DisplayName("Éowyn") == "Éowyn");
}

TEST_CASE("the real WoW player, self target and character pane follow Skyrim name changes", "[client][api][integration]")
{
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
    const auto script = R"(
        __WBLogin() __WBSkyrimMenus()
        __WBApplyLive({player={name="Aria's Druid"}})
        assert(UnitName('player')=="Aria's Druid" and PlayerName:GetText()=="Aria's Druid")
        ToggleCharacter('PaperDollFrame')
        assert(CharacterNameText:GetText()=="Aria's Druid")
        __WBApplyLive({target='player'})
        assert(UnitName('target')=="Aria's Druid")
        __WBApplyLive({player={name='Éowyn'}})
        assert(UnitName('player')=='Éowyn' and UnitName('target')=='Éowyn')
        assert(PlayerName:GetText()=='Éowyn' and TargetName:GetText()=='Éowyn')
        assert(CharacterNameText:GetText()=='Éowyn')
        GameTooltip:SetOwner(UIParent, 'ANCHOR_NONE') GameTooltip:SetUnit('player')
        assert(GameTooltipTextLeft1:GetText()=='Éowyn')
        __WBApplyLive({target={guid=12,name='Wolf',health=40,healthMax=40}})
        __WBApplyLive({player={name='Dragonborn'}})
        assert(UnitName('target')=='Wolf' and TargetName:GetText()=='Wolf')
        assert(UnitName('player')=='Dragonborn')
        __WBApplyLive({player={health=30}})
        assert(UnitName('player')=='Dragonborn')
    )";
    const auto rc = luaL_dostring(L, script);
    INFO((rc ? lua_tostring(L, -1) : ""));
    REQUIRE(rc == 0);
}
