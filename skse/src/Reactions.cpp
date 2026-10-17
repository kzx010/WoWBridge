#include "Reactions.h"
#include "Bridge.h"
#include "Frame.h"

namespace Reactions
{
    namespace
    {
        // Skyrim.esm idles (form IDs verified in the master). Vanilla has no shrug, clap, kiss or flex idle.
        constexpr RE::FormID kWave=0x3EA32, kSalute=0xB240C, kApplaud=0xD8730, kLaugh=0x75C5F, kNervous=0x10ACB1,
            kPoint=0xFFA0C, kDance=0xF7C8A, kAttention=0xB240B;
        enum class Group { Guard, Townsfolk, Follower };
        struct Reaction { RE::FormID guard, townsfolk, follower; const char* guardLine; const char* line; };
        // 1.12 emote tokens (EmotesText.dbc names); 0 = a glance only.
        const std::map<std::string, Reaction>& Table()
        {
            static const std::map<std::string, Reaction> table{
                {"WAVE",{kWave,kWave,kWave,nullptr,nullptr}}, {"HELLO",{kWave,kWave,kWave,nullptr,nullptr}},
                {"GREET",{kWave,kWave,kWave,nullptr,nullptr}}, {"BYE",{kWave,kWave,kWave,nullptr,nullptr}},
                {"BOW",{kAttention,kWave,kSalute,nullptr,nullptr}}, {"SALUTE",{kSalute,kWave,kSalute,nullptr,nullptr}},
                {"CHEER",{0,kApplaud,kApplaud,nullptr,nullptr}}, {"APPLAUD",{0,kApplaud,kApplaud,nullptr,nullptr}},
                {"CLAP",{0,kApplaud,kApplaud,nullptr,nullptr}}, {"DANCE",{0,kApplaud,kDance,nullptr,nullptr}},
                {"FLEX",{0,kApplaud,kLaugh,nullptr,nullptr}}, {"LAUGH",{0,kLaugh,kLaugh,nullptr,nullptr}},
                {"CHICKEN",{0,kLaugh,kLaugh,nullptr,nullptr}}, {"POINT",{0,0,0,nullptr,nullptr}},
                {"RUDE",{0,kNervous,0,"Watch yourself.",nullptr}}, {"KISS",{0,kNervous,kLaugh,nullptr,nullptr}},
                {"CRY",{0,0,0,"Are you alright?","Are you alright?"}}, {"THANK",{kWave,kWave,kWave,nullptr,nullptr}},
                {"POINTED",{0,kPoint,kPoint,nullptr,nullptr}},
            };
            return table;
        }
        constexpr double kPerActor=30, kGlobal=3, kGlance=4;
        constexpr float kNear=600.f, kTargetRange=2000.f;
        std::map<RE::FormID, double> g_last;   // per NPC
        double g_lastAny=-1e9;
        std::vector<std::pair<RE::ObjectRefHandle, double>> g_glances;
        nlohmann::json g_status={{"reactions",0}};

        const char* Skip(RE::PlayerCharacter* a_player, RE::Actor* a)
        {
            if (!a || a==a_player || !a->Is3DLoaded() || a->IsDead() || a->IsDisabled()) return "unavailable";
            if (!a->HasKeywordString("ActorTypeNPC")) return "not a person";
            if (const auto it=g_last.find(a->GetFormID()); it!=g_last.end() && Frame::Now()-it->second<kPerActor) return "cooling down";
            if (a->IsInCombat() || a->IsHostileToActor(a_player)) return "hostile or fighting";
            if (a->IsCommandedActor() || a->IsOnMount() || a->IsInKillMove()) return "busy";
            if (a->AsActorState()->GetSitSleepState()!=RE::SIT_SLEEP_STATE::kNormal) return "sitting or sleeping";
            if (a->GetCurrentScene()) return "in a scene";
            if (auto* manager=RE::MenuTopicManager::GetSingleton(); manager && manager->menuOpen && manager->speaker.get().get()==a) return "in dialogue";
            return nullptr;
        }

        bool React(RE::PlayerCharacter* a_player, RE::Actor* a, const Reaction& r, nlohmann::json& out)
        {
            const auto group=a->IsPlayerTeammate() ? Group::Follower : a->IsGuard() ? Group::Guard : Group::Townsfolk;
            const auto idleID=group==Group::Guard ? r.guard : group==Group::Follower ? r.follower : r.townsfolk;
            const char* line=group==Group::Guard && r.guardLine ? r.guardLine : r.line;
            auto* process=a->GetActorRuntimeData().currentProcess;
            if (!process) { g_status["skipped"][std::format("{:08X}",a->GetFormID())]="no AI process"; return false; }
            if (process->high) {
                process->high->SetHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPE::kScript, a_player);
                g_glances.emplace_back(a->GetHandle(), Frame::Now()+kGlance);
            }
            bool played=false;
            if (idleID) if (auto* idle=RE::TESForm::LookupByID<RE::TESIdleForm>(idleID)) played=process->PlayIdle(a, idle, a_player);
            if (line) Bridge::Say(a, line);
            g_last[a->GetFormID()]=Frame::Now();
            out.push_back({{"actor",a->GetFormID()},{"name",a->GetDisplayFullName()},
                {"group",group==Group::Guard ? "guard" : group==Group::Follower ? "follower" : "townsfolk"},
                {"idle",idleID},{"played",played},{"line",line ? line : ""}});
            return true;
        }

        int Respond(RE::PlayerCharacter* a_player, RE::Actor* a_target, const std::string& a_token, const char* a_source)
        {
            const auto found=Table().find(a_token);
            g_status={{"token",a_token},{"source",a_source},{"at",Frame::Now()},{"reacted",nlohmann::json::array()},{"skipped",nlohmann::json::object()}};
            if (found==Table().end()) { g_status["skipped"]["*"]="no reaction for this emote"; return 0; }
            if (Frame::Now()-g_lastAny<kGlobal) { g_status["skipped"]["*"]="too soon after the last reaction"; return 0; }
            std::vector<RE::Actor*> chosen;
            const auto pos=a_player->GetPosition();
            if (a_target && a_target!=a_player && pos.GetDistance(a_target->GetPosition())<kTargetRange) {
                if (const char* why=Skip(a_player,a_target)) g_status["skipped"][std::format("{:08X}",a_target->GetFormID())]=why;
                else chosen.push_back(a_target);
            } else if (auto* lists=RE::ProcessLists::GetSingleton()) {
                std::vector<std::pair<float, RE::Actor*>> nearby;
                for (const auto& handle : lists->highActorHandles) {
                    const auto actor=handle.get();
                    if (!actor) continue;
                    const float d=pos.GetDistance(actor->GetPosition());
                    if (d>kNear) continue;
                    if (const char* why=Skip(a_player,actor.get())) g_status["skipped"][std::format("{:08X}",actor->GetFormID())]=why;
                    else nearby.emplace_back(d,actor.get());
                }
                std::ranges::sort(nearby,{},&std::pair<float, RE::Actor*>::first);
                for (std::size_t i=0;i<nearby.size() && i<3;++i) chosen.push_back(nearby[i].second);
            }
            int count=0;
            for (auto* actor : chosen) count+=React(a_player,actor,found->second,g_status["reacted"]);
            if (count) g_lastAny=Frame::Now();
            g_status["reactions"]=count;
            logger::info("reactions: {} {} -> {}", a_source, a_token, g_status.dump());
            return count;
        }
    }

    int OnEmote(RE::PlayerCharacter* a_player, RE::Actor* a_target, const std::string& a_token)
    {
        std::string token=a_token;
        std::ranges::transform(token,token.begin(),[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return Respond(a_player,a_target,token,"emote");
    }

    int OnChat(RE::PlayerCharacter* a_player, RE::Actor* a_target, const std::string& a_text)
    {
        // A spoken greeting counts as a wave; anything else said aloud leaves them be.
        std::string text=a_text;
        std::ranges::transform(text,text.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const char* greeting : {"hello","hi","hail","greetings","well met","good day","bye","farewell","thanks","thank you"})
            if (text==greeting || text.starts_with(std::string(greeting)+" ") || text.starts_with(std::string(greeting)+"!") ||
                text.starts_with(std::string(greeting)+","))
                return Respond(a_player,a_target,std::string_view(greeting).starts_with("th") ? "THANK" :
                    std::string_view(greeting).starts_with("b") || std::string_view(greeting).starts_with("f") ? "BYE" : "HELLO","chat");
        return 0;
    }

    void Tick()
    {
        const double now=Frame::Now();
        std::erase_if(g_glances,[now](const auto& glance) {
            if (now<glance.second) return false;
            if (const auto ref=glance.first.get(); ref)
                if (auto* actor=ref->template As<RE::Actor>())
                    if (auto* process=actor->GetActorRuntimeData().currentProcess; process && process->high)
                        process->high->SetHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPE::kScript, nullptr);
            return true;
        });
    }

    nlohmann::json Status() { return g_status; }
}
