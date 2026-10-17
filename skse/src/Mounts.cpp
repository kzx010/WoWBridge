#include "Mounts.h"
#include "Audio.h"
#include "Avatar.h"
#include "Bags.h"
#include "Carriers.h"
#include "Cast.h"
#include "Character.h"
#include "Config.h"
#include "Forms.h"
#include "Frame.h"
#include "Melee.h"
#include "Picker.h"
#include "Progression.h"
#include "Service.h"
#include "LuaText.h"
#include "SkyrimSpells.h"
#include "ui/WowUi.h"

namespace Mounts {
namespace {
    using json = nlohmann::json;
    constexpr std::uint32_t kBuff = 0xF0000006;
    json g_catalog = json::array(), g_selected, g_pending;
    std::set<std::string> g_learned;
    bool g_recorded = false;   // this save's record was read
    RE::NiPoint3 g_start;
    double g_since = 0, g_publish = 0, g_refresh = 0;
    bool g_catalogSent = false;
    std::uint64_t g_loop = 0;   // the precast sound
    // TBC's flying mounts. In the air their position is driven each frame from where flight last put the player (Skyrim's own
    // movement for that frame discarded), the character controller held in its Flying state at rest - the path
    // wow_flight_kinematic proved: climb, hover and descent followed exactly, no fall damage. A ray along each step
    // keeps the player out of terrain and buildings; meeting the ground ends flight on the player's feet, still mounted.
    bool g_flying = false;
    RE::NiPoint3 g_flightAt;
    double g_flightTime = 0;
    float g_gravity = 1;
    bool g_jumpWas = false;
    bool Flyer() { return g_selected.is_object() && g_selected.value("flying", false); }
    bool KeyDown(std::uint32_t a_scan) {
        const UINT scan = a_scan & 0x80 ? 0xE000 | (a_scan & 0x7F) : a_scan;
        return a_scan && (GetAsyncKeyState(MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX)) & 0x8000);
    }
    bool Climbing() {   // Skyrim's jump key (WoW's Space); in the air it climbs
        const auto key = RE::ControlMap::GetSingleton()->GetMappedKey(RE::UserEvents::GetSingleton()->jump, RE::INPUT_DEVICE::kKeyboard);
        return key != RE::ControlMap::kInvalid && KeyDown(key);
    }
    bool Descending() { return KeyDown(0x2D); }   // X, as TBC's "Sit/Move Down"
    void HoldController(RE::PlayerCharacter* player, bool a_flying) {
        auto* c = player->GetCharController();
        if (!c || !c->GetHavokWorld()) return;
        using State = RE::hkpCharacterStateTypes;
        RE::BSWriteLockGuard guard(c->GetHavokWorld()->worldLock);
        c->fallTime = 0; c->fallStartHeight = player->GetPositionZ();
        if (a_flying) {
            c->gravity = 0;
            c->context.currentState = State::kFlying; c->wantState = State::kFlying;
            c->SetLinearVelocityImpl(RE::hkVector4(0, 0, 0, 0));
        } else {   // let go just above the ground: the player settles from there, nothing to fall from
            c->gravity = g_gravity;
            c->context.currentState = State::kInAir; c->wantState = State::kInAir;
        }
    }
    void Land(RE::PlayerCharacter* player) {
        if (!g_flying) return;
        g_flying = false;
        HoldController(player, false);
        Avatar::SetMountFlying(false);
    }
    void TakeOff(RE::PlayerCharacter* player) {
        auto* c = player->GetCharController();
        if (!c) return;
        g_gravity = c->gravity > 0 ? c->gravity : 1;
        g_flying = true; g_flightAt = player->GetPosition(); g_flightTime = Frame::Now();
        Avatar::SetMountFlying(true);
    }
    void Fly(RE::PlayerCharacter* player) {
        const double now = Frame::Now();
        const float dt = static_cast<float>(std::clamp(now - g_flightTime, 0.0, 0.1));
        g_flightTime = now;
        if (player->GetPosition().GetDistance(g_flightAt) > 10 * Avatar::kUnitsPerYard) g_flightAt = player->GetPosition();   // moved by the game
        const auto* controls = RE::PlayerControls::GetSingleton();
        RE::NiPoint2 input = controls ? controls->data.moveInputVec : RE::NiPoint2{};
        if (controls && controls->data.autoMove) input.y = 1;
        // Forward along their look, climbing as the player looks up (TBC); strafing stays level.
        const float yaw = player->GetAngleZ(), pitch = player->GetAngleX();
        const RE::NiPoint3 forward{ std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch) };
        const RE::NiPoint3 right{ std::cos(yaw), -std::sin(yaw), 0 };
        RE::NiPoint3 move = forward * input.y + right * input.x;
        if (const float length = move.Length(); length > 1) move = move / length;
        const float up = (Climbing() ? 1.0f : 0.0f) - (Descending() ? 1.0f : 0.0f);
        move.z += up * 0.6f;
        // WoW's run, 7 yards a second, with the mount's flying bonus (280%: 26.6 yards a second).
        const float speed = 7.0f * (1 + g_selected.value("flight_speed_percent", 280) / 100.0f) * Avatar::kUnitsPerYard;
        const RE::NiPoint3 chest{ 0, 0, 64 };
        auto to = g_flightAt + move * (speed * dt);
        if (const auto step = to - g_flightAt; step.Length() > 0.01f) {
            const auto dir = step / step.Length();
            const auto hit = Picker::Cast(g_flightAt + chest, to + chest + dir * 48, true, player);
            if (hit.hit) to = g_flightAt + dir * std::max(0.0f, hit.pos.GetDistance(g_flightAt + chest) - 48);
        }
        // Down onto the ground (not climbing): flight ends on the player's feet.
        if (up <= 0 && to.z <= g_flightAt.z + 1) {
            const auto ground = Picker::Cast(to + chest, to - RE::NiPoint3{ 0, 0, 24 }, true, player);
            if (ground.hit) {
                player->SetPosition(ground.pos + RE::NiPoint3{ 0, 0, 2 }, true);
                Land(player);
                return;
            }
        }
        g_flightAt = to;
        HoldController(player, true);
        player->SetPosition(to, true);
    }
    bool Moving() {
        const auto* controls=RE::PlayerControls::GetSingleton();
        return controls && (controls->data.moveInputVec.Length()>.01f || controls->data.autoMove);
    }
    // A class mount (the Paladin's Warhorse and Charger) is a class spell the player knows; the others come from their reins.
    bool Known(const json& a_row) {
        return a_row.contains("class") ? Progression::Knows(a_row.at("spell").get<std::uint32_t>())
                                       : g_learned.contains(a_row.at("key").get<std::string>());
    }
    void Speed(RE::PlayerCharacter* player) {
        // Short engine backstop: saving/restarting never leaves a session-only
        // mount's bonus behind. Renew only this owned effect while mounted.
        // Aura 32's percent as SpeedMult points (the Warhorse's 60, the epic mounts' 100). Unit::UpdateSpeed: mounted, the
        // mount's speed is the main modifier and Pursuit of Justice's mounted share the not-stacking multiplier; their
        // on-foot share (Forms' talent buff, still on the player) is taken back out while the player rides.
        const float mount = g_selected.value("speed_percent", 100.0f);
        const float points = (100.0f + mount) * (1.0f + Forms::MountedSpeedPercent() / 100.0f) - 100.0f - Forms::RunSpeedPercent();
        Carriers::Buff(player,kBuff,g_selected.at("name"),{{0,points,RE::ActorValue::kSpeedMult}},2);
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
        if (!g_catalogSent) {   // every mount (the flyers' spells, their items' tooltips), the learned ones marked
            auto rows=g_catalog;
            for (auto& row : rows) row["learned"]=Known(row);
            WowUi::Run("__WBMountCatalog("+Bags::ItemLua(rows)+")");
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
            const auto speed = row.at("speed_percent").get<int>();
            if (key.empty() || key.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string::npos ||
                !keys.insert(key).second || (speed != 100 && !(row.contains("class") && speed == 60)) ||
                !row.at("display").get<std::uint32_t>() || row.at("cast_ms") != 3000) throw std::runtime_error("invalid mount " + key);
        }
        g_catalog = data.at("mounts");
        logger::info("mounts: {} mounts",g_catalog.size());
    } catch (const std::exception& e) { logger::error("mounts: {}",e.what()); }
}
json Snapshot() { return {{"active",g_selected.is_object() ? g_selected : json(nullptr)},
    {"pending",g_pending.is_object() ? g_pending : json(nullptr)}}; }
bool Pending() { return g_pending.is_object(); }
void Dismount() {
    const bool casting = Pending();
    const auto spell = casting ? g_pending.at("spell").get<std::uint32_t>() : 0;
    if (!casting && !g_selected.is_object()) return;
    if (auto* player = RE::PlayerCharacter::GetSingleton(); player && g_flying) Land(player);   // in the air: the player falls, as in TBC
    g_pending = nullptr; g_selected = nullptr;
    if (casting) { Audio::Stop(g_loop); g_loop = 0; Avatar::StopActing(); }
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
    if (!Known(*row)) { message="Spell not learned"; return false; }   // SPELL_FAILED_NOT_KNOWN
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
    // The client's summoning precast (every epic mount's kit 358): a rustle of tack and their precast pose. A flyer has no
    // 1.12 spell: it summons as the swift ground mounts do (Swift Frostsaber's kits).
    auto kit=WowUi::Sounds(row->at("spell").get<std::uint32_t>());
    if (!kit.precast) kit=WowUi::Sounds(23221);
    g_loop=WowUi::PlaySound(kit.precast); Avatar::Act(kit.precastAnim,true);
    if (row->contains("class")) {
        // Their class mount costs mana (Summon Warhorse 100, Charger 150): the service prices it in Skyrim magicka for their
        // skills (skyrim_balance PALADIN) and refuses what the player cannot cast; it is taken when the summon completes
        // (Spell::cast's TakePower), so an interrupted summon costs nothing.
        const auto spell=row->at("spell").get<std::uint32_t>();
        Service::Request({{"op","resolve"},{"spell",spell},{"caster",Character::Caster()},{"target",{{"kind","self"}}}},[spell](const json& r) {
            if (!Pending() || g_pending.at("spell")!=spell) return;
            auto error=r.value("ok",false) ? std::string{} : r.value("error",std::string("SPELL_FAILED_ERROR"));
            auto* player=RE::PlayerCharacter::GetSingleton();
            if (error.empty() && player && player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kMagicka)<r.value("cost",0.0f))
                error="ERR_OUT_OF_MANA";
            if (!error.empty()) {
                WowUi::Run("__WBCastFailed("+LuaQuote(error=="SERVICE_OFFLINE" ? "SPELL_FAILED_ERROR" : error)+")");
                Dismount();
                return;
            }
            g_pending["skyrim_cost"]=r.value("cost",0.0f);
        });
    }
    message="Preparing "+row->at("name").get<std::string>()+"; stand still for 3 seconds.";
    return true;
}
void Save(SKSE::SerializationInterface* a_intfc) {
    const auto text=json(LearnedKeys()).dump();
    if (!a_intfc->OpenRecord(kRecord,kRecordVersion) || !a_intfc->WriteRecordData(text.data(),static_cast<std::uint32_t>(text.size())))
        logger::error("mounts: their learned mounts were not saved");
}
void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length) {
    g_recorded=true;   // even damaged: a save of this build, never migrated
    std::string text(a_length,'\0');
    if (!a_length || a_length>65536 || a_intfc->ReadRecordData(text.data(),a_length)!=a_length) { logger::error("mounts: a damaged record"); return; }
    const auto keys=json::parse(text,nullptr,false);
    if (!keys.is_array()) { logger::error("mounts: an unreadable record"); return; }
    for (const auto& key : keys) if (key.is_string()) g_learned.insert(key.get<std::string>());   // unknown keys stay, unused
}
void PrepareLoad() { g_learned.clear(); g_recorded=false; g_catalogSent=false; }
void Migrate(bool a_played) {
    if (g_recorded || !a_played) return;
    for (const auto& key : Keys()) g_learned.insert(key);
    logger::info("mounts: a save from before learned mounts keeps all {}",g_learned.size());
}
bool Learned(std::string_view a_key) { return g_learned.contains(std::string(a_key)); }
bool Learn(std::string_view a_key) {
    const auto row=std::ranges::find_if(g_catalog,[&](const auto& r) { return r.at("key").template get<std::string>()==a_key; });
    if (row==g_catalog.end() || !g_learned.insert(std::string(a_key)).second) return false;
    g_catalogSent=false; Publish();
    // The client's learning line (ERR_LEARN_SPELL_S or, for an ability as the mounts are, ERR_LEARN_ABILITY_S).
    WowUi::Run(std::format("__WBLearnedSpell({})",row->at("spell").get<std::uint32_t>()));
    return true;
}
std::vector<std::string> LearnedKeys() { return {g_learned.begin(),g_learned.end()}; }
void SetLearned(std::vector<std::string> a_keys) {
    g_learned={a_keys.begin(),a_keys.end()};
    if (g_selected.is_object() && !g_learned.contains(g_selected.at("key").get<std::string>())) Dismount();
    g_catalogSent=false; Publish();
}
std::vector<std::string> Keys() {
    std::vector<std::string> out;
    for (const auto& row : g_catalog) out.push_back(row.at("key").get<std::string>());
    return out;
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
        const bool priced=!g_pending.is_object() || !g_pending.contains("class") || g_pending.contains("skyrim_cost");
        if (Pending() && Frame::Now()-g_since >= 3 && priced && Avatar::MountReady(g_pending.at("display"))) {
            if (const auto cost=g_pending.value("skyrim_cost",0.0f); cost>0) {
                auto* av=player->AsActorValueOwner();
                if (av->GetActorValue(RE::ActorValue::kMagicka)<cost) {
                    WowUi::Run("__WBCastFailed(\"ERR_OUT_OF_MANA\")");
                    Dismount();
                    return;
                }
                av->DamageActorValue(RE::ActorValue::kMagicka,cost);
            }
            g_selected=g_pending; g_pending=nullptr;
            Audio::Stop(g_loop); g_loop=0; Avatar::StopActing();
            const auto call=WowUi::Sounds(g_selected.at("spell").get<std::uint32_t>()).cast;   // its call: SummonTiger, HorseSummon...
            WowUi::PlaySound(call ? call : g_selected.value("summon_sound",0u));   // ...or a flyer's own (GryphonAggro, WyvernAggro)
            Speed(player);
            Avatar::SetMount(g_selected.at("display"));
            WowUi::Run("__WBCastStop()"); Publish();
        } else if (Pending() && Frame::Now()-g_since>10) { Dismount(); logger::warn("mounts: asset preparation timed out"); }
    }
    if (g_selected.is_object() && Frame::Now()>=g_refresh) Speed(player);
    if (Flyer()) {   // Space (or falling) takes a flying mount up; then it flies until it meets the ground
        const bool jump = Climbing();
        if (!g_flying && ((jump && !g_jumpWas) || player->IsInMidair())) TakeOff(player);
        g_jumpWas = jump;
        if (g_flying) Fly(player);
    }
    if (Frame::Now()>=g_publish) Publish();
}
}
