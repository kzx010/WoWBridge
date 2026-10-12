#include "Trainers.h"
#include "Config.h"
#include "Frame.h"
#include "LuaText.h"
#include "ui/WowUi.h"
#include "wowcore/trainer.hpp"

namespace Trainers
{
    namespace
    {
        RE::UI::Create_t* g_create = nullptr;
        std::atomic<RE::IMenu*> g_backend = nullptr;
        std::atomic<std::uint32_t> g_epoch = 0;
        std::atomic<std::uint32_t> g_backendEpoch = 0;
        std::atomic<bool> g_initialized = false;
        RE::IMenu* g_current = nullptr; // identity only; never dereferenced after UI closure
        std::uint32_t g_active = 0, g_revision = 0;
        bool g_closing = false, g_reset = false;
        double g_next = 0;
        std::string g_last;
        constexpr const char* kKeys[] = {"one_handed", "two_handed", "archery", "block", "smithing", "heavy_armor",
            "light_armor", "pickpocket", "lockpicking", "sneak", "alchemy", "speech", "alteration", "conjuration",
            "destruction", "illusion", "restoration", "enchanting"};
        constexpr const char* kNames[] = {"One-handed", "Two-handed", "Archery", "Block", "Smithing", "Heavy Armor",
            "Light Armor", "Pickpocket", "Lockpicking", "Sneak", "Alchemy", "Speech", "Alteration", "Conjuration",
            "Destruction", "Illusion", "Restoration", "Enchanting"};

        bool CanPublish()
        {
            const auto* player = RE::PlayerCharacter::GetSingleton();
            auto* ui = RE::UI::GetSingleton();
            return player && player->Is3DLoaded() && WowUi::Ready() && ui && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
        }
        RE::GPtr<RE::TrainingMenu> Backend()
        {
            if (!CanPublish()) return {};
            auto* ui = RE::UI::GetSingleton();
            auto menu = ui->GetMenu<RE::TrainingMenu>();
            return g_initialized && g_backendEpoch == g_epoch && menu && menu.get() == g_backend &&
                ui->IsMenuOpen(RE::TrainingMenu::MENU_NAME) ? menu : RE::GPtr<RE::TrainingMenu>{};
        }
        bool Reachable(RE::Actor* trainer)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!trainer || trainer->IsDead() || trainer->IsDisabled() || trainer->IsDeleted() || !trainer->Is3DLoaded() ||
                player->IsDead() || !trainer->GetParentCell() || !player->GetParentCell()) return false;
            if (trainer->GetParentCell()->IsInteriorCell() || player->GetParentCell()->IsInteriorCell()) {
                if (trainer->GetParentCell() != player->GetParentCell()) return false;
            } else if (trainer->GetWorldspace() != player->GetWorldspace()) return false;
            return trainer->GetPosition().GetDistance(player->GetPosition()) <= 320.f;
        }
        wowcore::TrainingOffer Offer(RE::TrainingMenu* menu)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto& data = menu->GetRuntimeData();
            auto* base = data.trainer ? data.trainer->GetActorBase() : nullptr;
            auto* cls = base ? base->npcClass : nullptr;
            const float value = player->AsActorValueOwner()->GetBaseActorValue(data.skill);
            if (!std::isfinite(value) || value < 0 || value > 1000000) return {-1, 0, 0, 0, -1, 0};
            const int current = static_cast<int>(value);
            // AE 52667 calls this exact engine quote with the base skill and
            // truncates it to septims before paying the trainer (not WoW's price).
            static REL::Relocation<float(std::uint32_t)> price{REL::ID(26501)};
            const float cost = price(current);
            auto* settings = RE::GameSettingCollection::GetSingleton();
            const auto* limit = settings ? settings->GetSetting("iTrainingNumAllowedPerLevel") : nullptr;
            const auto trained = player->GetInfoRuntimeData().skillTrainingsThisLevel;
            return {current, cls ? cls->data.maximumTrainingLevel : 0,
                trained <= INT32_MAX ? static_cast<int>(trained) : INT32_MAX, limit ? limit->data.i : 5,
                std::isfinite(cost) && cost >= 0 && cost < static_cast<float>(INT32_MAX) ? static_cast<int>(cost) : -1,
                player->GetGoldAmount()};
        }
        void Publish(RE::TrainingMenu* menu, bool force = false)
        {
            if (!CanPublish()) { g_last.clear(); return; }
            const auto& data = menu->GetRuntimeData();
            const int index = static_cast<int>(data.skill) - 6;
            if (!data.trainer || index < 0 || index >= 18) return;
            const auto offer = Offer(menu);
            const auto snapshot = std::format("{{session={},name={},skill={},skillName={},current={},maximum={},trained={},limit={},cost={},gold={}}}",
                g_active, LuaQuote(data.trainer->GetDisplayFullName()), LuaQuote(kKeys[index]), LuaQuote(kNames[index]),
                offer.current, offer.maximum, offer.trained, offer.limit, offer.cost, offer.gold);
            if (force || snapshot != g_last) {
                g_last = snapshot;
                WowUi::Run(std::format("__WBTrainer({}, {})", snapshot, ++g_revision));
            }
        }
        struct Callbacks : RE::FxDelegateHandler::CallbackProcessor
        {
            RE::FxDelegateHandler::CallbackFn* train = nullptr;
            void Process(const RE::GString& name, RE::FxDelegateHandler::CallbackFn* fn) override
            {
                if (std::string_view(name.c_str()) == "Train") train = fn;
            }
        };
        struct MessageHook
        {
            static RE::UI_MESSAGE_RESULTS thunk(RE::TrainingMenu* menu, RE::UIMessage& message)
            {
                if (g_backend != menu) return func(menu, message);
                using T = RE::UI_MESSAGE_TYPE;
                if (message.type == T::kUserEvent || message.type == T::kScaleformEvent) return RE::UI_MESSAGE_RESULTS::kHandled;
                const auto result = func(menu, message); // kShow resolves the native trainer/skill
                if (menu->uiMovie) menu->uiMovie->SetVisible(false);
                if (message.type == T::kShow) g_initialized = true;
                if (message.type == T::kHide || message.type == T::kForceHide) { g_initialized = false; g_backend = nullptr; ++g_epoch; }
                return result; // publication is deferred to Tick, never a UI hook/task or loading screen
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };
        struct DisplayHook
        {
            static void thunk(RE::IMenu* menu) { if (g_backend != menu) func(menu); }
            static inline REL::Relocation<decltype(thunk)> func;
        };
        RE::IMenu* Create()
        {
            auto* menu = g_create();
            if (!menu || !Config::Get().wowUi || !WowUi::Ready()) return menu;
            menu->menuFlags = RE::UI_MENU_FLAGS::kNone;
            menu->inputContext = RE::IMenu::Context::kNone;
            if (menu->uiMovie) menu->uiMovie->SetVisible(false);
            g_initialized = false; g_backendEpoch = ++g_epoch; g_backend = menu;
            return menu;
        }
        void Close()
        {
            if (g_closing) return;
            g_closing = true;
            RE::UIMessageQueue::GetSingleton()->AddMessage(RE::TrainingMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
    }
    void Init()
    {
        if (REL::Module::get().version() != REL::Version{1, 7, 104, 0}) {
            logger::warn("trainer: unaudited runtime; native training retained"); return;
        }
        REL::Relocation<std::uintptr_t> table{RE::VTABLE_TrainingMenu[0]};
        const auto train = REL::Relocation<std::uintptr_t>{REL::ID(52667)}.address();
        const auto price = REL::Relocation<std::uintptr_t>{REL::ID(26501)}.address();
        if (*reinterpret_cast<const std::uintptr_t*>(table.address() + 4 * 8) != REL::Relocation<std::uintptr_t>{REL::ID(52662)}.address() ||
            std::memcmp(reinterpret_cast<const void*>(train), "\x48\x8B\xC4\x57\x48\x83\xEC\x70", 8) != 0 ||
            std::memcmp(reinterpret_cast<const void*>(price), "\x3B\x0D\x0A\x6B\xC8\x01", 6) != 0) {
            logger::error("trainer: native ABI mismatch; native training retained"); return;
        }
        auto* ui = RE::UI::GetSingleton();
        RE::BSSpinLockGuard lock(ui->processMessagesLock);
        const auto it = ui->menuMap.find(RE::BSFixedString(RE::TrainingMenu::MENU_NAME));
        if (it == ui->menuMap.end() || !it->second.create) { logger::error("trainer: no TrainingMenu factory"); return; }
        g_create = it->second.create; it->second.create = Create;
        MessageHook::func = table.write_vfunc(4, MessageHook::thunk);
        DisplayHook::func = table.write_vfunc(6, DisplayHook::thunk);
        logger::info("trainer: client ClassTrainerFrame with native Train callback installed");
    }
    void Reset()
    {
        ++g_epoch; g_current = nullptr; g_active = 0; g_reset = true; g_last.clear(); g_next = 0;
        // No publication here: kPreLoadGame can already be in a loading screen.
    }
    void Tick()
    {
        if (!CanPublish()) return;
        const auto menu = Backend();
        const auto epoch = g_epoch.load();
        if (g_reset || g_current != menu.get() || (menu && epoch != g_active)) {
            if (g_current || g_reset) WowUi::Run(std::format("__WBTrainer(nil, {})", ++g_revision));
            g_reset = false; g_current = menu.get(); g_active = menu ? epoch : 0;
            g_closing = false; g_last.clear(); g_next = 0;
        }
        if (!menu || g_closing) return;
        const auto& data = menu->GetRuntimeData();
        if (!Reachable(data.trainer)) { Close(); return; }
        if (Frame::Now() >= g_next) { g_next = Frame::Now() + 0.25; Publish(menu.get()); }
    }
    void Command(std::string action, std::uint32_t session, int current, int cost, int trained)
    {
        SKSE::GetTaskInterface()->AddTask([action = std::move(action), session, current, cost, trained] {
            if (!CanPublish()) { g_last.clear(); return; } // acknowledge a dropped ReloadUI request on the next safe Tick
            if (!g_active || session != g_active || session != g_epoch || g_closing) return;
            const auto menu = Backend();
            if (!menu || !Reachable(menu->GetRuntimeData().trainer)) { Close(); return; }
            if (action == "close") { Close(); return; }
            if (action != "train") return;
            const auto offer = Offer(menu.get());
            if (!offer.matches(current, cost, trained)) { Publish(menu.get(), true); return; }
            Callbacks callbacks;
            menu->Accept(&callbacks); // virtual through the owning menu, no embedded/unexported call
            if (!callbacks.train) { logger::error("trainer: native Train callback missing"); Publish(menu.get(), true); return; }
            const RE::FxDelegateArgs call(RE::GFxValue(), menu.get(), menu->uiMovie.get(), nullptr, 0);
            callbacks.train(call); // AE 52664 -> 52667: payment, skill XP, counter, native notifications
            Publish(menu.get(), true);
            logger::info("trainer: Train skill={} level={} cost={} trained={}", static_cast<int>(menu->GetRuntimeData().skill), current, cost, trained);
        });
    }
}
