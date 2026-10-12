#include <catch_amalgamated.hpp>
#include "wowcore/worldmap.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/soft_raster.hpp"
#include "wowcore/ui_session.hpp"
#include "wowcore/wowapi.hpp"
#include "addons/lua_engine.hpp"
#include "ui/widget_tree.hpp"
#include "pipeline/blp_loader.hpp"
#include <imgui.h>
extern "C" {
#include <lauxlib.h>
}
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <set>

namespace {
double area(const std::vector<wowcore::WorldMapVertex>& vertices) {
    double result=0;
    for (size_t i=0;i+2<vertices.size();i+=3) {
        const auto a=vertices[i].position,b=vertices[i+1].position,c=vertices[i+2].position;
        result+=std::abs((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x))/2;
    }
    return result;
}
}
TEST_CASE("world map projects stable rectangular geography and cuts parent coverage", "[worldmap]") {
    using namespace wowcore;
    const WorldMapView view{{-256,-128},{256,128}};
    const auto east=worldMapProject(view,{128,64});
    REQUIRE(east); CHECK(east->x==0.75); CHECK(east->y==0.25);
    const auto back=worldMapUnproject(view,*east);
    REQUIRE(back); CHECK(back->x==128); CHECK(back->y==64);
    CHECK_FALSE(worldMapProject({{},{}},{}));
    CHECK_FALSE(worldMapProject(view,{std::numeric_limits<double>::quiet_NaN(),0}));
    CHECK_FALSE(worldMapContains(*worldMapProject(view,{300,0})));
    const MinimapTile parent{{-256,-256},512,"parent"},child{{-64,-64},128,"child"};
    const auto whole=worldMapTileTriangles(view,parent),cut=worldMapTileTriangles(view,parent,{&child}),
        fill=worldMapTileTriangles(view,child);
    CHECK(area(whole)==Catch::Approx(1));
    CHECK(area(cut)+area(fill)==Catch::Approx(1));
    CHECK(area(fill)==Catch::Approx(0.125));
    for (const auto& vertex : cut) {
        CHECK(worldMapContains(vertex.position));
        CHECK(vertex.uv.x>=0.5/256); CHECK(vertex.uv.x<=255.5/256);
        CHECK(vertex.uv.y>=0.5/256); CHECK(vertex.uv.y<=255.5/256);
    }
    CHECK(worldMapTileTriangles(view,{{1000,1000},256,"offscreen"}).empty());
    CHECK(worldMapTileTriangles(view,parent,{&parent}).empty());
    // Tamriel's full atlas exceeds the minimap's 4096-yard radius limit.
    const WorldMapView continent{{-9216,-6144},{9216,6144}},textureBounds{{-6144,-6144},{6144,6144}};
    const auto overview=worldMapRectTriangles(continent,textureBounds,{1024,1024});
    CHECK(area(overview)==Catch::Approx(2.0/3));
    const auto north=worldMapTileTriangles(view,{{-256,0},256,"north"});
    REQUIRE_FALSE(north.empty());
    CHECK(north.front().position.y==0); CHECK(north.front().uv.y==Catch::Approx(0.5));
}

TEST_CASE("client world map uses exterior terrain POIs clicks keys and live player heading", "[client][api][worldmap]") {
    using namespace wowcore;
    const char* env=std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data=env?env:WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data/"interface.MPQ")) SKIP("WoW 1.12 client data not present");
    const auto root=std::filesystem::temp_directory_path()/("wowcore-worldmap-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::filesystem::remove_all(root); } } cleanup{root};
    std::ostringstream rows; rows<<"WOWBRIDGE_MINIMAP 1 64 16384\n";
    for (int x=-16;x<16;x+=4) for (int y=-16;y<16;y+=4) {
        const auto file="WB_testworld_"+std::to_string(x)+"_"+std::to_string(y)+".blp";
        rows<<"testworld "<<x<<' '<<y<<' '<<file<<'\n';
        std::vector<uint8_t> bytes(1172+256*256*4,0);
        const auto u32=[&](size_t at,uint32_t value) { for(int i=0;i<4;++i) bytes[at+i]=value>>(i*8); };
        std::copy_n("BLP2",4,bytes.begin()); u32(4,1); bytes[8]=3; bytes[9]=8;
        u32(12,256);u32(16,256);u32(20,1172);u32(84,256*256*4);
        for(int py=0;py<256;++py) for(int px=0;px<256;++px) {
            const size_t at=1172+(py*256+px)*4;
            bytes[at]=30; bytes[at+1]=40+(y*64+255-py+1024)*160/2048;
            bytes[at+2]=20+(x*64+px+1024)*200/2048; bytes[at+3]=255;
        }
        std::ofstream output(root/file,std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    }
    // The optional overview has the same colours/geography as the tiled map,
    // with a visibly different blue channel to prove full/local selection.
    {
        std::vector<uint8_t> bytes(1172+256*256*4,0);
        const auto u32=[&](size_t at,uint32_t value) { for(int i=0;i<4;++i) bytes[at+i]=value>>(i*8); };
        std::copy_n("BLP2",4,bytes.begin()); u32(4,1); bytes[8]=3; bytes[9]=8;
        u32(12,256);u32(16,256);u32(20,1172);u32(84,256*256*4);
        for(int y=0;y<256;++y) for(int x=0;x<256;++x) {
            const size_t at=1172+(y*256+x)*4;
            bytes[at]=80;bytes[at+1]=40+(255-y)*160/256;bytes[at+2]=20+x*200/256;bytes[at+3]=255;
        }
        std::ofstream output(root/"WB_testworld_overview.blp",std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    }
    std::istringstream index(rows.str()); const auto atlas=MinimapAtlas::read(index);
    auto mpq=std::make_shared<Mpq>(data,std::filesystem::path{},root);
    const ClientDb db(*mpq); SoftTextures textures; UiSession ui(mpq,db,textures,1280,720);
    std::string error; REQUIRE(ui.start(&error)); auto* L=ui.lua();
    const auto run=[&](const std::string& code) {
        const int status=luaL_dostring(L,code.c_str());
        INFO((status?lua_tostring(L,-1):code)); REQUIRE(status==0);
    };
    REQUIRE(luaL_dofile(L,(std::string(WOWBRIDGE_ROOT)+"/wowcore/tests/fixtures/elunara.lua").c_str())==0);
    run("__WBFixture('idle')"); REQUIRE(ui.loadInterface(&error));
    // Before game installation, retain captured map answers and closures.
    run("assert(GetMapInfo()=='Teldrassil'); __WBInstallWorldMap(); __WBApplyLive({worldmap={"
        "worldspace='testworld',name='Skyrim',x=0,y=0,heading=0,pois={"
        "{kind='quest',name='Retrieve the Dragonstone',x=350,y=250,icon=6},"
        "{kind='location',name='Riverwood',x=-350,y=-200,icon=4,discovered=true},"
        "{kind='location',name='Unseen',x=50,y=50,icon=5,discovered=false},"
        "{kind='location',name='Far discovered',x=950,y=950,icon=5,discovered=true},"
        "{kind='quest',name='Wrong world',x=99999,y=99999}}}})");
    ui.applyLiveMinimap(&atlas);
    const auto frame=[&] {
        ImDrawData* draw=nullptr;
        for(int i=0;i<64;++i) { ui.applyLiveMinimap(&atlas);draw=ui.frame(0.016f);textures.sync(draw); }
        Image image(1280,720); rasterize(draw,textures,image); return image;
    };
    run("ShowUIPanel(WorldMapFrame); assert(WorldMapFrame:IsShown()); "
        "assert(GetCurrentMapZone()==1 and GetMapContinents()=='Skyrim' and GetMapZones(1)=='Local view'); "
        "assert(GetNumMapLandmarks()==2); local x,y=GetPlayerMapPosition('player'); assert(x==0.5 and y==0.5)");
    const auto local=frame();
    run("assert(WorldMapFramePOI1.name=='Retrieve the Dragonstone' and WorldMapFramePOI2.name=='Riverwood','POI names'); "
        "this=WorldMapFramePOI1; this:GetScript('OnEnter')(); "
        "assert(WorldMapTooltip:IsShown() and WorldMapTooltipTextLeft1:GetText()=='Retrieve the Dragonstone','tooltip title'); "
        "assert(string.find(WorldMapTooltipTextLeft2:GetText(),'Shift+M',1,true),'tooltip hint'); "
        "this:GetScript('OnLeave')(); this=nil; assert(not WorldMapTooltip:IsShown(),'tooltip leave'); "
        "for i=1,12 do assert(not getglobal('WorldMapDetailTile'..i):IsShown(),'original map tiles') end; "
        "WorldMapZoomOutButton:Click(); assert(GetCurrentMapZone()==0 and GetNumMapLandmarks()==3,'continent '..GetCurrentMapZone()..' POIs '..GetNumMapLandmarks()); "
        "assert(WorldMapZoomOutButton:IsEnabled()==0,'zoom out disabled')");
    const auto continent=frame(); CHECK(local.rgba!=continent.rgba);
    run("assert(WorldMapFramePOI3.name=='Far discovered')");
    const auto* button=ui.engine().widgets().findByName("WorldMapButton"); REQUIRE(button);
    const float mapScale=ui.engine().widgets().uiScale();
    const float clickX=(button->left+0.6f*button->rectW)*mapScale,
        clickY=720-(button->bottom+0.6f*button->rectH)*mapScale;
    ui.pointer(clickX,clickY); frame();
    ui.engine().dispatchMouse(clickX,clickY,720,{true,false,false});
    ui.engine().dispatchMouse(clickX,clickY,720,{});
    run("assert(GetCurrentMapZone()==1); local x,y=GetPlayerMapPosition('player'); assert(x<0.5 and y>0.5); "
        "SetMapToCurrentZone(); __WB.worldmap.heading=math.pi/2");
    const auto turned=frame(); CHECK(local.rgba!=turned.rgba);
    const auto* detail=ui.engine().widgets().findByName("WorldMapDetailFrame"); REQUIRE(detail);
    const float scale=ui.engine().widgets().uiScale();
    const auto pixel=[&](const Image& image,double x,double y,int c) {
        const int px=std::lround((detail->left+x*detail->rectW)*scale);
        const int py=std::lround(720-(detail->bottom+(1-y)*detail->rectH)*scale);
        REQUIRE(px>=0); REQUIRE(px<1280); REQUIRE(py>=0); REQUIRE(py<720);
        return image.rgba[(py*image.w+px)*4+c];
    };
    CHECK(pixel(local,0.75,0.5,0)>pixel(local,0.25,0.5,0));
    CHECK(pixel(local,0.5,0.25,1)>pixel(local,0.5,0.75,1));
    CHECK(pixel(continent,0.75,0.5,2)>pixel(local,0.75,0.5,2));
    for(int c=0;c<4;++c) CHECK(pixel(local,0.7,0.3,c)==pixel(turned,0.7,0.3,c));
    run("__WB.worldmap.x=100; local x,y=GetPlayerMapPosition('player'); assert(x>0.5 and y==0.5)");
    const auto moved=frame(); CHECK(moved.rgba!=turned.rgba);
    run("this=WorldMapFramePOI1; this:GetScript('OnEnter')(); this=nil; __WB.worldmap.pois[1].name='Updated objective'");
    frame();
    run("assert(WorldMapTooltip:IsShown() and WorldMapTooltipTextLeft1:GetText()=='Updated objective'); "
        "this=WorldMapFramePOI1; this:GetScript('OnLeave')(); this=nil");
    // The minimap can have interior coordinates while the world map uses the
    // exterior doorway. Reload-style installation is idempotent.
    run("__WBApplyLive({minimap={worldspace='interior:1',x=9999,y=9999,heading=0},"
        "worldmap={worldspace='testworld',name='Skyrim',x=100,y=0,heading=math.pi/2,interior=true,pois={}}}); "
        "__WBInstallWorldMap()");
    const auto inside=frame();
    run("local x,y=GetPlayerMapPosition('player'); assert(x>0.5 and y==0.5); "
        "assert(string.find(WBWorldMapNotice:GetText(),'Interior',1,true)); assert(GetNumMapLandmarks()==0)");
    CHECK(pixel(inside,0.7,0.3,0)==pixel(moved,0.7,0.3,0));
    run("return __WBReloadStateText()");
    const std::string reload=lua_tostring(L,-1); lua_pop(L,1);
    // A world switch clears the previous local centre. Unknown/invalid poses
    // never retain the previous terrain, markers or player position.
    run("__WB.worldmap.worldspace='unknown'"); frame();
    run("local x,y=GetPlayerMapPosition('player'); assert(x==0 and y==0); assert(GetNumMapLandmarks()==0); "
        "assert(string.find(WBWorldMapNotice:GetText(),'Terrain unavailable',1,true))");
    CHECK_FALSE(ui.engine().widgets().findByName("WorldMapDetailFrame")->hostModel);
    run("__WB.worldmap.worldspace='testworld'; __WB.worldmap.x=0/0"); frame();
    run("assert(not __WB.worldMapView)");
    run("__WBApplyLive({worldmap=false})"); frame();
    run("assert(not __WB.worldMapView and GetNumMapLandmarks()==0)");
    std::vector<std::string> commands;
    setHostHandler(L,[&](lua_State* state,const std::string& name) {
        if(name=="menu") commands.push_back(lua_tostring(state,2));
        return 0;
    });
    run("assert(GetBindingAction('SHIFT-M')=='WBSKYRIMMAP'); RunBinding('WBSKYRIMMAP'); "
        "assert(not WorldMapFrame:IsShown())");
    REQUIRE(commands.size()==1); CHECK(commands.back()=="MapMenu");
    run("ToggleWorldMap(); assert(not WorldMapFrame:IsShown()); "
        "assert(rawget(_G,'WBMinimapWorldMapButton')==nil); "
        "WorldMapMicroButton:Click('LeftButton'); RunBinding('TOGGLEWORLDMAP'); assert(not WorldMapFrame:IsShown()); "
        "assert(Minimap:IsVisible() and MinimapCluster:IsVisible())");
    REQUIRE(commands.size()==4);
    for (const auto& command : commands) CHECK(command=="MapMenu");
    run("__WBApplyLive({target='player'}); RunBinding('TOGGLEWORLDMAP'); "
        "assert(not WorldMapFrame:IsShown() and UnitExists('target'))");
    // The corrected fixture closes panels/cursors before testing target-only Escape.
    run("ClearCursor(); CloseDropDownMenus(); CloseAllWindows(); CloseAllBags(); ChatFrameEditBox:ClearFocus(); "
        "assert(not WorldMapFrame:IsShown() and UnitExists('target') and __WBEscapeHasWork())");
    frame(); // Hide propagates to the copied widget tree on its next layout/render.
    CHECK_FALSE(ui.engine().keyboardFrameShown());
    bool clearedTarget=false;
    setHostHandler(L,[&](lua_State* state,const std::string& name) {
        if(name=="command" && std::string(lua_tostring(state,2))=="ClearTarget") clearedTarget=true;
        return 0;
    });
    run("RunBinding('TOGGLEGAMEMENU'); assert(not GameMenuFrame:IsShown())");
    CHECK(clearedTarget);
    SoftTextures freshTextures; UiSession fresh(mpq,db,freshTextures,1280,720);
    REQUIRE(fresh.start(&error)); REQUIRE(fresh.loadInterface(&error));
    REQUIRE(luaL_dostring(fresh.lua(),reload.c_str())==0);
    REQUIRE(luaL_dostring(fresh.lua(),"__WBLogin(); __WBSkyrimMenus(); assert(GetMapContinents()=='Skyrim'); "
        "assert(GetBindingAction('SHIFT-M')=='WBSKYRIMMAP'); RunBinding('TOGGLEWORLDMAP')")==0);
    fresh.applyLiveMinimap(&atlas);
    REQUIRE(luaL_dostring(fresh.lua(),"assert(not WorldMapFrame:IsShown() and Minimap:IsVisible()); "
        "__WBLoadBindings(1,'bind M TOGGLEWORLDMAP\\n'); assert(GetBindingAction('SHIFT-M')=='WBSKYRIMMAP')")==0);
}

TEST_CASE("generated world map overviews decode with alpha and complete mip chains", "[worldmap][worldmap-assets]") {
    const auto root=std::filesystem::path(WOWBRIDGE_ROOT)/"build/skyrim_minimap";
    std::ifstream index(root/"index.tsv");
    if (!index) SKIP("generate the atlas and overviews with tools/skyrim_minimap.py");
    const auto atlas=wowcore::MinimapAtlas::read(index);
    index.clear(); index.seekg(0);
    std::string line; std::getline(index,line);
    std::set<std::string> worlds;
    while(std::getline(index,line)) { std::istringstream row(line);std::string world;row>>world;worlds.insert(world); }
    for(const auto& world : worlds) {
        INFO(world);
        std::ifstream input(root/("WB_"+world+"_overview.blp"),std::ios::binary);
        REQUIRE(input.good());
        const std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input),{});
        const auto image=wowee::pipeline::BLPLoader::load(bytes);
        CHECK(image.width>=1); CHECK(image.width<=1024);
        CHECK(image.height>=1); CHECK(image.height<=1024);
        CHECK(image.data.size()==image.width*image.height*4);
        const auto* layer=atlas.world(world); REQUIRE(layer);
        const double width=(layer->high.x-layer->low.x)/8,height=(layer->high.y-layer->low.y)/8;
        const double ratio=std::min(1.0,1024/std::max(width,height));
        CHECK(image.width==std::max(1.0,std::floor(width*ratio)));
        CHECK(image.height==std::max(1.0,std::floor(height*ratio)));
        REQUIRE(bytes.size()>=1172);
        CHECK(bytes[8]==3); CHECK(bytes[9]==8); CHECK(bytes[11]==1);
        const auto u32=[&](size_t at) { return uint32_t(bytes[at])|uint32_t(bytes[at+1])<<8|uint32_t(bytes[at+2])<<16|uint32_t(bytes[at+3])<<24; };
        uint32_t w=image.width,h=image.height;
        for(size_t level=0;level<16;++level) {
            const auto offset=u32(20+level*4),size=u32(84+level*4);
            CHECK(size==w*h*4); CHECK(offset+size<=bytes.size());
            if(w==1 && h==1) break;
            w=std::max(1u,w/2);h=std::max(1u,h/2);
        }
    }
}
