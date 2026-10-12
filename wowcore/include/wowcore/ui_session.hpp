// The WoW 1.12 interface as one object: Lua engine + WoWBridge's API + the client's FrameXML + WoWee's widget
// renderer in an ImGui context of its own. The DLL and the native tests drive it the same way: write __WB, fire
// events, call frame(), draw the ImDrawData it returns.
#pragma once
#include "wowcore/character.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ImDrawData;
struct ImDrawList;
struct ImGuiContext;
struct lua_State;
namespace wowee::addons { class LuaEngine; }
namespace wowee::rendering { class VkContext; }

namespace wowcore {
class ClientDb;
class Mpq;
class MinimapAtlas;
struct MapPoint;
struct Look;

class UiSession {
public:
    // `textures` receives the interface's art (WoWee uploads through it) and must also serve ImGui's own textures
    // (the font atlas) from each frame's ImDrawData::Textures. All three must outlive the session.
    UiSession(std::shared_ptr<Mpq> mpq, const ClientDb& db, wowee::rendering::VkContext& textures, float width, float height);
    ~UiSession();
    UiSession(const UiSession&) = delete;
    UiSession& operator=(const UiSession&) = delete;

    // Engine, API and fonts. Write the initial __WB (lua()) between start() and loadInterface().
    bool start(std::string* error);
    // FrameXML from the archives; false with the first failure in `error`.
    bool loadInterface(std::string* error);
    // A load-on-demand addon from the archives (what LoadAddOn does); false if absent or broken.
    bool loadAddOn(const std::string& name, std::string* error);
    // An event to every frame registered for it, with 1.12's arg1..argN; game::kEventNil passes nil.
    void fire(const std::string& event, const std::vector<std::string>& args = {});
    // Render/Lua thread only. The atlas outlives the session; nullptr/unknown
    // worldspace clears terrain and map coordinates (interior/unsupported area).
    void setMinimap(const MinimapAtlas* atlas, const std::string& worldspace, MapPoint player, double heading);
    // Copy __WB.minimap pose, parent layers, tracking blips and compass POIs from live Lua data.
    void applyLiveMinimap(const MinimapAtlas* atlas);
    // Full map has a separate exterior pose, including while in an interior.
    void applyLiveWorldMap(const MinimapAtlas* atlas);
    void setPlayerLook(const Look& look, const Appearance& appearance, const std::vector<Worn>& worn); // render thread: captured avatar dressing
    // One interface frame: OnUpdate scripts, layout, draw. The result is valid until the next call.
    ImDrawData* frame(float elapsedSeconds, const std::function<void(ImDrawList*)>& overlay = {});   // overlay: drawn last
    // Where the pointer is, in window pixels from the top left - what GetCursorPosition answers (FrameXML's MouseIsOver:
    // the chat's tabs come up under it). Give it with the engine's dispatchMouse.
    void pointer(float x, float y);

    lua_State* lua();
    ImGuiContext* imguiContext();
    int fontsLoaded() const;        // of the five faces FrameXML names (FRIZQT, MORPHEUS, SKURRI, ARIALN, FRIENDS)   // for the host's renderer backend (init it, and call its NewFrame, in this context)
    wowee::addons::LuaEngine& engine();
    float width() const { return width_; }
    float height() const { return height_; }

    struct Impl;
    Impl* impl() { return impl_.get(); }   // for the session's own Lua bindings

private:
    std::unique_ptr<Impl> impl_;
    float width_, height_;
};
}  // namespace wowcore
