#include "Taxi.h"
#include "Config.h"
#include "Frame.h"
#include "LuaText.h"
#include "ui/WowUi.h"

namespace Taxi
{
    namespace
    {
        struct Destination { const char* property; RE::FormID topic; };
        // CarriageSystemScript's iDestination values, not map-marker discovery.
        constexpr Destination kDestinations[] = {
            {"Whiterun",0x185D0}, {"Solitude",0x17EE6}, {"Markarth",0x185D1},
            {"Riften",0x185CF}, {"Windhelm",0x185CE}, {"Morthal",0x9E64F},
            {"Dawnstar",0x9E64E}, {"Falkreath",0x9E651}, {"Winterhold",0x9E650}
        };
        constexpr int kSource[] = {1,3,2,4,5}; // DialogueCarriageSystem driver aliases 0..4
        struct Node { std::uint32_t id; RE::TESObjectREFR* marker; int cost; std::string name; };
        struct Routes { RE::Actor* driver; RE::TESQuest* quest; int current; std::vector<Node> nodes; };
        RE::ActorHandle g_driver;
        std::atomic<std::uint32_t> g_epoch=0;
        std::uint32_t g_session=0;
        bool g_clear=false;
        std::string g_last;
        double g_next=0;

        bool Ready()
        {
            auto* ui=RE::UI::GetSingleton();
            auto* player=RE::PlayerCharacter::GetSingleton();
            return Config::Get().wowUi && WowUi::Ready() && ui && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) &&
                player && player->Is3DLoaded() && player->GetParentCell();
        }
        template<class T> T* Property(RE::BSScript::Object* script, const char* name)
        {
            const auto* value=script->GetProperty(name);
            return value && value->IsObject() && !value->IsNoneObject() ? value->Unpack<T*>() : nullptr;
        }
        bool Offered(RE::TESTopic* topic, RE::Actor* driver, RE::TESQuest* quest)
        {
            if (!topic || topic->ownerQuest!=quest || !topic->topicInfos) return false;
            for (std::uint32_t i=0;i<topic->numTopicInfos;++i) {
                auto* info=topic->topicInfos[i];
                if (info && !info->IsDeleted() && info->objConditions.IsTrue(driver,RE::PlayerCharacter::GetSingleton())) return true;
            }
            return false;
        }
        std::optional<Routes> Gather(RE::Actor* driver)
        {
            auto* player=RE::PlayerCharacter::GetSingleton();
            if (!driver || driver->IsDead() || driver->IsDisabled() || driver->IsInCombat() ||
                !driver->GetParentCell() || driver->GetParentCell()->IsInteriorCell() ||
                driver->GetPosition().GetDistance(player->GetPosition())>320) return std::nullopt;
            auto* data=RE::TESDataHandler::GetSingleton();
            auto* quest=data->LookupForm<RE::TESQuest>(0x17F01,"Skyrim.esm");
            if (!quest || !quest->IsRunning()) return std::nullopt;
            int current=0;
            for (std::uint32_t i=0;i<std::size(kSource);++i)
                if (quest->GetAliasedRef(i).get().get()==driver) { current=kSource[i]; break; }
            if (!current || !Offered(data->LookupForm<RE::TESTopic>(0x17EEF,"Skyrim.esm"),driver,quest)) return std::nullopt;
            auto* vm=RE::BSScript::Internal::VirtualMachine::GetSingleton();
            RE::BSTSmartPointer<RE::BSScript::Object> script;
            if (!vm || !vm->FindBoundObject(vm->GetObjectHandlePolicy()->GetHandleForObject(quest->GetFormType(),quest),
                "CarriageSystemScript",script) || !script || !script->IsInitialized()) return std::nullopt;
            const auto* majorFare=Property<RE::TESGlobal>(script.get(),"CarriageCost");
            const auto* minorFare=Property<RE::TESGlobal>(script.get(),"CarriageCostSmall");
            if (!majorFare || !minorFare) return std::nullopt;
            Routes routes{driver,quest,current,{}};
            for (std::uint32_t i=0;i<std::size(kDestinations);++i) {
                const auto& destination=kDestinations[i];
                auto* marker=Property<RE::TESObjectREFR>(script.get(),destination.property);
                const double cost=i<5 ? majorFare->value : minorFare->value;
                if (!marker || marker->IsDeleted() || marker->IsDisabled() || !marker->GetWorldspace() ||
                    marker->GetWorldspace()!=driver->GetWorldspace() || !std::isfinite(cost) || cost<0 || cost>INT32_MAX/10000 ||
                    !std::isfinite(marker->GetPosition().x) || !std::isfinite(marker->GetPosition().y)) return std::nullopt;
                if (static_cast<int>(i+1)!=current && !Offered(data->LookupForm<RE::TESTopic>(destination.topic,"Skyrim.esm"),driver,quest)) continue;
                auto* extra=marker->extraList.GetByType<RE::ExtraMapMarker>();
                std::string name=extra && extra->mapData ? extra->mapData->locationName.fullName.c_str() : "";
                if (name.empty()) name=destination.property;
                routes.nodes.push_back({i+1,marker,static_cast<int>(i+1)==current ? 0 : static_cast<int>(cost),std::move(name)});
            }
            if (routes.nodes.size()<2) return std::nullopt;
            return routes;
        }
        RE::ObjectRefHandle DialogueSpeaker()
        {
            auto* manager=RE::MenuTopicManager::GetSingleton();
            if (!manager) return {};
            REX::W32::EnterCriticalSection(&manager->criticalSection);
            const auto speaker=manager->menuOpen ? manager->speaker : RE::ObjectRefHandle{};
            REX::W32::LeaveCriticalSection(&manager->criticalSection);
            return speaker;
        }
        void Publish(const Routes& routes)
        {
            std::string nodes;
            for (const auto& node : routes.nodes) {
                const auto p=node.marker->GetPosition();
                nodes+=std::format("{{id={},marker={},name={},x={:.6f},y={:.6f},cost={},current={}}},",
                    node.id,node.marker->GetFormID(),LuaQuote(node.name),p.x/64.0,p.y/64.0,node.cost,static_cast<int>(node.id)==routes.current ? "true" : "false");
            }
            const auto text=std::format("{{session={},name={},worldspace={},nodes={{{}}}}}",g_session,
                LuaQuote(routes.driver->GetDisplayFullName()),LuaQuote(routes.driver->GetWorldspace()->GetFormEditorID()),nodes);
            if (text==g_last) return;
            g_last=text; WowUi::Run("__WBTaxi("+text+")");
        }
        void Close()
        {
            g_driver={}; g_last.clear(); g_next=0; ++g_session; ++g_epoch;
            WowUi::Run("__WBTaxi(nil)");
        }
        void Error(const char* message)
        {
            WowUi::Run("__WBFire('UI_ERROR_MESSAGE',"+LuaQuote(message)+")");
            g_last.clear(); // acknowledge a refused click, clearing Lua's pending latch
        }
    }
    bool Open(RE::Actor* driver)
    {
        if (!Ready()) return false;
        const auto routes=Gather(driver);
        if (!routes) return false; // no menu suppression without complete live data
        const auto speaker=DialogueSpeaker();
        const auto handle=driver->CreateRefHandle();
        if ((speaker && speaker!=handle) ||
            (RE::UI::GetSingleton()->IsMenuOpen(RE::DialogueMenu::MENU_NAME) && speaker!=handle)) return false;
        ++g_session; ++g_epoch; g_driver=driver->CreateRefHandle(); g_last.clear(); g_clear=false; g_next=0;
        Publish(*routes);
        if (RE::UI::GetSingleton()->IsMenuOpen(RE::DialogueMenu::MENU_NAME))
            RE::UIMessageQueue::GetSingleton()->AddMessage(RE::DialogueMenu::MENU_NAME,RE::UI_MESSAGE_TYPE::kForceHide,nullptr);
        return true;
    }
    void Reset() { ++g_epoch; ++g_session; g_driver={}; g_last.clear(); g_clear=true; g_next=0; }
    void Tick()
    {
        if (!Ready()) return;
        if (g_clear) { g_clear=false; WowUi::Run("__WBTaxi(nil)"); }
        if (g_driver) {
            // Hide only this driver's deferred native show. A Courier/other
            // NPC can take dialogue ownership while the carriage map is open;
            // yield the map before its native menu receives input.
            const auto speaker=DialogueSpeaker();
            const bool dialogue=RE::UI::GetSingleton()->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
            if ((speaker && speaker!=g_driver) || (dialogue && speaker!=g_driver)) { Close(); return; }
            if (dialogue)
                RE::UIMessageQueue::GetSingleton()->AddMessage(RE::DialogueMenu::MENU_NAME,RE::UI_MESSAGE_TYPE::kForceHide,nullptr);
            if (Frame::Now()<g_next) return;
            g_next=Frame::Now()+0.5;
            const auto driver=g_driver.get();
            const auto routes=Gather(driver.get());
            if (!routes || RE::PlayerCharacter::GetSingleton()->IsDead() || RE::PlayerCharacter::GetSingleton()->IsInCombat()) Close();
            else Publish(*routes);
            return;
        }
        // E/ActivatePickRef has already built the native, condition-filtered
        // dialogue list. Intercept the carriage service while it still offers
        // its hire topic, before selecting it can show the destination submenu.
        auto* manager=RE::MenuTopicManager::GetSingleton();
        if (!manager) return;
        RE::ObjectRefHandle speaker;
        bool offered=false;
        // The native UI callbacks mutate this list under the same lock. Copy
        // the handle/decision, never retain Dialogue* beyond the lock.
        REX::W32::EnterCriticalSection(&manager->criticalSection);
        if (manager->menuOpen && manager->dialogueList) {
            speaker=manager->speaker;
            for (auto* dialogue : *manager->dialogueList) {
                if (dialogue && dialogue->parentTopic && dialogue->parentTopic->GetFormID()==0x17EEF) { offered=true; break; }
            }
        }
        REX::W32::LeaveCriticalSection(&manager->criticalSection);
        const auto ref=speaker.get();
        if (offered && ref) Open(ref->As<RE::Actor>());
    }
    void Command(std::string action,std::uint32_t destination,std::uint32_t session,int cost)
    {
        SKSE::GetTaskInterface()->AddTask([action=std::move(action),destination,session,cost,epoch=g_epoch.load()] {
            if (epoch!=g_epoch || session!=g_session || !g_driver) return;
            if (action=="close") {
                if (Ready()) Close();
                else Reset(); // retain the close until loaded play can publish it
                return;
            }
            if (action!="travel") return;
            if (!Ready()) {
                // Loading/ReloadUI can begin after Lua queued this click. Drop
                // the travel, but acknowledge it when publication resumes so
                // the unchanged snapshot cannot leave taxiPending latched.
                g_last.clear(); g_next=0;
                return;
            }
            const auto driver=g_driver.get();
            const auto routes=Gather(driver.get());
            if (!routes) { Close(); return; }
            const auto node=std::ranges::find(routes->nodes,destination,&Node::id);
            auto* player=RE::PlayerCharacter::GetSingleton();
            if (node==routes->nodes.end() || static_cast<int>(destination)==routes->current || node->cost!=cost) {
                Error("That carriage route or fare has changed."); Publish(*routes); return;
            }
            if (player->IsDead() || player->IsInCombat() || player->IsOverEncumbered()) {
                Error("You cannot take a carriage while dead, in combat or overencumbered."); Publish(*routes); return;
            }
            if (player->GetGoldAmount()<cost) { Error("You don't have enough gold."); Publish(*routes); return; }
            auto* vm=RE::BSScript::Internal::VirtualMachine::GetSingleton();
            auto* gold=RE::TESForm::LookupByID<RE::TESBoundObject>(15);
            if (!vm || !gold) { Error("Skyrim's carriage travel is unavailable."); Publish(*routes); return; }
            // The shipped CarriageSystemScript uses precisely Game.FastTravel
            // on this property, with RemoveItem(Gold, fare). Do not MoveTo:
            // FastTravel owns loading, elapsed game time and follower travel.
            if (cost) player->RemoveItem(gold,cost,RE::ITEM_REMOVE_REASON::kRemove,nullptr,nullptr);
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;
            if (!vm->DispatchStaticCall("Game","FastTravel",RE::MakeFunctionArguments(static_cast<RE::TESObjectREFR*>(node->marker)),result)) {
                if (cost) player->AddObjectToContainer(gold,nullptr,cost,nullptr);
                Error("Skyrim could not start carriage travel."); Publish(*routes); return;
            }
            logger::info("taxi: {} -> {} for {} septims",driver->GetFormID(),node->name,cost);
            Close(); // also invalidates duplicate/stale clicks before another task runs
        });
    }
}
