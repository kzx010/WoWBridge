#include <catch_amalgamated.hpp>
#include "../../skse/src/LookRecord.h"
#include "../../skse/src/Integration.h"
#include "wowcore/character.hpp"
#include "wowcore/character.h"
#include "wowcore/dbc.hpp"
#include "wowcore/item_preview.hpp"
#include "wowcore/player_model.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/soft_raster.hpp"
#include "wowcore/ui_session.hpp"
#include "addons/lua_engine.hpp"
#include "ui/widget_tree.hpp"
extern "C" {
#include <lauxlib.h>
}
#include <cstdlib>
#include <algorithm>
#include <filesystem>

namespace {
std::filesystem::path clientData() {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    return env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
}
}

TEST_CASE("identity records preserve legacy Druids and reject illegal pairs", "[customize][identity]") {
    const wowcore::Appearance male{4,0,5,0,3,3,1}, female{4,1,5,8,5,3,0};
    for (const auto sex : {0u,1u}) {
        const auto v1 = wowcore::readIdentity(1,{sex},male,female);
        REQUIRE(v1);
        CHECK(v1->classId == 11);
        CHECK(v1->looks == (sex ? female : male));
    }
    for (const auto race : {4u,6u}) {
        const wowcore::CharacterIdentity old{{race,0,1,2,3,4,1},true,11};
        auto bytes = wowcore::writeIdentity(old);
        bytes[8] = 2; // no class field in a legacy record
        const auto migrated = wowcore::readIdentity(3,bytes,male,female);
        REQUIRE(migrated);
        auto expected = old.looks;
        CHECK(migrated->classId == 11);
        CHECK(migrated->looks == expected);
        CHECK(migrated->offer);
        CHECK(wowcore::identityVersion(old.classId)==3);
        const auto legacy = LookRecord::Write({old.looks,old.offer});
        for (size_t i=0;i<legacy.size();++i) CHECK(bytes[i]==legacy[i]);
        CHECK(LookRecord::Read(3,legacy,4,male,female)->looks==migrated->looks);
    }
    for (const auto race : {1u,3u,4u,6u}) for (const auto classId : {2u,11u}) for (const auto sex : {0u,1u}) {
        const wowcore::CharacterIdentity who{{race,sex,0,0,0,0,0},true,classId};
        CHECK(wowcore::identityVersion(classId)==(classId==11 ? 3 : 4));
        const auto back = wowcore::readIdentity(4,wowcore::writeIdentity(who),male,female);
        REQUIRE(bool(back) == wowcore::eligibleCharacter(classId,race));
        if (back) { CHECK(back->classId == classId); CHECK(back->looks == who.looks); CHECK(back->offer); }
    }
    CHECK_FALSE(wowcore::readIdentity(4,{2},male,female));
    CHECK_FALSE(wowcore::readIdentity(5,{},male,female));
    CHECK_FALSE(wowcore::readIdentity(2,wowcore::writeIdentity({female,true,11}),male,female));
    CHECK(wowcore::identitySize(1)==4);
    CHECK(wowcore::identitySize(2)==0);
    CHECK(wowcore::identitySize(3)==32);
    CHECK(wowcore::identitySize(4)==36);
}

TEST_CASE("class selection belongs to onboarding and creator Back returns to the offer", "[customize][identity]") {
    using Integration::Mode;
    CHECK(Integration::CanChooseClass(Mode::Intro,false));
    CHECK(Integration::CanChooseClass(Mode::Intro,true));
    CHECK(Integration::CanChooseClass(Mode::Skyrim,false));
    CHECK(Integration::CanChooseClass(Mode::Skyrim,true));
    CHECK(Integration::CanChooseClass(Mode::WoW,true));
    CHECK_FALSE(Integration::CanChooseClass(Mode::WoW,false));
    CHECK(Integration::ClassBack(Mode::Intro,true));
    CHECK(Integration::ClassBack(Mode::Skyrim,true));
    CHECK_FALSE(Integration::ClassBack(Mode::WoW,true));
    CHECK_FALSE(Integration::ClassBack(Mode::Intro,false));
    CHECK_FALSE(Integration::ClassBack(Mode::Skyrim,false));
    CHECK_FALSE(Integration::ClassBack(Mode::WoW,false));
}

TEST_CASE("Druid LOOK records give sex-only readers a fallback before the full identity", "[persistence][customize][identity]") {
    const wowcore::Appearance male{4,0,5,0,3,3,1}, female{4,1,5,8,5,3,0};
    for (const auto race : {4u,6u}) for (const auto sex : {0u,1u}) {
        const wowcore::CharacterIdentity who{{race,sex,1,2,3,4,1},true,11};
        std::vector<std::pair<uint32_t,wowcore::IdentityData>> records;
        REQUIRE(wowcore::writeIdentityRecords(who,[&](auto version,const auto& data,auto size) {
            CHECK(size==wowcore::identitySize(version));
            // Copy only bytes actually written to the co-save.
            wowcore::IdentityData saved{};
            std::copy_n(data.begin(),size/sizeof(uint32_t),saved.begin());
            records.emplace_back(version,saved);
            return true;
        }));
        REQUIRE(records.size()==2);
        CHECK(records[0].first==1);
        CHECK(records[1].first==3);
        std::optional<wowcore::CharacterIdentity> legacy, current;
        // main.cpp visits LOOK records in file order; master 4eb63ac accepts only v1.
        for (const auto& [version,data] : records) {
            if (version==1) legacy=wowcore::readIdentity(version,data,male,female);
            if (const auto loaded=wowcore::readIdentity(version,data,male,female)) current=loaded;
        }
        REQUIRE(legacy); REQUIRE(current);
        CHECK(legacy->looks==(sex ? female : male));
        CHECK(current->looks==who.looks);
        CHECK(current->classId==11);
        CHECK(current->offer);
    }
    unsigned count=0;
    REQUIRE(wowcore::writeIdentityRecords({{1,0,0,0,0,0,0},false,2},[&](auto version,const auto&,auto size) {
        ++count; CHECK(version==4); CHECK(size==36); return true;
    }));
    CHECK(count==1);
    CHECK_FALSE(wowcore::writeIdentityRecords({male},[](auto,const auto&,auto) { return false; }));
}

TEST_CASE("Paladin eligibility and both sexes use the 1.12 client bodies", "[client][customize][identity]") {
    if (!std::filesystem::exists(clientData()/"interface.MPQ")) SKIP("WoW client absent");
    const wowcore::Mpq mpq(clientData());
    const auto classes = wowcore::characterClasses(mpq);
    REQUIRE(classes.size()==2);
    CHECK(classes[0].id==11); CHECK(classes[0].name=="Druid"); CHECK(classes[0].token=="DRUID");
    CHECK(classes[0].races==std::vector<uint32_t>{4,6});
    CHECK(classes[1].id==2); CHECK(classes[1].name=="Paladin"); CHECK(classes[1].token=="PALADIN");
    CHECK(classes[1].races==std::vector<uint32_t>{1,3});
    const auto races=wowcore::Dbc::load(mpq,R"(DBFilesClient\ChrRaces.dbc)");
    const auto displays=wowcore::Dbc::load(mpq,R"(DBFilesClient\CreatureDisplayInfo.dbc)");
    const auto models=wowcore::Dbc::load(mpq,R"(DBFilesClient\CreatureModelData.dbc)");
    REQUIRE(races); REQUIRE(displays); REQUIRE(models);
    for (const auto race : {1u,3u}) for (const auto sex : {0u,1u}) {
        INFO("race "<<race<<" sex "<<sex);
        const wowcore::Appearance preset{race,sex,0,0,0,0,0};
        const auto choices=wowcore::customization(mpq,race,sex);
        REQUIRE(choices.allows(preset));
        const auto look=wowcore::dressCharacter(mpq,preset,{});
        REQUIRE_FALSE(look.skin.rgba.empty());
        const auto body=wowcore::loadModel(mpq,look.body);
        REQUIRE(body);
        const auto r=races->rowOf(race); REQUIRE(r);
        const auto d=displays->rowOf(races->u32(*r,4+sex)); REQUIRE(d);
        const auto m=models->rowOf(displays->u32(*d,1)); REQUIRE(m);
        CHECK(wowcore::loadModel(mpq,models->str(*m,2))->vertices.size()==body->vertices.size());
        const auto stand=wowcore::findSequence(*body,0); REQUIRE(stand>=0);
        const float scale=displays->f32(*d,4)*models->f32(*m,4);
        const float height=(body->sequences[stand].boundMax.z-body->sequences[stand].boundMin.z)*scale;
        CHECK(scale>0); CHECK(height>0);
        wowcore::PlayerModel player(mpq); player.setLook(look);
        REQUIRE(player.ready()); CHECK_FALSE(player.render(0,192).rgba.empty());
    }
}

TEST_CASE("character creation's night elf choices come from the client data", "[client][customize]") {
    if (!std::filesystem::exists(clientData() / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    const wowcore::Mpq mpq(clientData());
    for (const uint32_t sex : { 0u, 1u }) {
        INFO("sex " << sex);
        const auto c = wowcore::customization(mpq, 4, sex);
        CHECK(c.skins == 9);
        CHECK(c.faces == std::vector<uint32_t>(9, 9));
        CHECK(c.hairStyles == 7);
        CHECK(c.hairColors == std::vector<uint32_t>(7, 8));
        CHECK(c.facialHairs == (sex ? 10u : 6u));            // markings; beards and sideburns
        CHECK(c.hair == "NORMAL");
        CHECK(c.facialHair == (sex ? "MARKINGS" : "NORMAL"));   // GlueStrings: "Markings", "Facial Hair"
    }
    const auto female = wowcore::customization(mpq, 4, 1), male = wowcore::customization(mpq, 4, 0);
    CHECK(female.allows({ 4, 1, 5, 8, 5, 3, 0 }));    // the packaged looks (build/character.json, data/character-male.json)
    CHECK(male.allows({ 4, 0, 5, 0, 3, 3, 1 }));
    CHECK(female.allows({ 4, 1, 8, 8, 6, 7, 9 }));
    CHECK_FALSE(male.allows({ 4, 0, 8, 8, 6, 7, 9 }));   // a male night elf has six facial hair styles
    CHECK_FALSE(female.allows({ 4, 1, 9, 0, 0, 0, 0 }));
    CHECK_FALSE(female.allows({ 4, 1, 0, 0, 7, 0, 0 }));
    CHECK(wowcore::customization(mpq, 6, 1).hair == "HORNS");   // other races keep their own words
}

TEST_CASE("Tauren creation and equipment use the 1.12 client variants", "[client][customize][character]") {
    if (!std::filesystem::exists(clientData() / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    const wowcore::Mpq mpq(clientData());
    for (const auto sex : {0u,1u}) {
        const auto c = wowcore::customization(mpq,6,sex);
        CHECK(c.skins == (sex ? 11u : 19u));
        CHECK(c.faces == std::vector<uint32_t>(c.skins,sex ? 4 : 5));
        CHECK(c.hairStyles == (sex ? 7u : 8u));
        CHECK(c.hairColors == std::vector<uint32_t>(c.hairStyles,3));
        CHECK(c.facialHairs == (sex ? 5u : 7u));
        CHECK(c.hair == "HORNS");
        CHECK(c.facialHair == (sex ? "HAIR" : "NORMAL"));
        const wowcore::Appearance who{6,sex,0,0,0,0,1};
        REQUIRE(c.allows(who));
        auto last=who; last.skin=c.skins-1; last.face=c.faces.back()-1; last.hairStyle=c.hairStyles-1;
        last.hairColor=2; last.facialHair=c.facialHairs-1;
        CHECK(c.allows(last)); ++last.skin; CHECK_FALSE(c.allows(last));
        const auto naked=wowcore::dressCharacter(mpq,who,{});
        CHECK(naked.body == std::string("Character\\Tauren\\") + (sex ? "Female\\TaurenFemale.m2" : "Male\\TaurenMale.m2"));
        REQUIRE(naked.replaceableTextures.contains(8));
        REQUIRE_FALSE(wowcore::loadImage(mpq,naked.replaceableTextures.at(8)).rgba.empty());
        if (sex) CHECK(naked.geosets.contains(202));   // female Hair uses the sixth DBC geoset
        const auto dressed=wowcore::dressCharacter(mpq,who,{{0,33655},{4,30536},{14,23019}});
        REQUIRE_FALSE(dressed.attached.empty());
        CHECK(dressed.attached.front().model.ends_with(sex ? "_TaF.m2" : "_TaM.m2"));
        REQUIRE(wowcore::loadModel(mpq,dressed.attached.front().model));
        const auto helmet=wowcore::loadItemPreview(mpq,16900,{{16900,{33655,1}}},who);
        CHECK(helmet.status == wowcore::ItemPreviewStatus::Ready);
        const auto chest=wowcore::loadArmorItemPreview(mpq,16897,{{16897,{30536,20}}},who);
        CHECK(chest.status == wowcore::ItemPreviewStatus::Ready);
        wowcore::PlayerModel player(mpq);
        player.setLook(dressed);
        CHECK(player.ready());
        CHECK_FALSE(player.render(0,192).rgba.empty());
    }
    for (const auto display : {8571u,2289u,15375u,632u,2428u}) {
        const auto look=wowcore::dressCreature(mpq,display);
        REQUIRE(look); REQUIRE(wowcore::loadModel(mpq,look->body));
    }
}

TEST_CASE("the co-save keeps her whole look and still reads a sex-only record", "[persistence][customize]") {
    const wowcore::Appearance male{ 4, 0, 5, 0, 3, 3, 1 }, female{ 4, 1, 5, 8, 5, 3, 0 }, mine{ 4, 1, 2, 6, 1, 7, 9 };
    const auto data = LookRecord::Write({ mine, true });
    CHECK(sizeof(data) == LookRecord::Size(3));
    CHECK(LookRecord::Size(2) == 28);
    const auto back = LookRecord::Read(2, data, 4, male, female);
    REQUIRE(back);
    CHECK(back->looks == mine);
    CHECK(back->offer);
    CHECK_FALSE(LookRecord::Read(2, LookRecord::Write({ male }), 4, male, female)->offer);
    CHECK(LookRecord::Size(1) == 4);
    CHECK(LookRecord::Read(1, { 0 }, 4, male, female)->looks == male);   // v1: the sex's packaged look
    CHECK(LookRecord::Read(1, { 1 }, 4, male, female)->looks == female);
    CHECK_FALSE(LookRecord::Read(1, { 1 }, 4, male, female)->offer);
    CHECK_FALSE(LookRecord::Read(1, { 2 }, 4, male, female));
    const wowcore::Appearance tauren{6,0,0,0,0,0,1};
    CHECK(LookRecord::Read(3, LookRecord::Write({tauren,true}),4,male,female)->looks == tauren);
    CHECK(LookRecord::Read(2, LookRecord::Write({tauren}),4,male,female)->looks.race == 4);
    CHECK_FALSE(LookRecord::Read(4,data,4,male,female));
    CHECK(LookRecord::Size(4) == 0);
}

TEST_CASE("the customization window chooses within the client's ranges and answers the host", "[client][api][customize]") {
    if (!std::filesystem::exists(clientData() / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    auto mpq = std::make_shared<wowcore::Mpq>(clientData());
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 1280, 720);
    std::string err;
    REQUIRE(ui.start(&err));
    auto* L = ui.lua();
    const auto run = [&](const char* script) {
        const int rc = luaL_dostring(L, script);
        INFO(script);
        INFO((rc ? lua_tostring(L, -1) : ""));
        REQUIRE(rc == 0);
    };
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    REQUIRE(luaL_dofile(L, (std::string(WOWBRIDGE_ROOT) + "/wowcore/tests/fixtures/hud_states.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run(R"(
      __WBLogin() __WBCharacterFixture()
      calls={} __WB.host=function(...) if arg[1]=='appearance' then tinsert(calls,arg) end end
      OPTIONS={'skin','face','hairStyle','hairColor','facialHair'}
      male={race=4,sex=0,skin=5,face=0,hairStyle=3,hairColor=3,facialHair=1}
      female={race=4,sex=1,skin=5,face=8,hairStyle=5,hairColor=3,facialHair=0}
      tmale={race=6,sex=0,skin=0,face=0,hairStyle=0,hairColor=0,facialHair=1}
      tfemale={race=6,sex=1,skin=0,face=0,hairStyle=0,hairColor=0,facialHair=1}
      defaults={[4]={[0]=male,[1]=female},[6]={[0]=tmale,[1]=tfemale}}
      function open(look) calls={} __WBAppearance(look,defaults) __WBCustomizeAppearance() end
      -- Accept, the look it told the host; the window opens again on it, as the host would publish it.
      function accept()
        calls={} WBCustomizeAccept:Click()
        local c=calls[1] assert(table.getn(calls)==1 and not WBCustomizeFrame:IsShown())
        local look={race=c[2],sex=c[3],skin=c[4],face=c[5],hairStyle=c[6],hairColor=c[7],facialHair=c[8]}
        open(look) return look
      end
      function choices(look,i)
        local r=__WBCustomization(look.race,look.sex)
        return i==2 and r.face[look.skin] or i==4 and r.hairColor[look.hairStyle] or r[OPTIONS[i]]
      end
      open(female)
      assert(WBCustomizeFrame:IsShown() and WBCustomizeModel:GetModel()=='__customize')
      assert(WBCustomizeOption1Text:GetText()=='Skin Color' and WBCustomizeOption2Text:GetText()=='Face')
      assert(WBCustomizeOption3Text:GetText()=='Hair Style' and WBCustomizeOption4Text:GetText()=='Hair Color')
      assert(WBCustomizeOption5Text:GetText()=='Markings' and WBCustomizeOption5:IsShown())
      assert(WBCustomizeGenderFemale:GetChecked() and not WBCustomizeGenderMale:GetChecked())
      assert(WBCustomizeGenderFemaleHighlightText:GetText()=='Female' and WBCustomizeGenderMaleHighlightText:GetText()=='')
      assert(WBCustomizeAccept:GetText()=='Accept' and WBCustomizeCancel:GetText()=='Cancel')
      assert(WBCustomizeRandomize:GetText()=='Randomize')
      local _,height=WBCustomizeAccept:GetFontString():GetFont() assert(GlueFontNormal and height==15)   -- the glue font
    )");
    // The arrows wrap within each sex's ranges, from the client data: round to where they started, and down past 0.
    run(R"(
      for _,start in ipairs({female,male}) do
        open(start)
        local counts={}
        for i=1,5 do
          local look=accept() local n=choices(look,i) counts[i]=n
          for k=1,n do getglobal('WBCustomizeOption'..i..'RightButton'):Click() end
          assert(accept()[OPTIONS[i]]==look[OPTIONS[i]],OPTIONS[i]..' did not come round')
          for k=0,look[OPTIONS[i]] do getglobal('WBCustomizeOption'..i..'LeftButton'):Click() end
          local wrapped=accept() assert(wrapped[OPTIONS[i]]==n-1,OPTIONS[i]..' did not wrap below 0')
          for _,o in ipairs(OPTIONS) do if o~=OPTIONS[i] then assert(wrapped[o]==look[o]) end end
        end
        assert(counts[1]==9 and counts[2]==9 and counts[3]==7 and counts[4]==8 and counts[5]==(start.sex==1 and 10 or 6))
      end
    )");
    // A change of sex starts from that sex's packaged look and its words, and keeps each sex's work while open.
    run(R"(
      open(female) WBCustomizeOption1RightButton:Click()
      WBCustomizeGenderMale:Click()
      assert(WBCustomizeGenderMale:GetChecked() and not WBCustomizeGenderFemale:GetChecked())
      assert(WBCustomizeOption5Text:GetText()=='Facial Hair' and WBCustomizeGenderMaleHighlightText:GetText()=='Male')
      local look=accept()
      for k,v in pairs(male) do assert(look[k]==v,k) end
      WBCustomizeOption5RightButton:Click() WBCustomizeGenderFemale:Click() WBCustomizeGenderMale:Click()
      assert(accept().facialHair==2)
      WBCustomizeGenderFemale:Click() WBCustomizeOption1LeftButton:Click() WBCustomizeGenderMale:Click() WBCustomizeGenderFemale:Click()
      look=accept() assert(look.sex==1 and look.skin==4 and look.face==8 and look.hairStyle==5)
    )");
    // Client race buttons, labels, packaged resets, and General-book isolation on acceptance.
    run(R"(
      open(female)
      assert(WBCustomizeRace4:IsEnabled()==1 and WBCustomizeRace6:IsEnabled()==1)
      for _,race in ipairs({1,2,3,5,7,8}) do local b=getglobal('WBCustomizeRace'..race) assert(b:IsEnabled()==0 and b:GetDisabledTexture()) end
      WBCustomizeRace6:Click()
      assert(WBCustomizeRace6:GetChecked() and not WBCustomizeRace4:GetChecked())
      assert(WBCustomizeRace6HighlightText:GetText()=='Tauren')
      assert(WBCustomizeOption3Text:GetText()=='Horn Style' and WBCustomizeOption4Text:GetText()=='Horn Color')
      assert(WBCustomizeOption5Text:GetText()=='Hair')
      local look=accept() for k,v in pairs(tfemale) do assert(look[k]==v,k) end
      assert(UnitRace('player')=='Tauren' and UnitSex('player')==3)
      local seen={} for _,id in ipairs(__WB.spellbook) do seen[id]=true end
      assert(seen[20549] and seen[20550] and seen[20551] and seen[20552] and not seen[20580] and not seen[20582])
      assert(not __WB.spellInfo(20549).passive and __WB.spellInfo(20550).passive)
      WBCustomizeGenderMale:Click()
      assert(WBCustomizeOption5Text:GetText()=='Facial Hair')
      look=accept() for k,v in pairs(tmale) do assert(look[k]==v,k) end
      WBCustomizeOption1RightButton:Click() WBCustomizeRace4:Click() WBCustomizeRace6:Click()
      look=accept() for k,v in pairs(tmale) do assert(look[k]==v,k) end
      WBCustomizeRace1:Click() assert(accept().race==6)
      for _,start in ipairs({tmale,tfemale}) do
        open(start)
        for i=1,5 do
          local look=accept() local n=choices(look,i)
          for k=1,n do getglobal('WBCustomizeOption'..i..'RightButton'):Click() end
          assert(accept()[OPTIONS[i]]==look[OPTIONS[i]])
        end
        for k=1,30 do
          WBCustomizeRandomize:Click() local look=accept()
          assert(look.race==6 and look.sex==start.sex)
          for i=1,5 do assert(look[OPTIONS[i]]<choices(look,i)) end
        end
      end
      open(female)
    )");
    // Randomize stays within the ranges and keeps their sex.
    run(R"(
      for _,start in ipairs({female,male}) do
        open(start)
        local seen={}
        for k=1,60 do
          WBCustomizeRandomize:Click()
          local look=accept() assert(look.sex==start.sex)
          for i=1,5 do
            local v=look[OPTIONS[i]] assert(v>=0 and v<choices(look,i),OPTIONS[i]..' '..v..' out of range')
            seen[OPTIONS[i]..v]=true
          end
        end
        assert(seen.skin0 or seen.skin1 or seen.skin2) assert(seen.hairStyle6 or seen.hairStyle5 or seen.hairStyle4)
      end
    )");
    // Cancel and Escape keep their look and tell the host so; the host's own close owes it nothing.
    run(R"(
      open(female)
      WBCustomizeOption1RightButton:Click() WBCustomizeOption3RightButton:Click() WBCustomizeGenderMale:Click()
      WBCustomizeCancel:Click()
      assert(not WBCustomizeFrame:IsShown() and table.getn(calls)==1 and calls[1].n==1 and calls[1][1]=='appearance')
      open(female) local look=accept()
      for k,v in pairs(female) do assert(look[k]==v,k) end
    )");
    textures.sync(ui.frame(.016f));
    run(R"(
      WBCustomizeOption2RightButton:Click() calls={}
      assert(__WBEscapeHasWork()==1) assert(CloseWindows())   -- what Escape runs (ToggleGameMenu)
      assert(not WBCustomizeFrame:IsShown() and table.getn(calls)==0)
    )");
    textures.sync(ui.frame(.016f));   // the interface runs OnHide on its next frame
    run(R"(
      assert(table.getn(calls)==1 and calls[1].n==1)
      open(female) WBCustomizeOption2RightButton:Click() __WBCustomizeClose()
    )");
    textures.sync(ui.frame(.016f));
    run(R"(
      assert(not WBCustomizeFrame:IsShown() and table.getn(calls)==0)
      open(female) accept() __WBCustomizeClose() open(female)
    )");
    textures.sync(ui.frame(.016f));
    run("assert(WBCustomizeFrame:IsShown() and table.getn(calls)==0)");   // reopened before its OnHide ran: nothing owed
    // Layout: every control inside the window, none over another (the rotation buttons by their hit areas, as the
    // screen overlaps their art), labels inside their rows' label frames and buttons.
    for (int i = 0; i < 3; ++i) textures.sync(ui.frame(.016f));
    run(R"(
      local fx,fy,fw,fh=WBCustomizeFrame:GetRect()
      local nodes={}
      local function add(f,l,r,t,b)
        assert(f:IsVisible(),f:GetName()..' hidden')
        local x,y,w,h=f:GetRect() x,y,w,h=x+(l or 0),y+(b or 0),w-(l or 0)-(r or 0),h-(t or 0)-(b or 0)
        assert(x>=fx-.1 and y>=fy-.1 and x+w<=fx+fw+.1 and y+h<=fy+fh+.1,f:GetName()..' outside the window')
        tinsert(nodes,{f=f,x=x,y=y,w=w,h=h})
      end
      for _,f in ipairs({WBCustomizeGenderMale,WBCustomizeGenderFemale,WBCustomizeRandomize,WBCustomizeAccept,WBCustomizeCancel}) do add(f) end
      for _,race in ipairs({1,3,7,4,2,5,8,6}) do add(getglobal('WBCustomizeRace'..race)) end
      add(WBCustomizeRotateLeft,13,13,10,13) add(WBCustomizeRotateRight,13,13,10,13)
      for i=1,5 do
        local row=getglobal('WBCustomizeOption'..i) add(row)
        local text=getglobal(row:GetName()..'Text')
        assert(text:GetStringWidth()>0 and text:GetLeft()>=row:GetLeft()-5 and text:GetRight()<=getglobal(row:GetName()..'LeftButton'):GetLeft()+3,row:GetName()..' label under its arrows')
      end
      for _,b in ipairs({WBCustomizeRandomize,WBCustomizeAccept,WBCustomizeCancel}) do
        local fs=b:GetFontString() assert(fs:GetLeft()>=b:GetLeft() and fs:GetRight()<=b:GetRight(),b:GetName()..' label outside')
      end
      local cx,cy,cw,ch=WBCustomizeConfiguration:GetRect()
      for _,f in ipairs({WBCustomizeGenderMale,WBCustomizeGenderFemale,WBCustomizeRandomize,WBCustomizeOption1,WBCustomizeOption5}) do
        assert(f:GetLeft()>=cx-9 and f:GetRight()<=cx+cw+9 and f:GetBottom()>=cy and f:GetTop()<=WBCustomizeLogo:GetBottom()+10,f:GetName()..' outside the column')
      end
      assert(WBCustomizeGenderMale:GetTop()<WBCustomizeRace4:GetBottom(),'gender buttons below race choices')
      for i,a in ipairs(nodes) do for j=i+1,table.getn(nodes) do local b=nodes[j]
        local overlapX=math.min(a.x+a.w,b.x+b.w)-math.max(a.x,b.x)
        local overlapY=math.min(a.y+a.h,b.y+b.h)-math.max(a.y,b.y)
        assert(overlapX<=.1 or overlapY<=.1,a.f:GetName()..' overlaps '..b.f:GetName())
      end end
      for _,f in ipairs({WBCustomizeRotateLeft,WBCustomizeRotateRight,WBCustomizeAccept,WBCustomizeCancel}) do
        assert(f:GetFrameLevel()>WBCustomizeModel:GetFrameLevel(),f:GetName()..' under their figure')
      end
    )");
    // Their figure shows each change at once (its frame's pixels).
    const auto figure = [&] {
        ImDrawData* draw = nullptr;
        for (int i = 0; i < 10; ++i) textures.sync(draw = ui.frame(.016f));   // the art loads as it is first drawn
        wowcore::Image image(1280, 720);
        wowcore::rasterize(draw, textures, image);
        const auto* w = ui.engine().widgets().findByName("WBCustomizeModel");
        REQUIRE(w);
        REQUIRE(w->hostModel);
        // Their middle, clear of the buttons over the scene's corners.
        const float scale = ui.engine().widgets().uiScale();
        const int x0 = int((w->left + w->rectW * 0.3f) * scale), x1 = int((w->left + w->rectW * 0.6f) * scale);
        const int y0 = int(720 - (w->bottom + w->rectH) * scale), y1 = int(720 - w->bottom * scale);
        REQUIRE(x0 >= 0); REQUIRE(y0 >= 0); REQUIRE(x1 <= 1280); REQUIRE(y1 <= 720); REQUIRE(x1 - x0 > 100);
        std::vector<uint8_t> pixels;
        size_t covered = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const uint8_t* p = &image.rgba[(size_t(y) * 1280 + x) * 4];
                pixels.insert(pixels.end(), p, p + 4);
                covered += p[0] + p[1] + p[2] > 120;
            }
        CHECK(covered > 2000);   // their figure, not only the panel's dark backdrop
        return pixels;
    };
    const auto before = figure();
    run("WBCustomizeOption1RightButton:Click() WBCustomizeOption1RightButton:Click()");
    const auto skin = figure();
    run("WBCustomizeOption3RightButton:Click()");
    const auto hair = figure();
    const bool skinShown = before != skin, hairShown = skin != hair;
    CHECK(skinShown);
    CHECK(hairShown);
    run("WBCustomizeCancel:Click() open(female)");
    const bool restored = figure() == before;   // Cancel left the player as the player was
    CHECK(restored);
    CHECK(ui.engine().lastError().empty());
}
