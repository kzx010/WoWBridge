#include "Distribution.h"

#include "Armory.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace Distribution
{
	namespace
	{
		std::map<std::string, RE::TESLevItem*, std::less<>> g_lists;
		std::map<std::uint32_t, RE::TESGlobal*> g_classBooks;   // WoW class -> its book lists' chance-none global

		// What any NPC carries or wears, as loaded with every plugin (Populated Cities' people too): their inventories and
		// outfits, and every leveled list within those. A list among them is never joined (their gear has no Skyrim body to
		// show on theirs); a death item's only reaches a corpse.
		std::unordered_set<const RE::TESForm*> Carried()
		{
			std::unordered_set<const RE::TESForm*> seen;
			std::vector<const RE::TESForm*>        open;
			const auto add = [&](const RE::TESForm* a_form) {
				if (a_form && seen.insert(a_form).second) {
					open.push_back(a_form);
				}
			};
			for (auto* npc : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESNPC>()) {
				if (!npc) {
					continue;
				}
				npc->ForEachContainerObject([&](RE::ContainerObject& a_object) {
					add(a_object.obj);
					return RE::BSContainer::ForEachResult::kContinue;
				});
				add(npc->defaultOutfit);
				add(npc->sleepOutfit);
			}
			while (!open.empty()) {
				const auto* form = open.back();
				open.pop_back();
				if (const auto* outfit = form->As<RE::BGSOutfit>()) {
					for (auto* item : outfit->outfitItems) add(item);
				} else if (const auto* list = form->As<RE::TESLevItem>()) {
					for (const auto& entry : list->entries) add(entry.form);
				}
			}
			return seen;
		}

		// Entries at level 1 among the list's level-1 entries (the game reads a list in level order), each count rolling: a
		// list choosing one of its entries chooses this one that many times as often.
		bool Join(RE::TESLevItem* a_into, RE::TESLevItem* a_list, std::uint16_t a_count, std::size_t a_entries)
		{
			auto& entries = a_into->entries;
			if (std::ranges::any_of(entries, [&](const RE::LEVELED_OBJECT& e) { return e.form == a_list; })) {
				return false;
			}
			if (entries.size() + a_entries > 255) {   // LLCT is a byte
				logger::warn("distribution: {:08X} is full", a_into->GetFormID());
				return false;
			}
			const auto at = static_cast<std::size_t>(std::ranges::find_if(entries, [](const RE::LEVELED_OBJECT& e) { return e.level > 1; }) - entries.begin());
			const auto size = entries.size();
			entries.resize(size + a_entries);
			std::move_backward(entries.begin() + at, entries.begin() + size, entries.end());
			for (std::size_t i = 0; i < a_entries; ++i) entries[at + i] = RE::LEVELED_OBJECT{ a_list, a_count, 1, 0, nullptr };
			a_into->numEntries = static_cast<std::uint8_t>(size + a_entries);
			return true;
		}

		// A plugin's rows (gear.json's distribution, or armory.json's): its lists into Skyrim's and its vendors' chests.
		int Distribute(const nlohmann::json& a_rows, std::string_view a_plugin, const std::unordered_set<const RE::TESForm*>& a_carried)
		{
			int joined = 0;
			auto* handler = RE::TESDataHandler::GetSingleton();
			for (const auto& row : a_rows) {
				auto* list = handler->LookupForm<RE::TESLevItem>(row.at("list").get<RE::FormID>(), a_plugin);
				const auto name = row.at("name").get<std::string>();
				if (!list) {
					logger::warn("distribution: {} is not in {}", name, a_plugin);
					continue;
				}
				g_lists[name] = list;
				for (const auto& target : row.value("lists", nlohmann::json::array())) {
					// The list as loaded after every plugin (its winning record), appended to, never replaced.
					auto* into = RE::TESForm::LookupByID<RE::TESLevItem>(target.at("form").get<RE::FormID>());
					if (!into) {
						logger::warn("distribution: no leveled list {:08X} for {}", target.at("form").get<RE::FormID>(), name);
					} else if (a_carried.contains(into)) {
						logger::warn("distribution: {:08X} is in an NPC's inventory or outfit with these plugins; {} stays out", into->GetFormID(), name);
					} else if (Join(into, list, target.at("count").get<std::uint16_t>(), target.value("entries", 1u))) {
						++joined;
					}
				}
				for (const auto& target : row.value("vendors", nlohmann::json::array())) {
					// The vendor faction's merchant chest as loaded (another plugin may move it): its base container, which each
					// restock rolls again. A faction without one is skipped.
					auto* faction = RE::TESForm::LookupByID<RE::TESFaction>(target.at("form").get<RE::FormID>());
					auto* chest = faction ? faction->vendorData.merchantContainer : nullptr;
					auto* base = chest && chest->GetBaseObject() ? chest->GetBaseObject()->As<RE::TESObjectCONT>() : nullptr;
					if (!base) {
						logger::warn("distribution: vendor {:08X} has no merchant chest for {}", target.at("form").get<RE::FormID>(), name);
					} else if (!base->GetObjectCount(list) && base->AddObjectToContainer(list, target.at("count").get<std::int32_t>(), nullptr)) {
						++joined;
					}
				}
			}
			return joined;
		}
	}

	void Init()
	{
		int joined = 0;
		const auto carried = Carried();
		try {
			std::ifstream in("Data/SKSE/Plugins/WoWBridge/gear.json");
			const auto data = nlohmann::json::parse(in);
			joined += Distribute(data.value("distribution", nlohmann::json::array()), "WoWBridge.esp", carried);
			for (const auto& [cls, form] : data.value("class_book_globals", nlohmann::json::object()).items()) {
				auto* global = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESGlobal>(form.get<RE::FormID>() & 0xFFFFFF, "WoWBridge.esp");
				if (global) g_classBooks[static_cast<std::uint32_t>(std::stoul(cls))] = global;
				else logger::warn("distribution: class {} book global is not in WoWBridge.esp", cls);
			}
		} catch (const std::exception& e) {
			logger::error("distribution: gear.json: {}", e.what());
		}
		try {   // WoWBridgeArmory.esp's gear by level and quality (read with its items: Armory::Init, before this)
			joined += Distribute(Armory::Distribution(), Armory::kPlugin, carried);
		} catch (const std::exception& e) {
			logger::error("distribution: armory.json: {}", e.what());
		}
		logger::info("distribution: {} lists joined {} vanilla lists and vendor chests", g_lists.size(), joined);
	}

	void SetClass(std::uint32_t a_class)
	{
		for (auto& [cls, global] : g_classBooks) global->value = cls == a_class ? 0.0f : 100.0f;
		logger::info("distribution: class {} books roll ({} classes gated)", a_class, g_classBooks.size());
	}

	RE::TESLevItem* List(std::string_view a_name)
	{
		const auto it = g_lists.find(a_name);
		return it == g_lists.end() ? nullptr : it->second;
	}

	std::vector<RE::TESForm*> Roll(RE::TESLevItem* a_list, std::uint16_t a_level)
	{
		std::vector<RE::TESForm*> out;
		RE::BSScrapArray<RE::CALCED_OBJECT> calced;
		if (a_list) a_list->CalculateCurrentFormList(a_level, 1, calced, 0, false);
		for (auto& c : calced) {
			if (auto* inner = c.form ? c.form->As<RE::TESLevItem>() : nullptr) {
				const auto more = Roll(inner, a_level);
				out.insert(out.end(), more.begin(), more.end());
			} else if (c.form) {
				out.push_back(c.form);
			}
		}
		return out;
	}
}
