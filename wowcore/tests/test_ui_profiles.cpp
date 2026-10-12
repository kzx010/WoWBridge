#include <catch_amalgamated.hpp>
#include "../../skse/src/UiProfileState.h"

TEST_CASE("new character profiles isolate caches and persist across reloads", "[persistence][profiles]")
{
    const std::string first = "character-" + std::string(32,'1');
    const std::string second = "character-" + std::string(32,'2');
    UiProfile::State a,b;
    REQUIRE(a.Load(1, first)); REQUIRE(b.Load(1, second));
    const auto root = std::filesystem::path("WTF");
    CHECK(UiProfile::Directory(root,a.key) != UiProfile::Directory(root,b.key));
    CHECK(UiProfile::Directory(root,a.key) == root / first);
    // Display names are not part of the ID: equal names and later renames do
    // not merge profiles or move a character to another cache directory.
    UiProfile::State loaded;
    REQUIRE(loaded.Load(1, a.key));
    CHECK(loaded.key == a.key);
    CHECK(UiProfile::Directory(root,loaded.key) == UiProfile::Directory(root,a.key));
    loaded.LegacyLoad(); CHECK(loaded.key == "Elunara");
    CHECK(UiProfile::Directory(root,loaded.key) == root / "Elunara");
}

TEST_CASE("profile records reject unsafe paths and incompatible versions", "[persistence][profiles]")
{
    UiProfile::State profile;
    const std::string key = "character-0123456789abcdef0123456789abcdef";
    REQUIRE(profile.Load(1,key));
    for (const auto bad : {"", ".", "..", "../Elunara", "Elunara/../other", "/tmp", "C:\\Saves",
                           "character-", "character-0123456789abcdef0123456789abcdeg"}) {
        CHECK_FALSE(profile.Load(1,bad)); CHECK(profile.key == key);
    }
    CHECK_FALSE(profile.Load(0,key)); CHECK_FALSE(profile.Load(2,key));
    CHECK_FALSE(profile.Load(1,key + "\n"));
    CHECK_FALSE(profile.Load(1,key + std::string(1,'\0')));
    CHECK(profile.key == key);
}

#include "../../skse/src/ActionState.h"
#include <limits>
extern "C" {
#include <lauxlib.h>
}

TEST_CASE("saved action bars round-trip WoW and all native FormID ranges", "[persistence][actions]") {
    auto* L=luaL_newstate(); REQUIRE(L);
    const double ids[]={6603,23227,-0x12FCC,-0xE40C3,-double(0xFE001234u),-double(0xFF001234u),-4294967295.0};
    std::string text="__WB={actions={";
    for (int i=0;i<7;++i) ActionState::Append(text,i+1,ids[i]);
    text+="}}";
    const int rc=luaL_dostring(L,text.c_str());
    INFO((rc ? lua_tostring(L,-1) : text)); REQUIRE(rc==0);
    lua_getglobal(L,"__WB"); lua_getfield(L,-1,"actions");
    for (int i=0;i<7;++i) {
        lua_rawgeti(L,-1,i+1); lua_getfield(L,-1,"spell");
        CHECK(lua_tonumber(L,-1)==ids[i]); lua_pop(L,2);
    }
    lua_close(L);
}

TEST_CASE("invalid action cache values cannot emit malformed Lua or overflow an ID", "[persistence][actions]") {
    std::string text;
    for (double id : {std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),4294967296.0,-4294967296.0,2.5})
        ActionState::Append(text,1,id);
    for (double slot : {0.0,-1.0,121.0,1.5,std::numeric_limits<double>::infinity()}) ActionState::Append(text,slot,6603);
    CHECK(text.empty());
    ActionState::Append(text,120,9885);
    CHECK(text==" [120] = { spell = 9885 },");
}

#include "../../skse/src/CarrierPersistence.h"
TEST_CASE("cold loads recover only orphaned project modifiers", "[persistence][carriers]") {
    CHECK(CarrierPersistence::MissingModifier(90,90,90,false)==-90);
    CHECK(CarrierPersistence::MissingModifier(80,60,80,false)==-60); // keeps unrelated native +20
    CHECK(CarrierPersistence::MissingModifier(80,60,20,false)==0); // Skyrim already removed it
    CHECK(CarrierPersistence::MissingModifier(80,60,80,true)==0); // native effect will be dispelled normally
    CHECK(CarrierPersistence::MissingModifier(-30,-30,-30,false)==30); // detrimental Prowl
    CHECK(CarrierPersistence::MissingModifier(26.666666f,26.666666f,26.666668f,false)==-26.666666f);
    CHECK_FALSE(CarrierPersistence::MissingModifier(80,60,85,false)); // changed native state is preserved
    CHECK_FALSE(CarrierPersistence::MissingModifier(std::numeric_limits<float>::infinity(),60,80,false));
    CHECK_FALSE(CarrierPersistence::MissingModifier(80,std::numeric_limits<float>::quiet_NaN(),80,false));
}
