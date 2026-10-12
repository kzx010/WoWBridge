#include <catch_amalgamated.hpp>
#include "wowcore/dbc.hpp"
#include "wowcore/minimap.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/soft_raster.hpp"
#include "wowcore/ui_session.hpp"
#include "wowcore/wowapi.hpp"
#include "addons/lua_engine.hpp"
#include "ui/widget_tree.hpp"
#include "ui/framexml_takeover.hpp"
extern "C" {
#include <lauxlib.h>
}
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

TEST_CASE("Carriages drive the client TaxiFrame, routes, fares, commands and ReloadUI", "[client][api][taxi]") {
    using namespace wowcore;
    // The character-sheet renderer's process-wide debug request can outlive a
    // preceding UiSession. Its later-client UnitAura probe is unrelated here.
    wowee::ui::frameXmlTakeProbeRequest();
    const char* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW 1.12 client data not present");
    const auto root=std::filesystem::temp_directory_path()/ ("wb-taxi-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::filesystem::remove_all(root); } } cleanup{root};
    // One independently generated north-up gradient tile verifies the actual
    // raster path and orientation, without requiring installed Skyrim assets.
    std::vector<uint8_t> bytes(1172+256*256*4,0);
    const auto u32=[&](size_t at,uint32_t value) { for(int i=0;i<4;++i) bytes[at+i]=value>>(i*8); };
    std::copy_n("BLP2",4,bytes.begin()); u32(4,1); bytes[8]=3; bytes[9]=8;
    u32(12,256); u32(16,256); u32(20,1172); u32(84,256*256*4);
    for(int y=0;y<256;++y) for(int x=0;x<256;++x) {
        const size_t at=1172+(y*256+x)*4;
        bytes[at]=40; bytes[at+1]=40+(255-y)/2; bytes[at+2]=40+x/2; bytes[at+3]=255;
    }
    {
        std::ofstream output(root/"WB_testworld_-4_-4.blp",std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    }
    std::istringstream index("WOWBRIDGE_MINIMAP 1 64 16384\ntestworld -4 -4 WB_testworld_-4_-4.blp\n");
    const auto atlas=MinimapAtlas::read(index);
    auto mpq=std::make_shared<Mpq>(data,std::filesystem::path{},root);
    const ClientDb db(*mpq); SoftTextures textures; UiSession ui(mpq,db,textures,1280,720);
    std::string error; REQUIRE(ui.start(&error)); auto* L=ui.lua();
    const auto run=[&](const std::string& code) {
        const int status=luaL_dostring(L,code.c_str());
        INFO((status ? lua_tostring(L,-1) : code)); REQUIRE(status==0);
    };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/hud_states.lua").c_str())==0);
    run("__WBFixture('idle')"); REQUIRE(ui.loadInterface(&error));
    std::vector<std::string> commands;
    setHostHandler(L,[&](lua_State* state,const std::string& name) {
        if (name=="taxi") {
            std::string command=name;
            for(int i=2;i<=lua_gettop(state);++i) { command+='|'; command+=lua_tostring(state,i); }
            commands.push_back(std::move(command));
        }
        return 0;
    });
    run(R"(
      local nodes,cost=NumTaxiNodes,TaxiNodeCost
      __WBLogin() __WBInstallTaxi()
      assert(nodes==NumTaxiNodes and cost==TaxiNodeCost and NumTaxiNodes()==0)
      local ok,err=pcall(TaxiNodeCost,1) assert(not ok and string.find(err,'Invalid taxi node slot',1,true))
      events={}
      local f=CreateFrame('Frame') f:RegisterEvent('TAXIMAP_OPENED') f:RegisterEvent('TAXIMAP_CLOSED')
      f:SetScript('OnEvent',function() tinsert(events,event) end)
      __WBTaxiFixture()
      assert(events[1]=='TAXIMAP_OPENED' and TaxiFrame:IsVisible())
      assert(NumTaxiNodes()==9 and TaxiMerchant:GetText()=='Bjorlam' and UnitName('NPC')=='Bjorlam')
      assert(TaxiNodeGetType(1)=='CURRENT' and TaxiNodeGetType(2)=='REACHABLE')
      assert(TaxiNodeCost(2)==200000 and TaxiNodeCost(7)==500000)
      assert(TaxiNodeGetType(99)=='NONE' and GetNumRoutes(1)==0 and GetNumRoutes(2)==1)
      local x,y=TaxiNodePosition(2)
      assert(x>0 and x<1 and y>0 and y<1)
      assert(TaxiGetDestX(2,1)==x and TaxiGetDestY(2,1)==y)
      local cx,cy=TaxiNodePosition(1)
      assert(TaxiGetSrcX(2,1)==cx and TaxiGetSrcY(2,1)==cy)
      this=TaxiButton2 this:GetScript('OnEnter')() this=nil
      assert(GameTooltip:IsShown() and GameTooltipTextLeft1:GetText()=='Solitude')
      assert(TaxiRoute1:IsShown() and GameTooltipMoneyFrame.staticMoney==200000)
      this=TaxiButton1 this:GetScript('OnEnter')() this=nil
      assert(NUM_TAXI_ROUTES==8)
      TaxiButton1:Click('LeftButton')
      __WB.world.money=19*10000 TaxiButton2:Click('LeftButton')
      assert(not __WB.taxiPending)
    )");
    CHECK(commands.empty());
    run("__WB.world.money=20*10000 TaxiButton2:Click('LeftButton') TaxiButton2:Click('LeftButton')");
    REQUIRE(commands.size()==1); CHECK(commands.back()=="taxi|travel|2|72|20");
    // Host refusal acknowledges the request and retains a usable frame.
    run("__WBTaxiFixture() TaxiButton7:Click('LeftButton')");
    REQUIRE(commands.size()==2); CHECK(commands.back()=="taxi|travel|7|72|50");
    run("__WBTaxiFixture() return __WBReloadStateText()");
    const std::string reload=lua_tostring(L,-1); lua_pop(L,1);
    run("CloseTaxiMap() CloseTaxiMap() assert(not TaxiFrame:IsShown() and not __WB.taxi and NumTaxiNodes()==0)");
    REQUIRE(commands.size()==3); CHECK(commands.back()=="taxi|close|0|72|0");
    run("assert(events[table.getn(events)]=='TAXIMAP_CLOSED'); __WBTaxiFixture()");
    textures.sync(ui.frame(0.1f));
    run("HideUIPanel(TaxiFrame)");
    textures.sync(ui.frame(0.1f));
    REQUIRE(commands.size()==4); CHECK(commands.back()=="taxi|close|0|72|0");
    run(R"(
      __WBTaxi({session=73,name='Driver',worldspace='testworld',nodes={
        {id=1,name='Here',x=-192,y=-192,cost=0,current=true},
        {id=2,name='North East',x=-96,y=-96,cost=20},
        {id=3,name='North West',x=-224,y=-64,cost=50}}})
      GameTooltip:Hide()
      for i=1,NUM_TAXI_ROUTES do getglobal('TaxiRoute'..i):Hide() end
    )");
    ImDrawData* draw=nullptr;
    for(int i=0;i<64;++i) { ui.applyLiveMinimap(&atlas); draw=ui.frame(0.016f); textures.sync(draw); }
    Image image(1280,720); rasterize(draw,textures,image);
    const auto* map=ui.engine().widgets().findByName("TaxiRouteMap"); REQUIRE(map); CHECK(map->hostModel);
    const float scale=ui.engine().widgets().uiScale();
    const auto pixel=[&](double x,double y,int c) {
        const int px=std::lround((map->left+x*map->rectW)*scale);
        const int py=std::lround(720-(map->bottom+y*map->rectH)*scale);
        REQUIRE(px>=0); REQUIRE(px<1280); REQUIRE(py>=0); REQUIRE(py<720);
        return image.rgba[(py*1280+px)*4+c];
    };
    CHECK(pixel(0.65,0.5,0)>pixel(0.35,0.5,0)); // east red
    CHECK(pixel(0.5,0.65,1)>pixel(0.5,0.35,1)); // north green
    run("local x,y=TaxiNodePosition(2); local bx,by=TaxiButton2:GetCenter(); local mx,my=TaxiMap:GetLeft(),TaxiMap:GetBottom(); "
        "assert(math.abs(bx-mx-x*TAXI_MAP_WIDTH)<0.01 and math.abs(by-my-y*TAXI_MAP_HEIGHT)<0.01)");
    const auto nodePixel=[&](int slot) {
        run("return TaxiNodePosition("+std::to_string(slot)+")");
        const double x=lua_tonumber(L,-2), y=lua_tonumber(L,-1); lua_pop(L,2);
        return MapPoint{(map->left+x*map->rectW)*scale,720-(map->bottom+y*map->rectH)*scale};
    };
    const auto source=nodePixel(1), destination=nodePixel(2);
    run("this=TaxiButton2 this:GetScript('OnEnter')() this=nil GameTooltip:Hide()");
    for(int i=0;i<64;++i) { ui.applyLiveMinimap(&atlas); draw=ui.frame(0.016f); textures.sync(draw); }
    Image routed(1280,720); rasterize(draw,textures,routed);
    int near=0, far=0;
    const double dx=destination.x-source.x, dy=destination.y-source.y;
    for(int y=std::ceil(720-(map->bottom+map->rectH)*scale);y<720-map->bottom*scale;++y)
        for(int x=std::ceil(map->left*scale);x<(map->left+map->rectW)*scale;++x) {
            const size_t at=(y*1280+x)*4;
            if (std::equal(image.rgba.begin()+at,image.rgba.begin()+at+4,routed.rgba.begin()+at)) continue;
            const double along=std::clamp(((x-source.x)*dx+(y-source.y)*dy)/(dx*dx+dy*dy),0.0,1.0);
            const double distance=std::hypot(x-source.x-along*dx,y-source.y-along*dy);
            if (distance<20) ++near; else ++far;
        }
    CHECK(near>0); CHECK(far==0); // no repeated parallel lines from wrapped rotated UVs
    // An unavailable atlas/world and a close discard previous terrain.
    ui.applyLiveMinimap(nullptr); CHECK_FALSE(map->hostModel);
    run("__WB.taxi.worldspace='unknown'"); ui.applyLiveMinimap(&atlas); CHECK_FALSE(map->hostModel);
    run("__WBTaxi(nil)"); ui.applyLiveMinimap(&atlas); CHECK_FALSE(map->hostModel);
    INFO(ui.engine().lastError()); CHECK(ui.engine().lastError().empty());
    {
        SoftTextures freshTextures; UiSession fresh(mpq,db,freshTextures,1280,720);
        REQUIRE(fresh.start(&error)); REQUIRE(fresh.loadInterface(&error));
        REQUIRE(luaL_dostring(fresh.lua(),reload.c_str())==0);
        REQUIRE(luaL_dostring(fresh.lua(),"__WBLogin(); assert(NumTaxiNodes()==9 and TaxiNodeCost(7)==500000); "
            "assert(TaxiFrame:IsVisible() and TaxiMerchant:GetText()=='Bjorlam'); __WBInstallTaxi()") == 0);
        CHECK(fresh.engine().lastError().empty());
    }
}
