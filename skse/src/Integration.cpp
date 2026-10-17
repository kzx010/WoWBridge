#include "Integration.h"
#include "Distribution.h"
#include "RE/S/SendHUDMessage.h"
#include "Avatar.h"
#include "Bridge.h"
#include "Character.h"
#include "Config.h"
#include "Gear.h"
#include "Input.h"
#include "LuaText.h"
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "ui/WowUi.h"

namespace Integration
{
    namespace
    {
        State g_state;
        std::atomic<bool> g_pumpQueued = false;
        std::atomic<bool> g_active = true;
        std::atomic<bool> g_chooseClass = false;
        std::atomic<std::uint64_t> g_epoch = 0;
        enum class Step { None, Offer, Class, Race, Sex, Name };
        Step g_next = Step::None;
        bool g_box = false, g_creator = false;
        bool g_waitingForControls = false;
        std::uint64_t g_customizeShown = 0; // the interface generation the customization window last opened in
        std::string g_name;
        std::uint32_t g_class = 11, g_race = 4;

        const wowcore::CharacterClass* SelectedClass()
        {
            const auto& classes = Character::Classes();
            const auto c = std::ranges::find(classes,g_class,&wowcore::CharacterClass::id);
            return c == classes.end() ? nullptr : &*c;
        }

        void SetActive(bool active)
        {
            g_active = active;
            if (!active) {
                WowUi::ClearPending();
                Input::SetCursorMode(false);
                Input::Update();
            }
            Avatar::Suspend(!active);
            Avatar::IntegrationChanged(active);
            Bridge::IntegrationChanged(active);
        }
        void Reset(Mode mode)
        {
            ++g_epoch; // late native callbacks belong to the old save
            g_state.Load(mode);
            UpdateClassChoice();
            g_next = Step::None;
            g_box = g_creator = false;
            g_waitingForControls = false; // only a successful post-load may activate the restored player
            g_name.clear();
            g_class = Character::Get().classId; g_race = Character::Looks().race;
            SetActive(false);
            g_customizeShown = 0;
            WowUi::Run("__WBCustomizeClose()"); // a window opened for the old game owes it nothing
            logger::info("integration: reset to mode {}", static_cast<std::uint32_t>(mode));
        }
        class Callback final : public RE::IMessageBoxCallback
        {
        public:
            Callback(Step step, std::uint64_t epoch) : _step(step), _epoch(epoch) {}
            void Run(std::uint8_t button) override
            {
                SKSE::GetTaskInterface()->AddTask([step = _step, epoch = _epoch, button] {
                    if (epoch != g_epoch) return;
                    g_box = false;
                    auto* ui = RE::UI::GetSingleton();
                    if (g_creator && (!ui || !ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME))) return;
                    if (step == Step::Offer) {
                        if (button == 0) g_next = CanChooseClass() ? Step::Class : Step::Race;
                        else if (button == 1) { g_state.ChooseSkyrim(); Character::OfferCustomization(false); }
                    } else if (step == Step::Class) {
                        if (button <= 1 && CanChooseClass()) { g_class = button == 0 ? 11 : 2; g_next = Step::Race; }
                        else if (ClassBack(g_state.mode,g_creator)) g_next = Step::Offer;
                    } else if (step == Step::Race) {
                        const auto* c = SelectedClass();
                        if (c && button < c->races.size()) { g_race = c->races[button]; g_next = Step::Sex; }
                        else if (CanChooseClass()) g_next = Step::Class;
                    } else if (step == Step::Sex) {
                        if (button <= 1 && g_state.ChooseSex(button == 0 ? 1 : 0)) {
                            if (!Character::SetIdentity(g_class,g_race,*g_state.selectedSex)) {
                                g_state.selectedSex.reset(); g_next = Step::Sex;
                                RE::SendHUDMessage::ShowHUDMessage("That WoWBridge body is unavailable. Choose another.");
                                return;
                            }
                            Distribution::SetClass(g_class);   // their class's books roll from now on // co-save the preset while Skyrim's opening keeps controls disabled
                            // Choosing WoWBridge (not another body for a WoWBridge save) offers WoW's character
                            // customization once the interface is up; the save keeps the offer until it is answered.
                            if (g_state.mode != Mode::WoW) Character::OfferCustomization(true);
                            if (g_creator) g_next = Step::Name;
                            else { g_state.Finish(); UpdateClassChoice(); g_waitingForControls = true; }
                        } else g_next = Step::Race;
                    }
                });
            }
        private:
            Step _step;
            std::uint64_t _epoch;
        };
        bool Box(Step step)
        {
            RE::BSString message;
            RE::BSTArray<RE::BSString> buttons;
            if (step == Step::Offer) {
                message = "Play this character with WoWBridge?\n\nWoWBridge plays Skyrim with the WoW interface. Choose a class (Druid or Paladin), one of its races (Druid: Night Elf or Tauren; Paladin: Human or Dwarf), female or male, and a name. Skin, face and hair follow in WoW's character customization once Skyrim's opening returns control to you.\n\nYou can also start later with the console command: wb start";
                buttons.push_back("Play WoWBridge");
                buttons.push_back("Use Skyrim character creation");
            } else if (step == Step::Class) {
                message = "WoWBridge: choose a class";
                for (const auto id : {11u,2u}) {
                    const auto& classes = Character::Classes();
                    const auto c = std::ranges::find(classes,id,&wowcore::CharacterClass::id);
                    if (c == classes.end()) return false;
                    buttons.push_back(c->name.c_str());
                }
                buttons.push_back(ClassBack(g_state.mode,g_creator) ? "Back" : "Cancel");
            } else if (step == Step::Race) {
                message = "WoWBridge: choose a race";
                // Names from ChrRaces, also used by the embedded creation screen.
                const wowcore::Mpq mpq(Config::ClientData());
                const auto races = wowcore::Dbc::load(mpq,R"(DBFilesClient\ChrRaces.dbc)");
                if (!races) return false;
                const auto* c = SelectedClass();
                if (!c) return false;
                for (const auto race : c->races) {
                    const auto row = races->rowOf(race);
                    if (!row) return false;
                    buttons.push_back(races->str(*row,17));
                }
                buttons.push_back(CanChooseClass() ? "Back" : "Cancel");
            } else {
                // Before WoWBridge has started, customization follows; for a WoWBridge save, a new body takes its preset look.
                const std::string body = g_state.mode != Mode::WoW ?
                    "Choose female or male. You will customize skin, face and hair in the WoW interface." :
                    "Choose female or male. The other body starts from its preset look; to change skin, face or hair, use the console command: wb appearance";
                const auto text = std::string("WoWBridge\n\n") + body + "\n\n" +
                    (g_creator ? std::string("Enter a name on the next screen (Dragonborn by default); the WoW interface shows it too.") : "Name: " + PlayerName());
                message = text.c_str();
                buttons.push_back("Female"); buttons.push_back("Male"); buttons.push_back("Back");
            }
            RE::BSTSmartPointer<RE::IMessageBoxCallback> callback(new Callback(step, g_epoch));
            return RE::MessageBoxMenu::Create(message, callback, 0, 0, 4, buttons);
        }
        void EnterName()
        {
            // Use Skyrim's own name-only dialog and ChangeName completion path.
            // No shipped SWF, forced menu close, quest stage, race change or teleport.
            const auto epoch = g_epoch.load();
            const auto name = PlayerName();
            SKSE::GetTaskInterface()->AddUITask([epoch, name] {
                if (epoch != g_epoch) return;
                auto* ui = RE::UI::GetSingleton();
                auto menu = ui ? ui->GetMenu<RE::RaceSexMenu>() : nullptr;
                if (!menu || !menu->uiMovie || !ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME)) return;
                RE::GFxValue result;
                // The creator can still be configured for its virtual keyboard.
                // ShowTextEntryField otherwise returns successfully without displaying
                // the editable field. Select the native text-entry path first.
                constexpr auto panels = "_root.RaceSexMenuBaseInstance.RaceSexPanelsInstance";
                RE::GFxValue textEntry(true);
                const auto configure = std::string(panels) + ".ShowTextEntry";
                const auto path = std::string(panels) + ".ShowTextEntryField";
                if (!menu->uiMovie->Invoke(configure.c_str(), &result, &textEntry, 1) ||
                    !menu->uiMovie->Invoke(path.c_str(), &result, nullptr, 0)) {
                    logger::warn("integration: name-only entry unavailable; finish the installed character creator normally");
                    RE::SendHUDMessage::ShowHUDMessage("WoWBridge selected. Finish your character's name in the installed creator.");
                    return;
                }
                if (!menu->uiMovie->SetVariable("_root.RaceSexMenuBaseInstance.RaceSexPanelsInstance.NameEntryInstance.TextInputInstance.text", RE::GFxValue(name.c_str())))
                    logger::warn("integration: the installed creator did not accept the default name");
                RE::GFxValue show, x, mode;
                menu->uiMovie->GetVariable(&show, (std::string(panels) + ".bShowTextEntry").c_str());
                menu->uiMovie->GetVariable(&x, (std::string(panels) + "._TextEntryField._x").c_str());
                menu->uiMovie->GetVariable(&mode, (std::string(panels) + ".Mode").c_str());
                logger::info("integration: native name-only entry opened (text={}, x={}, mode={})",
                    show.IsBool() ? static_cast<int>(show.GetBool()) : -1,
                    x.IsNumber() ? x.GetNumber() : -9999, mode.IsNumber() ? mode.GetNumber() : -9999);
            });
        }
        struct CreatorUpdate
        {
            static void thunk(RE::RaceSexMenu* menu, float interval, std::uint32_t time)
            {
                func(menu, interval, time);
                if (g_pumpQueued.exchange(true)) return;
                const auto epoch = g_epoch.load();
                SKSE::GetTaskInterface()->AddTask([epoch] {
                    g_pumpQueued = false;
                    if (epoch == g_epoch) Tick();
                });
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };
        struct MenuSink : RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (!event || event->menuName != RE::RaceSexMenu::MENU_NAME) return RE::BSEventNotifyControl::kContinue;
                const auto epoch = g_epoch.load();
                const bool opening = event->opening;
                SKSE::GetTaskInterface()->AddTask([epoch, opening] {
                    if (epoch != g_epoch) return;
                    if (opening) {
                        if (Config::Get().wowUi && !Config::Get().autorun && g_state.Offer(true)) {
                            g_creator = true; g_next = Step::Offer;
                            logger::info("integration: offering WoWBridge at character creation");
                        }
                    } else {
                        if (g_creator && g_state.Finish()) {
                            g_waitingForControls = true;
                            RE::SendHUDMessage::ShowHUDMessage("WoWBridge selected. It starts, with WoW character customization, when Skyrim returns control to you.");
                        } else if (g_state.mode == Mode::Intro) {
                            g_state.ChooseSkyrim(); // creator was completed before accepting our popup
                        }
                        UpdateClassChoice();
                        ++g_epoch; // a dismissed creator invalidates any unanswered popup callback
                        g_creator = false; g_next = Step::None; g_box = false;
                    }
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        } g_sink;
    }

    void Init()
    {
        RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&g_sink);
        REL::Relocation<std::uintptr_t> table{ RE::VTABLE_RaceSexMenu[0] };
        CreatorUpdate::func = table.write_vfunc(5, CreatorUpdate::thunk);
    }
    bool Active() { return g_active; }
    bool CanChooseClass() { return g_chooseClass.load(); }
    void UpdateClassChoice() { g_chooseClass = CanChooseClass(g_state.mode,Character::CustomizationOffered()); }
    std::string PlayerName()
    {
        const auto* player = RE::PlayerCharacter::GetSingleton();
        const char* name = player ? player->GetName() : nullptr;
        return DisplayName(name ? name : "");
    }
    void NewGame() { Reset(Mode::Intro); g_state.NewGame(); }
    void PrepareLoad() { Reset(Mode::WoW); }
    void OnGameLoaded()
    {
        // Old saves without MODE retain the established WoWBridge behavior.
        g_waitingForControls = g_state.mode == Mode::WoW;
    }
    bool Start()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        if (!Config::Get().wowUi || !player || !player->Is3DLoaded() || Character::Get().classId == 0 || !ui || ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || g_box || g_next != Step::None || g_state.selectedSex) return false;
        g_creator = ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME);
        const auto* controls = RE::ControlMap::GetSingleton();
        if (g_state.mode == Mode::Intro && !g_creator && (!controls || !controls->IsMovementControlsEnabled() || !controls->IsFightingControlsEnabled())) {
            RE::SendHUDMessage::ShowHUDMessage("WoWBridge setup will appear at character creation.");
            return true;
        }
        if (!g_state.Offer(g_creator, true)) return false;
        g_class = Character::Get().classId; g_race = Character::Looks().race;
        g_next = g_creator && g_state.mode != Mode::WoW ? Step::Offer : CanChooseClass() ? Step::Class : Step::Race;
        logger::info("integration: manual setup requested (creator={})", g_creator);
        return true;
    }
    void Tick()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        if (!player || !player->Is3DLoaded() || !ui || ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return;
        const bool creator = ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME);
        // Also catch loading a save made during the opening, with its creator already open.
        if (!Config::Get().autorun && Config::Get().wowUi && g_state.Offer(creator)) {
            g_creator = true; g_next = Step::Offer;
        }
        if (g_next != Step::None && !g_box && !ui->IsMenuOpen(RE::Console::MENU_NAME) && !ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME)) {
            const auto step = std::exchange(g_next, Step::None);
            if (step == Step::Name) EnterName();
            else if (!(g_box = Box(step))) {
                logger::warn("integration: setup popup unavailable; use wb start to retry");
                RE::SendHUDMessage::ShowHUDMessage("WoWBridge setup could not open. Try wb start.");
            }
        }
        const auto* controls = RE::ControlMap::GetSingleton();
        if (g_waitingForControls && controls && !ui->GameIsPaused() && g_state.ShouldActivate(true, creator,
                controls->IsMovementControlsEnabled(), controls->IsFightingControlsEnabled())) {
            g_waitingForControls = false;
            const auto sex = g_state.selectedSex;
            g_state.selectedSex.reset();
            SetActive(true);
            if (sex) Character::SetSex(*sex);
            else Character::OnGameLoaded();
            Gear::OnGameLoaded();   // their worn gear read again (the Druid's starter kit is given only to a Druid)
            logger::info("integration: active as {}", PlayerName());
        }
        // WoW's character customization once the interface is up, after WoWBridge was chosen; again in a fresh
        // interface (ReloadUI, another profile) while it is unanswered.
        if (Active() && WowUi::Ready() && Character::CustomizationOffered() && g_customizeShown != WowUi::Generation()) {
            g_customizeShown = WowUi::Generation();
            WowUi::Run("__WBCustomizeAppearance()");
            logger::info("integration: character customization offered");
        }
        if (Active()) {
            const auto name = PlayerName();
            if (name != g_name) {
                g_name = name;
                WowUi::Run("__WBApplyLive({player={name=" + LuaQuote(name) + "}})");
            }
        }
    }
    void Save(SKSE::SerializationInterface* intfc)
    {
        const auto mode = static_cast<std::uint32_t>(g_state.mode);
        if (!intfc->OpenRecord(kRecord, kRecordVersion) || !intfc->WriteRecordData(mode)) logger::error("integration: mode was not saved");
    }
    void Load(SKSE::SerializationInterface* intfc, std::uint32_t length, std::uint32_t version)
    {
        std::uint32_t mode = 0;
        if (version != kRecordVersion || length != sizeof(mode) || intfc->ReadRecordData(mode) != sizeof(mode) || !State::Valid(mode)) {
            logger::warn("integration: invalid saved mode; using Skyrim until wb start");
            g_state.Load(Mode::Skyrim); UpdateClassChoice(); return;
        }
        g_state.Load(static_cast<Mode>(mode));
        UpdateClassChoice();
    }
}
