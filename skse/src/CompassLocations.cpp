#include "CompassLocations.h"
#include "Frame.h"

namespace CompassLocations {
namespace {
    std::vector<RE::FormID> g_ids;
    double g_next=0;
    RE::TESWorldSpace* MapWorld(RE::TESWorldSpace* world) {
        for (int depth=0; world && world->parentWorld && depth<16 &&
            world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseSkyCell,
                RE::TESWorldSpace::ParentUseFlag::kUseLODData); ++depth) world=world->parentWorld;
        return world;
    }
}
void Reset() { g_ids.clear(); g_next=0; }
std::vector<Location> Snapshot(RE::PlayerCharacter* player) {
    std::vector<Location> result;
    auto* ui=RE::UI::GetSingleton();
    if (!player || !player->GetParentCell() || player->GetParentCell()->IsInteriorCell() ||
        !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return result;
    if (Frame::Now()>=g_next) {
        g_next=Frame::Now()+10; g_ids.clear();
        // currentMapMarkers omits the undiscovered persistent references the HUD
        // still renders. The global form map has its own lock; copy IDs only.
        // Never walk attached cell arrays concurrently with the loader.
        std::vector<RE::FormID> references;
        {
            const auto [forms,lock]=RE::TESForm::GetAllForms();
            RE::BSReadLockGuard guard(lock.get());
            if (forms) for (const auto& [id,form] : *forms)
                if (form && form->GetFormType()==RE::FormType::Reference) references.push_back(id);
        }
        // Resolving extra data acquires another lock: release the form map first.
        for (const auto id:references) {
            const auto* ref=RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
            if (ref && ref->extraList.HasType(RE::ExtraDataType::kMapMarker)) g_ids.push_back(id);
        }
    }
    const auto* here=MapWorld(player->GetWorldspace());
    if (!here) return result;
    for (const auto id:g_ids) {
        auto* ref=RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
        const auto* extra=ref ? ref->extraList.GetByType<RE::ExtraMapMarker>() : nullptr;
        const auto* data=extra ? extra->mapData : nullptr;
        if (!data || data->type.get()==RE::MARKER_TYPE::kNone) continue;
        // Persistent map markers can be valid while their exterior cell is not
        // attached. GetWorldspace also uses this engine-provided saved parent.
        const auto* cell=ref->GetParentCell() ? ref->GetParentCell() : ref->GetSaveParentCell();
        if (!cell || cell->IsInteriorCell() || ref->IsDeleted() || ref->IsDisabled() ||
            MapWorld(ref->GetWorldspace())!=here) continue;
        const auto at=ref->GetPosition();
        if (!std::isfinite(at.x) || !std::isfinite(at.y)) continue;
        const std::string name=data->locationName.fullName.c_str();
        if (!name.empty()) result.push_back({id,name,at,static_cast<int>(data->type.get())});
    }
    return result;
}
}
