#include "Learning.h"

#include "Character.h"
#include "LuaText.h"
#include "Mounts.h"
#include "Progression.h"
#include "ui/WowUi.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace Learning
{
	namespace
	{
		struct Item
		{
			std::uint32_t       entry = 0;
			RE::TESBoundObject* form = nullptr;
			std::string         name;
			std::uint32_t       spell = 0, previous = 0;   // a book's rank and the rank it needs first
			std::string         mount;                     // a mount item's mount
			std::uint16_t       level = 0;                 // its "Requires Level", Skyrim's
			std::int32_t        classes = -1;              // AllowableClass
		};
		std::vector<Item>                 g_items;
		std::atomic<RE::TESObjectBOOK*>   g_reading = nullptr;

		const Item* Find(const RE::TESBoundObject* a_form)
		{
			const auto it = std::ranges::find(g_items, a_form, &Item::form);
			return it == g_items.end() ? nullptr : &*it;
		}

		bool Refuse(std::string_view a_key, int a_value = 0)   // the client's string (GlobalStrings.lua), as the server sends it
		{
			WowUi::Run(std::format("__WBItemError({}{})", LuaQuote(a_key), a_value ? std::format(",{}", a_value) : ""));
			return true;
		}

		// Skyrim's Book Menu, every time a book opens: one of theirs read from their inventory (no world reference: a book
		// in the world is taken first) is used as from their bags when the menu closes, back in their inventory.
		struct Reading : RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event || a_event->menuName != RE::BookMenu::MENU_NAME) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event->opening) {
					auto* book = RE::BookMenu::GetTargetForm();
					g_reading = book && !RE::BookMenu::GetTargetReference() && Find(book) ? book : nullptr;
				} else if (auto* book = g_reading.exchange(nullptr)) {
					SKSE::GetTaskInterface()->AddTask([book] { Use(book); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		} g_sink;
	}

	void Init()
	{
		try {
			std::ifstream in("Data/SKSE/Plugins/WoWBridge/gear.json");
			const auto data = nlohmann::json::parse(in);
			auto* handler = RE::TESDataHandler::GetSingleton();
			for (const auto* section : { "books", "mounts" }) {
				for (const auto& [key, row] : data.value(section, nlohmann::json::object()).items()) {
					auto* found = handler->LookupForm(row.at("form").get<RE::FormID>(), "WoWBridge.esp");
					const auto& teaches = row.at("teaches");
					Item item{ row.at("entry").get<std::uint32_t>(), found ? found->As<RE::TESBoundObject>() : nullptr,
						row.at("name").get<std::string>(), teaches.value("spell", 0u), teaches.value("previous", 0u),
						teaches.value("mount", std::string{}), static_cast<std::uint16_t>(row.at("wow").value("RequiredLevel", 0)),
						row.at("wow").value("AllowableClass", -1) };
					if (!item.form) {
						logger::warn("learning: {} is not in WoWBridge.esp", item.name);
						continue;
					}
					g_items.push_back(std::move(item));
				}
			}
		} catch (const std::exception& e) {
			logger::error("learning: gear.json books and mount items: {}", e.what());
		}
		RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&g_sink);
		logger::info("learning: {} books and mount items", g_items.size());
	}

	std::uint32_t EntryOf(const RE::TESBoundObject* a_form)
	{
		const auto* item = Find(a_form);
		return item ? item->entry : 0;
	}

	bool Use(RE::TESBoundObject* a_form, RE::ExtraDataList* a_extra)
	{
		const auto* item = Find(a_form);
		if (!item) {
			return false;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || player->GetItemCount(a_form) <= 0) {
			return true;
		}
		// CMaNGOS Player::CanUseItem's order (class, required spell, level), then the learning spell's own refusal.
		const auto classId = Character::Get().classId;
		if (!classId || (item->classes & 1 << (classId - 1)) == 0) {
			return Refuse("ERR_CANT_EQUIP_EVER");
		}
		if (item->previous && !Progression::Taught(item->previous)) {
			return Refuse("ERR_PROFICIENCY_NEEDED");
		}
		if (player->GetLevel() < item->level) {
			return Refuse("ERR_CANT_EQUIP_LEVEL_I", item->level);
		}
		if (!(item->mount.empty() ? Progression::LearnBook(item->spell) : Mounts::Learn(item->mount))) {
			return Refuse("SPELL_FAILED_SPELL_LEARNED");
		}
		player->RemoveItem(a_form, 1, RE::ITEM_REMOVE_REASON::kRemove, a_extra, nullptr);
		logger::info("learning: {} used ({})", item->name, item->mount.empty() ? std::to_string(item->spell) : item->mount);
		return true;
	}

	RE::TESBoundObject* Book(std::uint32_t a_spell)
	{
		const auto it = std::ranges::find_if(g_items, [&](const Item& i) { return i.mount.empty() && i.spell == a_spell; });
		return it == g_items.end() ? nullptr : it->form;
	}

	RE::TESBoundObject* MountItem(std::string_view a_key)
	{
		const auto it = std::ranges::find_if(g_items, [&](const Item& i) { return !i.mount.empty() && i.mount == a_key; });
		return it == g_items.end() ? nullptr : it->form;
	}

	std::vector<std::uint32_t> Books()
	{
		std::vector<std::uint32_t> out;
		for (const auto& i : g_items) {
			if (i.mount.empty()) {
				out.push_back(i.spell);
			}
		}
		return out;
	}

	std::vector<std::string> MountKeys()
	{
		std::vector<std::string> out;
		for (const auto& i : g_items) {
			if (!i.mount.empty()) {
				out.push_back(i.mount);
			}
		}
		return out;
	}
}
