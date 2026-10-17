#include "Loot.h"

#include "Bags.h"
#include "Config.h"
#include "Frame.h"
#include "Input.h"
#include "ui/WowUi.h"

#include <nlohmann/json.hpp>

namespace Loot
{
	namespace
	{
		using json = nlohmann::json;
		using Mode = RE::ContainerMenu::ContainerMode;
		RE::UI::Create_t* g_create = nullptr;
		std::atomic<RE::IMenu*> g_backend = nullptr;
		std::atomic<std::uint32_t> g_session = 0;
		bool g_closing = false; // main thread only
		RE::ObjectRefHandle g_source;
		std::uint32_t g_active = 0;
		std::map<std::string, int> g_slots;
		std::string g_last;
		double g_next = 0;

		RE::NiPointer<RE::TESObjectREFR> Target()
		{
			RE::NiPointer<RE::TESObjectREFR> source;
			RE::LookupReferenceByHandle(RE::ContainerMenu::GetTargetRefHandle(), source);
			return source;
		}

		bool Eligible(RE::TESObjectREFR* ref)
		{
			if (!ref || ref->IsDeleted() || ref->IsDisabled()) return false;
			if (auto* actor = ref->As<RE::Actor>()) return actor->IsDead() && !actor->IsPlayerRef();
			return ref->GetBaseObject() && ref->GetBaseObject()->GetFormType() == RE::FormType::Container;
		}

        bool CanDeposit(RE::TESObjectREFR* source)
        {
            return source && source->GetBaseObject() && source->GetBaseObject()->GetFormType()==RE::FormType::Container;
        }
		bool Reachable(RE::TESObjectREFR* source)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!Eligible(source) || !player || player->IsDead() || !source->GetParentCell() || !player->GetParentCell()) return false;
			if (source->GetParentCell()->IsInteriorCell() || player->GetParentCell()->IsInteriorCell()) {
				if (source->GetParentCell() != player->GetParentCell()) return false;
			} else if (source->GetWorldspace() != player->GetWorldspace()) return false;
			float reach = 320.f;
			if (auto* actor = source->As<RE::Actor>()) {
				if (!actor->Is3DLoaded()) return false;
				// Large corpses can be activated beside the player's body, well away from
				// the origin. Keep the same five-yard allowance outside that radius.
				const float radius = actor->GetBoundRadius();
				if (std::isfinite(radius) && radius > 0) reach += radius;
			}
			return source->GetPosition().GetDistance(player->GetPosition()) <= reach;
		}

		struct Part
		{
			RE::TESBoundObject* form;
			RE::ExtraDataList* extra; // borrowed; resolved afresh for every native transfer
			json item;
		};

		std::vector<Part> Parts(RE::TESObjectREFR* source)
		{
			std::vector<Part> out;
			for (const auto& [form, data] : source->GetInventory()) {
				if (!form || !form->GetPlayable() || data.first <= 0) continue;
				int remaining = data.first;
				auto add = [&](RE::ExtraDataList* extra, int count) {
					if (count <= 0 || !Bags::Listed(form, extra)) return;
					RE::InventoryEntryData entry(form, count);
					if (extra) entry.AddExtraList(extra);
					auto* owner = entry.GetOwner();
					if (!owner) owner = source->GetOwner();
					auto item = Bags::DescribeItem(form, extra, count);
					item["steal"] = !entry.IsOwnedBy(RE::PlayerCharacter::GetSingleton(), owner, true);
					// Ownership is part of a loot identity: never merge a free and a stolen instance.
					item["key"] = item["key"].get<std::string>() + ":" + std::to_string(owner ? owner->GetFormID() : 0);
					item["worn"] = false;
					out.push_back({form, extra, std::move(item)});
				};
				if (data.second && data.second->extraLists) for (auto* extra : *data.second->extraLists) {
					if (!extra || remaining <= 0) continue;
					const auto count = std::min(remaining, std::max(0, extra->GetCount()));
					add(extra, count); remaining -= count;
				}
				add(nullptr, remaining);
			}
			return out;
		}

		json Items(RE::TESObjectREFR* source)
		{
			std::map<std::string, json> grouped;
			for (auto& part : Parts(source)) {
				const auto key = part.item["key"].get<std::string>();
				auto [it, added] = grouped.try_emplace(key, part.item);
				if (!added) it->second["count"] = it->second["count"].get<int>() + part.item["count"].get<int>();
			}
			json rows = json::array();
			for (auto& [key, item] : grouped) {
				if (!g_slots.contains(key)) g_slots[key] = static_cast<int>(g_slots.size()) + 1;
				item["slot"] = g_slots.at(key);
				rows.push_back(std::move(item));
			}
			return rows;
		}

		void Publish(bool opening = false, bool shift = false)
		{
			const auto source = g_source.get();
			if (!source || !g_active) return;
			const auto lua = "__WBLoot(" + std::to_string(g_active) + "," + Bags::ItemLua(Items(source.get())) +
				"," + (opening ? "true" : "false") + "," + (shift ? "true" : "false") + "," + (CanDeposit(source.get()) ? "true" : "false") + ")";
			if (opening || lua != g_last) { g_last = lua; WowUi::Run(lua); }
		}

		// Wrap creation before UI counts pause/cursor flags. Keep Skyrim's native menu object and close path,
		// but let the client's LootFrame own rendering and input. Pickpocket/NPC modes are untouched.
		RE::IMenu* Create()
		{
			auto* menu = g_create();
			const auto mode = RE::ContainerMenu::GetContainerMode();
			const auto source = Target();
			if (!menu || !Config::Get().wowUi || !WowUi::Ready() || (mode != Mode::kLoot && mode != Mode::kSteal) ||
				!Eligible(source.get())) return menu;
			menu->menuFlags = RE::UI_MENU_FLAGS::kNone;
			menu->inputContext = RE::IMenu::Context::kNone;
			if (menu->uiMovie) menu->uiMovie->SetVisible(false);
			g_backend = menu;
			++g_session;
			return menu;
		}

		struct MessageHook
		{
			static RE::UI_MESSAGE_RESULTS thunk(RE::ContainerMenu* menu, RE::UIMessage& message)
			{
				if (g_backend != menu) return func(menu, message);
				using T = RE::UI_MESSAGE_TYPE;
				if (message.type == T::kUserEvent || message.type == T::kScaleformEvent) return RE::UI_MESSAGE_RESULTS::kHandled;
				const auto result = func(menu, message);
				if (menu->uiMovie) menu->uiMovie->SetVisible(false);
				if (message.type == T::kShow) {
					const auto ref = Target();
					const auto source = ref ? ref->CreateRefHandle() : RE::ObjectRefHandle{};
					const auto session = g_session.load();
					const bool shift = Input::ShiftHeld();
					// Its own menu, not a stale session number: a previous container's queued hide bumps g_session
					// between this capture and the task (a right-click a second after a loot window closed published
					// nothing in three suites, October 16), and this menu is still the backend.
					SKSE::GetTaskInterface()->AddTask([source, session, shift, menu] {
						if (g_backend != menu) return;
						g_source = source; g_active = std::max(session, g_session.load()); g_closing = false; g_slots.clear(); g_last.clear();
						Publish(true, shift);
					});
				} else if (message.type == T::kHide || message.type == T::kForceHide) {
					const auto session = g_session.load();
					g_backend = nullptr; ++g_session;
					SKSE::GetTaskInterface()->AddTask([session] {
						if (g_active == session) { g_active = 0; g_source = {}; g_last.clear(); }
						WowUi::Run("__WBLootClosed(" + std::to_string(session) + ")");
					});
				}
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct DisplayHook
		{
			static void thunk(RE::IMenu* menu) { if (g_backend != menu) func(menu); }
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Init()
	{
		// Native transfer ABI was audited against this executable, not inferred from callback names.
		if (REL::Module::get().version() != REL::Version{1, 7, 104, 0}) {
			logger::warn("loot: unaudited runtime; native containers retained"); return;
		}
		REL::Relocation<std::uintptr_t> table{RE::VTABLE_ContainerMenu[0]};
		const auto transfer = REL::Relocation<std::uintptr_t>{REL::ID(51141)}.address();
		if (*reinterpret_cast<const std::uintptr_t*>(table.address() + 4 * 8) != REL::Relocation<std::uintptr_t>{REL::ID(51126)}.address() ||
			std::memcmp(reinterpret_cast<const void*>(transfer), "\x48\x8B\xC4\x44\x88\x48\x20\x44\x89\x40\x18", 11) != 0) {
			logger::error("loot: native ABI mismatch; native containers retained"); return;
		}
		auto* ui = RE::UI::GetSingleton();
		RE::BSSpinLockGuard lock(ui->processMessagesLock);
		const auto it = ui->menuMap.find(RE::BSFixedString(RE::ContainerMenu::MENU_NAME));
		if (it == ui->menuMap.end() || !it->second.create) { logger::error("loot: no ContainerMenu factory"); return; }
		g_create = it->second.create; it->second.create = Create;
		MessageHook::func = table.write_vfunc(4, MessageHook::thunk);
		DisplayHook::func = table.write_vfunc(6, DisplayHook::thunk);
		logger::info("loot: client LootFrame with native container lifecycle/transfer installed");
	}

	void Reset()
	{
		const auto session = g_session.fetch_add(1);
		g_active = 0; g_source = {}; g_closing = false; g_slots.clear(); g_last.clear(); g_next = 0;
		WowUi::Run("__WBLootClosed(" + std::to_string(session) + ")");
	}

	void Close(std::uint32_t session)
	{
		SKSE::GetTaskInterface()->AddTask([session] {
			if (session != g_session || !g_backend || g_closing) return;
			g_closing = true;
			// AE 51126 dispatches native container cleanup only for kHide (3). kForceHide (4)
			// falls through to IMenu and would skip its item-list/Inventory3D close lifecycle.
			RE::UIMessageQueue::GetSingleton()->AddMessage(RE::ContainerMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
		});
	}

	void Tick()
	{
		if (!g_active || !WowUi::Ready()) return;
		const auto source = g_source.get();
		if (!Reachable(source.get())) { Close(g_active); return; }
		if (Frame::Now() >= g_next) { g_next = Frame::Now() + 0.25; Publish(); }
	}

    void Deposit(std::uint32_t session, std::string key, int count)
    {
        SKSE::GetTaskInterface()->AddTask([session,key=std::move(key),count] {
            if (!g_active || session!=g_active || session!=g_session || count<=0 || g_closing) return;
            const auto source=g_source.get();
            auto menu=RE::UI::GetSingleton()->GetMenu<RE::ContainerMenu>();
            if (!Reachable(source.get()) || !menu || g_backend!=menu.get()) { Close(session); return; }
            const auto selected=Bags::Sale(key);
            int moved=0;
            if (CanDeposit(source.get()) && !selected.is_null() && count<=selected["count"].get<int>()) {
                const auto identity=selected["identity"].get<std::string>();
                auto* player=RE::PlayerCharacter::GetSingleton();
                using Transfer=bool (*)(RE::ContainerMenu*,RE::InventoryEntryData*,std::int32_t,bool);
                static REL::Relocation<Transfer> transfer{REL::ID(51141)};
                // Resolve borrowed extra lists again after every native mutation.
                while (moved<count) {
                    const auto parts=Parts(player);
                    const auto part=std::ranges::find_if(parts,[&](const Part& p) {
                        const auto i=Bags::DescribeItem(p.form,p.extra,p.item["count"].get<int>());
                        return !i.value("worn",false) && i["key"]==identity;
                    });
                    if (part==parts.end()) break;
                    const auto inventory=player->GetInventory();
                    const auto whole=inventory.find(part->form);
                    if (whole==inventory.end() || !whole->second.second || whole->second.second->IsQuestObject()) break;
                    const int n=std::min(count-moved,part->item["count"].get<int>());
                    RE::InventoryEntryData entry(part->form,n);
                    if (part->extra) entry.AddExtraList(part->extra);
                    const int before=player->GetItemCount(part->form);
                    menu->GetRuntimeData().pcControlsReady=true;
                    // AE 51141's false branch is Skyrim's Store path, retaining its restrictions/extra data.
                    transfer(menu.get(),&entry,n,false);
                    const int delta=before-player->GetItemCount(part->form);
                    if (delta<=0) break;
                    moved+=delta;
                }
            }
            if (moved>0) Bags::Sold(key,moved);
            else WowUi::Run("__WBFire('UI_ERROR_MESSAGE','This item cannot be stored here.')");
            g_last.clear(); Publish(); // acknowledge refusal too, preserving the bag cursor
        });
    }

	void Take(std::uint32_t session, std::string key, int count)
	{
		SKSE::GetTaskInterface()->AddTask([session, key = std::move(key), count] {
			if (!g_active || session != g_active || session != g_session || count <= 0 || g_closing) return;
			const auto source = g_source.get();
			auto menu = RE::UI::GetSingleton()->GetMenu<RE::ContainerMenu>();
			if (!Reachable(source.get()) || !menu || g_backend != menu.get()) { Close(session); return; }
			const auto rows = Items(source.get());
			const auto row = std::ranges::find_if(rows, [&](const json& i) { return i["key"] == key; });
			if (row == rows.end() || (*row)["count"].get<int>() != count) { g_last.clear(); Publish(); return; }
			// This is the engine function called by ItemTransfer and TakeAllItems. It resolves ownership,
			// uses kSteal where appropriate, accumulates the crime value and preserves instance extra data.
			using Transfer = bool (*)(RE::ContainerMenu*, RE::InventoryEntryData*, std::int32_t, bool);
			static REL::Relocation<Transfer> transfer{REL::ID(51141)};
			int remaining = count;
			while (remaining > 0) {
				auto parts = Parts(source.get());
				const auto part = std::ranges::find_if(parts, [&](const Part& p) { return p.item["key"] == key; });
				if (part == parts.end()) break;
				const int n = std::min(remaining, part->item["count"].get<int>());
				RE::InventoryEntryData entry(part->form, n);
				if (part->extra) entry.AddExtraList(part->extra);
				// Avoid a native selected-item tooltip update; the client owns selection and tooltips.
				menu->GetRuntimeData().pcControlsReady = true;
				if (!transfer(menu.get(), &entry, n, true)) break;
				const auto after = Items(source.get());
				const auto now = std::ranges::find_if(after, [&](const json& i) { return i["key"] == key; });
				const int left = now == after.end() ? 0 : (*now)["count"].get<int>();
				if (left >= remaining) break; // a scripted/refused transfer cannot loop or clear its UI row
				remaining = left;
			}
			g_last.clear(); Publish();
			if (Items(source.get()).empty()) Close(session);
		});
	}
}
