#include <catch_amalgamated.hpp>
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/soft_raster.hpp"
#include "wowcore/ui_session.hpp"
extern "C" {
#include <lauxlib.h>
}
#include <cstdlib>
#include <filesystem>

// The client's menus in Skyrim (__WBSkyrimMenus): the main menu's rows that do something here, Interface Options less
// what has nothing behind it, Show Helm / Show Cloak kept for their model, and the key bindings Skyrim has no use for.
TEST_CASE("Skyrim keeps the client's menus with only what works here", "[client][api][menus]") {
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
        INFO((rc ? lua_tostring(L,-1) : ""));
        REQUIRE(rc==0);
    };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run(R"(
      __WBLogin()
      -- The reference keeps the client's list; in game the menus follow, and their helm and cloak reach the host.
      assert(GetBindingAction('R')=='REPLY' and GetBindingAction('F5')=='TARGETPARTYMEMBER4')
      calls={} __WBHost=function(...) tinsert(calls,arg) end
      local function sent(name) local last for _,c in ipairs(calls) do if c[1]==name then last=c end end return last end
      __WBSkyrimMenus()
      local gear=sent('showGear') assert(gear and gear[2]==1 and gear[3]==1)
      sentCommand=sent
    )");

    // The main menu: Skyrim Options, Interface Options, Key Bindings, Exit Game, then Return to Game, in the client's
    // template and spacing, three rows shorter.
    run(R"(
      ShowUIPanel(GameMenuFrame)
      for _,b in ipairs({GameMenuButtonSoundOptions,GameMenuButtonMacros,GameMenuButtonLogout,HelpMicroButton}) do
        b:Show() assert(not b:IsShown())
      end
      for _,b in ipairs({GameMenuButtonOptions,GameMenuButtonUIOptions,GameMenuButtonKeybindings,GameMenuButtonQuit,
          GameMenuButtonContinue}) do assert(b:IsVisible()) end
      assert(GameMenuButtonOptions:GetText()=='Skyrim Options' and GameMenuButtonUIOptions:GetText()==UIOPTIONS_MENU)
      assert(GameMenuButtonKeybindings:GetText()==KEY_BINDINGS and GameMenuButtonQuit:GetText()==EXIT_GAME)
      assert(GameMenuButtonContinue:GetText()==RETURN_TO_GAME and GameMenuFrame:GetHeight()==180)
    )");
    ui.frame(0.1f);   // laid out
    run(R"(
      local function near(a,b) return math.abs(a-b)<0.01 or error(a..' ~= '..b,2) end
      assert(near(GameMenuButtonUIOptions:GetTop(),GameMenuButtonOptions:GetBottom()-1))
      assert(near(GameMenuButtonKeybindings:GetTop(),GameMenuButtonUIOptions:GetBottom()-1))
      assert(near(GameMenuButtonQuit:GetTop(),GameMenuButtonKeybindings:GetBottom()-1))
      assert(near(GameMenuButtonContinue:GetTop(),GameMenuButtonQuit:GetBottom()-16))
      -- The client's margins: the first row 37 below the top, 29.5 below Return to Game.
      assert(near(GameMenuFrame:GetTop()-37,(GameMenuButtonOptions:GetTop()+GameMenuButtonOptions:GetBottom())/2))
      assert(near(GameMenuButtonContinue:GetBottom()-GameMenuFrame:GetBottom(),29.5))
      calls={} GameMenuButtonOptions:Click()
      local menu=sentCommand('menu') assert(menu and menu[2]=='System' and not GameMenuFrame:IsShown())
      ShowUIPanel(GameMenuFrame) calls={} GameMenuButtonQuit:Click()
      menu=sentCommand('menu') assert(sentCommand('quit') and menu and menu[2]=='System' and not GameMenuFrame:IsShown())
      ShowUIPanel(GameMenuFrame) GameMenuButtonKeybindings:Click() assert(KeyBindingFrame:IsShown())
      HideUIPanel(KeyBindingFrame) ShowUIPanel(GameMenuFrame)
      GameMenuButtonContinue:Click() assert(not GameMenuFrame:IsShown())
      -- The Socials button opens the testing tools, and its tooltip says so with its key, after a bindings change too.
      local tip='Testing Tools '..NORMAL_FONT_COLOR_CODE..'(O)'..FONT_COLOR_CODE_CLOSE
      assert(SocialsMicroButton.tooltipText==tip)
      __WBFire('UPDATE_BINDINGS') assert(SocialsMicroButton.tooltipText==tip)
    )");

    // Interface Options: what has no effect here is hidden, the rest closes up in the client's columns; Okay and
    // Defaults leave the hidden settings alone.
    run(R"(
      ShowUIPanel(GameMenuFrame) GameMenuButtonUIOptions:Click()
      assert(UIOptionsFrame:IsShown() and BasicOptions:IsVisible())
    )");
    ui.frame(0.1f);
    run(R"(
      local function visible(list) for _,n in ipairs(list) do
        assert(getglobal('UIOptionsFrameCheckButton'..n):IsVisible(),'option '..n) end end
      local function gone(list) for _,n in ipairs(list) do
        assert(not getglobal('UIOptionsFrameCheckButton'..n):IsVisible(),'option '..n) end end
      visible({70,71,12,2,31,20,43,39,29})
      gone({1,3,5,6,14,16,21,22,23,28,30,42,44,45,61,62,66,67,24,26,27,15})
      assert(not BasicOptionsCamera:IsVisible() and not UIOptionsFrameSlider1:IsVisible())
      assert(not UIOptionsFrameClickCameraDropDown:IsVisible() and not UIOptionsFrameResetTutorials:IsVisible())
      -- WoW's tutorials are cleared; Area Spells Hit Friendly is off by default and reaches the host when ticked.
      assert(__WBTutorialFlagged(1) and UIOptionsFrameCheckButton71Text:GetText()==WB_AREA_FRIENDLY_TEXT)
      assert(GetCVar('wbAreaFriendly')=='0')
      local told,host=nil,__WBHost __WBHost=function(...) if arg[1]=='areaFriendly' then told=arg[2] end end
      SetCVar('wbAreaFriendly',1) assert(told==1) SetCVar('wbAreaFriendly',0) assert(told==0) __WBHost=host
      assert(math.abs(UIOptionsFrameCheckButton71:GetTop()-UIOptionsFrameCheckButton70:GetTop())<0.01)
      assert(math.abs(UIOptionsFrameCheckButton29:GetTop()-(BasicOptionsHelp:GetTop()-8))<0.01)
      assert(UIOptionsFrameCheckButton31Text:GetText()==SHOW_CLOAK and UIOptionsFrameCheckButton20Text:GetText()==SHOW_HELM)
      assert(BasicOptionsGeneral:GetHeight()==42 and BasicOptionsDisplay:GetHeight()==68 and BasicOptionsHelp:GetHeight()==42)
      local function near(a,b) return math.abs(a-b)<0.01 or error(a..' ~= '..b,2) end
      assert(near(BasicOptionsHelp:GetTop(),BasicOptionsDisplay:GetBottom()-20))
      assert(near(UIOptionsFrameCheckButton70:GetTop(),BasicOptionsGeneral:GetTop()-8))
      assert(near(UIOptionsFrameCheckButton31:GetTop(),UIOptionsFrameCheckButton12:GetTop()))
      assert(near(UIOptionsFrameCheckButton39:GetTop(),UIOptionsFrameCheckButton12:GetTop()))
      assert(near(UIOptionsFrameCheckButton43:GetTop(),UIOptionsFrameCheckButton2:GetTop()))
      UIOptionsFrameTab2:Click() assert(AdvancedOptions:IsVisible())
    )");
    ui.frame(0.1f);
    run(R"(
      local function near(a,b) return math.abs(a-b)<0.01 or error(a..' ~= '..b,2) end
      local function visible(list) for _,n in ipairs(list) do
        assert(getglobal('UIOptionsFrameCheckButton'..n):IsVisible(),'option '..n) end end
      local function gone(list) for _,n in ipairs(list) do
        assert(not getglobal('UIOptionsFrameCheckButton'..n):IsVisible(),'option '..n) end end
      visible({32,33,34,35,36,40,7,8,18,37})
      gone({13,38,46,51,68,47,41,48,49,50,52,19,11,9})
      assert(not AdvancedOptionsRaid:IsVisible() and not AdvancedOptionsCombatText:IsVisible())
      assert(AdvancedOptionsChat:GetHeight()==68 and near(AdvancedOptionsChat:GetWidth(),AdvancedOptionsActionBars:GetWidth()))
      assert(near(UIOptionsFrameCheckButton18:GetTop(),UIOptionsFrameCheckButton7:GetTop()))
      assert(near(UIOptionsFrameCheckButton18:GetLeft(),UIOptionsFrameCheckButton40:GetLeft()))
      UIOptionsFrameTab1:Click()
      local n=0
      for _,info in pairs(UIOptionsFrameCheckButtons) do
        n=n+1 assert(getglobal('UIOptionsFrameCheckButton'..info.index):IsShown(),'listed '..info.index)
      end
      assert(n==20 and next(UIOptionsFrameSliders)==nil)   -- Area Spells Hit Friendly, Skyrim Minimap Icons
      SetCVar('mouseInvertPitch','1') SetCVar('autoSelfCast','1') SetCVar('deselectOnClick','0')
      UIOptionsFrameDefaults:Click() UIOptionsFrameOkay:Click()
      assert(not UIOptionsFrame:IsShown())
      assert(GetCVar('mouseInvertPitch')=='1' and GetCVar('autoSelfCast')=='1' and GetCVar('deselectOnClick')=='0')
    )");

    // Show Helm / Show Cloak: the panel's checkboxes, kept with their character's saved variables, told to the host on
    // a change only.
    run(R"(
      assert(ShowingHelm()==1 and ShowingCloak()==1)
      HideUIPanel(GameMenuFrame) ShowUIPanel(UIOptionsFrame)
      assert(UIOptionsFrameCheckButton20:GetChecked() and UIOptionsFrameCheckButton31:GetChecked())
      UIOptionsFrameCheckButton20:SetChecked(nil)
      calls={} UIOptionsFrameOkay:Click()
      local gear=sentCommand('showGear')
      assert(gear and gear[2]==0 and gear[3]==1 and table.getn(calls)>0)
      assert(ShowingHelm()==nil and ShowingCloak()==1)
      local saved=__WBCharacterVariablesText()
      assert(string.find(saved,'WBShowHelm = 0',1,true) and string.find(saved,'WBShowCloak = 1',1,true))
      assert(not string.find(__WBSavedVariablesText(),'WBShowHelm',1,true))
      WBShowHelm,WBShowCloak=nil,nil assert(ShowingHelm()==1)
      assert(loadstring(saved))()
      assert(ShowingHelm()==nil and ShowingCloak()==1)
      HideUIPanel(GameMenuFrame) ShowUIPanel(UIOptionsFrame)
      assert(not UIOptionsFrameCheckButton20:GetChecked() and UIOptionsFrameCheckButton31:GetChecked())
      UIOptionsFrameCheckButton31:SetChecked(nil)
      calls={} UIOptionsFrameOkay:Click()
      gear=sentCommand('showGear') assert(gear and gear[2]==0 and gear[3]==0)
      HideUIPanel(GameMenuFrame) ShowUIPanel(UIOptionsFrame)
      calls={} UIOptionsFrameOkay:Click() assert(not sentCommand('showGear'))
      HideUIPanel(GameMenuFrame) ShowUIPanel(UIOptionsFrame)
      UIOptionsFrameDefaults:Click() assert(UIOptionsFrameCheckButton20:GetChecked() and UIOptionsFrameCheckButton31:GetChecked())
      calls={} UIOptionsFrameOkay:Click()
      gear=sentCommand('showGear') assert(gear and gear[2]==1 and gear[3]==1 and ShowingHelm()==1)
      HideUIPanel(GameMenuFrame)
    )");

    // Key bindings: the rows with nothing behind them are not listed and their keys are Skyrim's; F5 and F9 are
    // Skyrim's quicksave and quickload. An older save's lines for them take nothing from Skyrim.
    run(R"(
      local listed={}
      for i=1,GetNumBindings() do listed[GetBinding(i)]=i end
      for _,c in ipairs({'SITORSTAND','FOLLOWTARGET','PITCHUP','PITCHDOWN','REPLY','REPLY2','TOGGLEAUTOSELFCAST',
          'TARGETPET','TARGETPARTYPET1','TARGETPARTYPET4','TARGETLASTHOSTILE','ASSISTTARGET','PETATTACK','TOGGLEKEYRING',
          'TOGGLEPETBOOK','TOGGLECHARACTER2','TOGGLECHARACTER3','TOGGLECHARACTER4','TOGGLEFRIENDSTAB','TOGGLEWHOTAB',
          'TOGGLEGUILDTAB','TOGGLERAIDTAB','TOGGLEWORLDSTATESCORES','TOGGLEBATTLEFIELDMINIMAP','SCREENSHOT','HEADER_CAMERA',
          'NEXTVIEW','CAMERAZOOMIN','SETVIEW1','SAVEVIEW5','RESETVIEW2','FLIPCAMERAYAW','HEADER_RAID_TARGET','RAIDTARGET1',
          'RAIDTARGET8','RAIDTARGETNONE'}) do assert(not listed[c],c) end
      for _,c in ipairs({'HEADER_MOVEMENT','JUMP','TOGGLESHEATH','HEADER_CHAT','OPENCHAT','TARGETPARTYMEMBER4','TOGGLEBAG2',
          'TOGGLECHARACTER0','TOGGLECHARACTER1','TOGGLESOCIAL','HEADER_MISC','TOGGLEUI','HEADER_MULTIACTIONBAR',
          'MULTIACTIONBAR4BUTTON12','HEADER_WOWBRIDGE','WBLOOTALL'}) do assert(listed[c],c) end
      for _,k in ipairs({'R','SHIFT-R','F5','F9','PRINTSCREEN','X','G','F','U','H','SHIFT-P','SHIFT-I','SHIFT-T',
          'SHIFT-SPACE','HOME','END','INSERT','DELETE','SHIFT-F1','SHIFT-F5'}) do
        assert(GetBindingAction(k)=='',k)
      end
      assert(not GetBindingKey('TARGETPARTYMEMBER4') and not GetBindingKey('TOGGLEBAG2'))
      assert(GetBindingAction('F2')=='TARGETPARTYMEMBER1' and GetBindingAction('F4')=='TARGETPARTYMEMBER3')
      assert(GetBindingAction('F8')=='TOGGLEBAG1' and GetBindingAction('F10')=='TOGGLEBAG3' and GetBindingAction('F11')=='TOGGLEBAG4')
      assert(GetBindingAction('O')=='TOGGLESOCIAL' and BINDING_NAME_TOGGLESOCIAL=='Testing Tools')
      assert(GetBindingKey('WBLOOTALL')=='R' and GetBindingAction('B')=='OPENALLBAGS' and GetBindingAction('SHIFT-M')=='WBSKYRIMMAP')
      KeyBindingFrame_LoadUI() ShowUIPanel(KeyBindingFrame)
      KeyBindingFrameScrollFrame.offset=listed.TOGGLESOCIAL-1 KeyBindingFrame_Update()
      assert(KeyBindingFrameBinding1Description:GetText()=='Testing Tools')
      HideUIPanel(KeyBindingFrame)
      -- A save from before: its lines for unlisted commands are ignored, so R takes nothing (Loot All keeps it in the
      -- loot window) and Y, which Toggle Keyring held, is free for Toggle Auto Loot; F5 and F9 at their old defaults
      -- go back to Skyrim, while F6 the player gave the second bag stays.
      __WBLoadBindings(1,"bind R REPLY\nbind SHIFT-R REPLY2\nbind Y TOGGLEKEYRING\nbind F5 TARGETPARTYMEMBER4\n"..
        "bind F9 TOGGLEBAG2\nbind F6 TOGGLEBAG2\nbind PRINTSCREEN SCREENSHOT\nbind C TOGGLECHARACTER0\n")
      assert(GetBindingAction('R')=='' and GetBindingAction('SHIFT-R')=='' and GetBindingAction('PRINTSCREEN')=='')
      assert(GetBindingAction('F5')=='' and GetBindingAction('F9')=='' and GetBindingAction('F6')=='TOGGLEBAG2')
      assert(GetBindingKey('WBLOOTALL')=='R' and GetBindingKey('WBTOGGLEAUTOLOOT')=='Y' and not GetBindingKey('REPLY'))
      -- Saved since, F5 given back to the fourth follower by the player is kept.
      SetBinding('F5','TARGETPARTYMEMBER4') calls={} SaveBindings(1)
      local saved=sentCommand('saveBindings')[3]
      assert(string.find(saved,'^# WoWBridge bindings 2\n') and string.find(saved,'bind F5 TARGETPARTYMEMBER4',1,true))
      assert(not string.find(saved,'REPLY',1,true))
      __WBDefaultBindings() __WBLoadBindings(1,saved)
      assert(GetBindingAction('F5')=='TARGETPARTYMEMBER4' and GetBindingAction('F6')=='TOGGLEBAG2' and GetBindingAction('R')=='')
      -- The panel's defaults are Skyrim's too.
      LoadBindings(0)
      assert(GetBindingAction('F5')=='' and GetBindingAction('R')=='' and GetBindingKey('WBLOOTALL')=='R')
      assert(GetBindingAction('B')=='OPENALLBAGS')
    )");

    // The options' buff filters: "0" is unticked (the client's default for both), "1" keeps only castable buffs and
    // debuffs the player can dispel (Curse, Poison).
    run(R"(
      __WBFixture('target')
      local t=__WB.units[__WB.tokens.target]
      t.buffs={{spell=133,texture='Fireball'},{spell=22812,texture='Barkskin'}}
      t.debuffs={{spell=1,texture='Magic',dispel='Magic'},{spell=2,texture='Poison',dispel='Poison'}}
      for _,off in ipairs({'0',0,false}) do
        assert(UnitBuff('target',1,off)=='Fireball' and UnitDebuff('target',1,off)=='Magic')
      end
      SHOW_CASTABLE_BUFFS,SHOW_DISPELLABLE_DEBUFFS='0','0'
      assert(UnitBuff('target',1,SHOW_CASTABLE_BUFFS)=='Fireball' and UnitDebuff('target',2,SHOW_DISPELLABLE_DEBUFFS)=='Poison')
      for _,on in ipairs({'1',1}) do
        assert(UnitBuff('target',1,on)=='Barkskin' and not UnitBuff('target',2,on))
        assert(UnitDebuff('target',1,on)=='Poison' and not UnitDebuff('target',2,on))
      end
      __WBFixture('idle')
    )");
}
