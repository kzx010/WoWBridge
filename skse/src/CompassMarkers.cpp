#include "CompassMarkers.h"

namespace CompassMarkers {
namespace {
    using json=nlohmann::json;
    std::mutex g_lock;
    json g_snapshot={{"ready",false},{"markers",json::array()}};
    std::chrono::steady_clock::time_point g_at;
    std::atomic<bool> g_pending=false;
    const json& Manifest() {
        static const json data=[] {
            try { std::ifstream in("Data/SKSE/Plugins/WoWBridge/Icons/compass.json"); auto data=json::parse(in);
                if (!data.is_object() || !data.contains("version") || !data["version"].is_number_integer() ||
                    !data.contains("nonempty_frames") || !data["nonempty_frames"].is_array() ||
                    !data.contains("labels") || !data["labels"].is_object()) return json::object();
                for (const auto* label : {"Quest","QuestDoor","PlayerSet","Enemy","LocationMarkers","UndiscoveredMarkers"}) {
                    if (!data["labels"].contains(label) || !data["labels"][label].is_number_integer()) return json::object();
                }
                return data; }
            catch (...) { return json::object(); }
        }();
        return data;
    }
}

json Snapshot() {
    std::lock_guard guard(g_lock);
    if (std::chrono::steady_clock::now()-g_at>std::chrono::seconds(1)) return {{"ready",false},{"markers",json::array()}};
    return g_snapshot;
}

void Update(bool rendered) {
    if (g_pending.exchange(true)) return;
    SKSE::GetTaskInterface()->AddUITask([rendered] {
        json out={{"ready",false},{"markers",json::array()}};
        auto* ui=RE::UI::GetSingleton(); auto hud=ui ? ui->GetMenu<RE::HUDMenu>() : nullptr;
        RE::GFxValue root, data, holder, compass;
        bool valid=hud && hud->uiMovie && ui && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
        if (valid) root=hud->GetRuntimeData().root;
        valid=valid && root.IsObject() && root.GetMember("CompassTargetDataA",&data) && data.IsArray() && data.GetArraySize()%4==0 && data.GetArraySize()<=4096;
        const auto& manifest=Manifest();
        valid=valid && manifest.value("version",0)==1 && manifest.contains("nonempty_frames") && manifest.contains("labels");
        if (valid) {
            for (const auto& [label,member] : {std::pair{"Quest","CompassMarkerQuest"},{"QuestDoor","CompassMarkerQuestDoor"},
                {"PlayerSet","CompassMarkerPlayerSet"},{"Enemy","CompassMarkerEnemy"},
                {"LocationMarkers","CompassMarkerLocations"},{"UndiscoveredMarkers","CompassMarkerUndiscovered"}}) {
                RE::GFxValue value; valid &= root.GetMember(member,&value) && value.IsNumber() && manifest["labels"].value(label,0)==value.GetNumber();
            }
        }
        if (valid) for (std::uint32_t i=0;i<data.GetArraySize();i+=4) {
            std::array<double,4> row{};
            for (std::size_t n=0;n<4;++n) { RE::GFxValue value; if (!data.GetElement(i+static_cast<unsigned>(n),&value) || !value.IsNumber() || !std::isfinite(value.GetNumber())) { valid=false; break; } row[n]=value.GetNumber(); }
            if (!valid) break;
            if (row[2]<1 || row[2]>152) { valid=false; break; }
            const int frame=static_cast<int>(row[2]);
            if (row[2]!=frame || frame<1 || frame>152 || row[1]<0 || row[1]>100 || row[3]<0 || row[3]>300 ||
                (row[1]>0 && std::ranges::find(manifest["nonempty_frames"],json(frame))==manifest["nonempty_frames"].end())) { valid=false; break; }
            out["markers"].push_back({{"bearing",row[0]},{"opacity",row[1]/100},{"frame",frame},{"scale",row[3]/100}});
        }
        out["ready"]=valid; out["native_count"]=valid ? data.GetArraySize()/4 : 0;
        // Skyrim's SetCompassAngle writes _visible every update. Alpha persists
        // through that script and is restored when the replacement is unavailable.
        bool hidden=false;
        if (root.IsObject() && root.GetMember("CompassShoutMeterHolder",&holder) && holder.IsObject() && holder.GetMember("Compass",&compass) && compass.IsObject()) {
            compass.SetMember("_alpha",RE::GFxValue(valid && rendered ? 0.0 : 100.0));
            RE::GFxValue alpha; hidden=valid && rendered && compass.GetMember("_alpha",&alpha) && alpha.IsNumber() && alpha.GetNumber()==0;
        }
        out["hidden"]=hidden;
        { std::lock_guard guard(g_lock); g_snapshot=std::move(out); g_at=std::chrono::steady_clock::now(); }
        g_pending=false;
    });
}
}
