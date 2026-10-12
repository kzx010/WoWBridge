#include "Mounts.h"
#include "Avatar.h"
#include "Bags.h"
#include "Carriers.h"
#include "Cast.h"
#include "Config.h"
#include "Forms.h"
#include "Frame.h"
#include "Melee.h"
#include "LuaText.h"
#include "SkyrimSpells.h"
#include "ui/WowUi.h"

namespace Mounts {
namespace {
    using json = nlohmann::json;
    constexpr std::uint32_t kBuff = 0xF0000006;
    json g_catalog = json::array(), g_selected, g_pending;
    RE::NiPoint3 g_start;
    double g_since = 0, g_publish = 0, g_refresh = 0;
    bool g_catalogSent = false;
    bool Moving() {
        const auto* controls=RE::PlayerControls::GetSingleton();
        return controls && (controls->data.moveInputVec.Length()>.01f || controls->data.autoMove);
    }
    void Speed(RE::PlayerCharacter* player) {
        // Short engine backstop: saving/restarting never leaves a session-only
        // mount's bonus behind. Renew only this owned effect while mounted.
        Carriers::Buff(player,kBuff,g_selected.at("name"),{{0,100,RE::ActorValue::kSpeedMult}},2);
        g_refresh=Frame::Now()+1;
    }
    std::string Unavailable(RE::PlayerCharacter* player, bool starting) {
        if (!player || !player->Is3DLoaded() || !player->GetParentCell()) return "The player is not ready.";
        if (!Config::Get().wowUi) return "Enable the WoW interface to use mounts.";
        if (player->IsDead()) return "You cannot mount while dead.";
        if (player->GetParentCell()->IsInteriorCell()) return "Mounts require an outdoor area.";
        if (Forms::Current() != "none") return "Return to humanoid form before mounting.";
        if (player->IsOnMount()) return "Dismount the Skyrim horse first.";
        if (player->AsActorState()->IsSwimming()) return "You cannot mount while swimming.";
        if (player->IsSneaking()) return "Leave stealth before mounting.";
        if (player->AsActorState()->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) return "Stand up before mounting.";
        if (starting && (player->IsInCombat() || Melee::Attacking())) return "You cannot mount during combat.";
        if (starting && (player->IsInMidair() || player->IsMoving() || Moving())) return "Stand still to mount.";
        if (starting && (Cast::Casting() || SkyrimSpells::Casting())) return "Finish casting before mounting.";
        return {};
    }
    void Publish() {
        if (!WowUi::Ready()) return;
        if (!g_catalogSent) {
            WowUi::Run("__WBMountCatalog("+Bags::ItemLua(g_catalog)+")");
            g_catalogSent = true;
        }
        WowUi::Run("__WBMountState("+Bags::ItemLua(Snapshot())+")");
        g_publish = Frame::Now() + 1;
    }
}
void Init() {
    try {
        std::ifstream in("Data/SKSE/Plugins/WoWBridge/mounts.json");
        json data; in >> data;
        if (data.value("version",0) != 1) throw std::runtime_error("unsupported mount catalog");
        std::set<std::string> keys;
        for (const auto& row : data.at("mounts")) {
            const auto key = row.at("key").get<std::string>();
            if (key.empty() || key.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string::npos ||
                !keys.insert(key).second || row.at("speed_percent") != 100 ||
                !row.at("display").get<std::uint32_t>() || row.at("cast_ms") != 3000) throw std::runtime_error("invalid epic mount");
        }
        g_catalog = data.at("mounts");
        logger::info("mounts: {} epic ground mounts",g_catalog.size());
    } catch (const std::exception& e) { logger::error("mounts: {}",e.what()); }
}
json Snapshot() { return {{"active",g_selected.is_object() ? g_selected : json(nullptr)},
    {"pending",g_pending.is_object() ? g_pending : json(nullptr)}}; }
bool Pending() { return g_pending.is_object(); }
void Dismount() {
    const bool casting = Pending();
    const auto spell = casting ? g_pending.at("spell").get<std::uint32_t>() : 0;
    if (!casting && !g_selected.is_object()) return;
    g_pending = nullptr; g_selected = nullptr;
    Avatar::SetMount(0);
    if (auto* player=RE::PlayerCharacter::GetSingleton(); player && player->Is3DLoaded()) Carriers::Unbuff(player,kBuff);
    if (casting && WowUi::Ready()) WowUi::Run(std::format("if __WB.casting and __WB.casting.spell=={} then __WBCastInterrupted() end",spell));
    Publish();
}
void Reset() { Dismount();
    if (auto* player=RE::PlayerCharacter::GetSingleton(); player && player->Is3DLoaded()) Carriers::Unbuff(player,kBuff);
    g_catalogSent=false; g_publish=0; }
bool Use(std::uint32_t spell) {
    const auto row=std::ranges::find_if(g_catalog,[&](const auto& r) { return r.at("spell")==spell; });
    if (row==g_catalog.end()) return false;
    std::string message;
    if (!Select(row->at("key"),message)) WowUi::Run("__WBFire('UI_ERROR_MESSAGE',"+LuaQuote(message)+")");
    return true;
}
bool Select(const std::string& key, std::string& message) {
    if (key=="none") { Dismount(); message="Dismounted."; return true; }
    const auto row=std::ranges::find_if(g_catalog,[&](const auto& r) { return r.at("key").template get<std::string>()==key; });
    if (row==g_catalog.end()) { message="Choose an epic mount from the Mounts testing page."; return false; }
    auto* player=RE::PlayerCharacter::GetSingleton();
    if (g_selected.is_object() && g_selected.at("key")==key) { Dismount(); message="Dismounted."; return true; }
    message=Unavailable(player,true);
    if (!message.empty()) return false;
    Dismount();
    g_pending=*row; g_start=player->GetPosition(); g_since=Frame::Now();
    Avatar::PrepareMount(row->at("display"));
    Avatar::SetWeaponsDrawn(false); Avatar::StopActing();
    Publish();
    WowUi::Run(std::format("__WBCastStart({},3000)",row->at("spell").get<std::uint32_t>()));
    message="Preparing "+row->at("name").get<std::string>()+"; stand still for 3 seconds.";
    return true;
}
void Tick() {
    auto* ui=RE::UI::GetSingleton();
    if (!ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return;
    auto* player=RE::PlayerCharacter::GetSingleton();
    if (Pending() || g_selected.is_object()) {
        auto reason=Unavailable(player,false);
        if (reason.empty() && Pending() && (player->GetPosition().GetDistance(g_start)>6 || player->IsMoving() || Moving() || player->IsInMidair() ||
            player->IsInCombat() || Cast::Casting() || SkyrimSpells::Casting())) reason="Mount preparation interrupted.";
        if (!reason.empty()) { Dismount(); return; }
        if (Pending() && Frame::Now()-g_since >= 3 && Avatar::MountReady(g_pending.at("display"))) {
            g_selected=g_pending; g_pending=nullptr;
            Speed(player);
            Avatar::SetMount(g_selected.at("display"));
            WowUi::Run("__WBCastStop()"); Publish();
        } else if (Pending() && Frame::Now()-g_since>10) { Dismount(); logger::warn("mounts: asset preparation timed out"); }
    }
    if (g_selected.is_object() && Frame::Now()>=g_refresh) Speed(player);
    if (Frame::Now()>=g_publish) Publish();
}
}
