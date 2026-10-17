#include "Armory.h"

#include <charconv>
#include <fstream>
#include <random>

namespace Armory
{
	namespace
	{
		std::vector<Item>                                       g_items;
		std::unordered_map<std::uint32_t, std::size_t>           g_byEntry;
		std::unordered_map<const RE::TESBoundObject*, std::size_t> g_byForm;
		std::vector<std::vector<RE::SpellItem*>>                g_abilities;
		std::map<std::uint32_t, Set>                            g_sets;
		std::unordered_map<std::uint32_t, std::string>          g_rows;    // entry -> {"wow":..,"skyrim":..} as text
		std::unordered_map<std::uint32_t, nlohmann::json>       g_shown;   // the rows their stacks have shown
		nlohmann::json                                          g_distribution = nlohmann::json::array();
		std::mt19937                                            g_rng{ std::random_device{}() };

		std::string Lower(std::string_view a_text)
		{
			std::string out(a_text);
			for (auto& c : out) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
			return out;
		}
	}

	void Init()
	{
		auto* data = RE::TESDataHandler::GetSingleton();
		if (!data->LookupModByName(kPlugin)) {
			logger::info("armory: {} not loaded; WoWBridge.esp's items only", kPlugin);
			return;
		}
		const auto started = std::chrono::steady_clock::now();
		const std::filesystem::path path = "Data/SKSE/Plugins/WoWBridge/armory.json";
		try {
			std::ifstream in(path);
			if (!in) throw std::runtime_error("not found");
			const auto armory = nlohmann::json::parse(in);
			for (const auto& copies : armory.at("abilities")) {
				auto& spells = g_abilities.emplace_back();
				for (const auto& local : copies) {
					spells.push_back(data->LookupForm<RE::SpellItem>(local.get<RE::FormID>(), kPlugin));
				}
				std::erase(spells, nullptr);
			}
			std::size_t missing = 0;
			for (const auto& [key, row] : armory.at("items").items()) {
				// Untyped, then cast: LookupForm<T> wants T's exact form type, and a bound object has none of its own.
				auto* found = data->LookupForm(row.at("form").get<RE::FormID>(), kPlugin);
				auto* form = found ? found->As<RE::TESBoundObject>() : nullptr;
				if (!form) {
					++missing;
					continue;
				}
				const auto& wow = row.at("wow");
				const auto field = [&](const char* a_name) { return wow.value(a_name, 0u); };
				Item item;
				item.entry = std::stoul(key);
				item.form = form;
				item.look = { Character::SlotOf(field("InventoryType")), item.entry, field("displayid"), field("InventoryType"),
					field("class"), field("subclass"), field("sheath"), field("Material"), field("delay") };
				item.ability = row.value("ability", -1);
				item.set = row.value("set", 0u);
				item.level = row.at("skyrim").at("level").get<std::uint16_t>();
				item.quality = static_cast<std::uint8_t>(field("Quality"));
				item.name = wow.at("name").get<std::string>();
				g_rows[item.entry] = nlohmann::json{ { "wow", wow }, { "skyrim", row.at("skyrim") } }.dump();
				g_items.push_back(std::move(item));
			}
			std::ranges::sort(g_items, {}, &Item::entry);
			for (std::size_t i = 0; i < g_items.size(); ++i) {
				g_byEntry[g_items[i].entry] = i;
				g_byForm[g_items[i].form] = i;
			}
			for (const auto& [key, row] : armory.at("sets").items()) {
				Set set{ row.at("name").get<std::string>() };
				for (const auto& member : row.at("items")) {
					set.entries.push_back(member.at("entry"));
					set.names.push_back(member.at("name"));
				}
				for (const auto& b : row.at("bonuses")) set.bonuses.push_back({ b.at("spell"), b.at("threshold"), b.value("ability", 0u) });
				set.level = row.at("level"), set.quality = row.at("quality");
				g_sets.emplace(std::stoul(key), std::move(set));
			}
			g_distribution = armory.value("distribution", nlohmann::json::array());
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
			logger::info("armory: {} items, {} bonuses, {} sets from {} in {} ms; {} forms missing", g_items.size(), g_abilities.size(),
				g_sets.size(), kPlugin, ms, missing);
		} catch (const std::exception& e) {
			logger::error("armory: {}: {}", path.string(), e.what());
			g_items.clear(), g_byEntry.clear(), g_byForm.clear(), g_rows.clear();
		}
	}

	bool Ready() { return !g_items.empty(); }

	const Item* Find(std::uint32_t a_entry)
	{
		const auto it = g_byEntry.find(a_entry);
		return it == g_byEntry.end() ? nullptr : &g_items[it->second];
	}

	const Item* Find(const RE::TESBoundObject* a_form)
	{
		const auto it = a_form ? g_byForm.find(a_form) : g_byForm.end();
		return it == g_byForm.end() ? nullptr : &g_items[it->second];
	}

	const std::vector<Item>& Items() { return g_items; }

	const std::vector<std::vector<RE::SpellItem*>>& Abilities() { return g_abilities; }

	const std::map<std::uint32_t, Set>& Sets() { return g_sets; }

	nlohmann::json Row(std::uint32_t a_entry)
	{
		if (const auto shown = g_shown.find(a_entry); shown != g_shown.end()) return shown->second;
		const auto text = g_rows.find(a_entry);
		const auto* item = Find(a_entry);
		if (text == g_rows.end() || !item) return nullptr;
		auto row = nlohmann::json::parse(text->second);
		row["displayid"] = item->look.displayId;
		row["quality"] = item->quality;
		row["InventoryType"] = item->look.inventoryType;
		if (const auto set = g_sets.find(item->set); set != g_sets.end()) {   // as gear.json's: its members' names and bonuses
			auto members = nlohmann::json::array(), bonuses = nlohmann::json::array();
			for (std::size_t i = 0; i < set->second.entries.size(); ++i) {
				members.push_back({ { "entry", set->second.entries[i] }, { "name", set->second.names[i] } });
			}
			for (const auto& b : set->second.bonuses) bonuses.push_back({ { "spell", b.spell }, { "threshold", b.threshold } });
			row["set"] = { { "id", set->first }, { "name", set->second.name }, { "items", members }, { "bonuses", bonuses } };
		}
		return g_shown[a_entry] = std::move(row);
	}

	const nlohmann::json& Distribution() { return g_distribution; }

	const Item* Random(std::uint32_t a_level, std::uint32_t a_quality)
	{
		std::uint32_t below = 0, above = UINT32_MAX;
		for (const auto& i : g_items) {
			if (i.quality != a_quality) continue;
			if (i.level <= a_level) below = std::max<std::uint32_t>(below, i.level);
			else above = std::min<std::uint32_t>(above, i.level);
		}
		const auto level = below ? below : above;
		std::vector<const Item*> pick;
		for (const auto& i : g_items) if (i.quality == a_quality && i.level == level) pick.push_back(&i);
		if (pick.empty()) return nullptr;
		return pick[std::uniform_int_distribution<std::size_t>(0, pick.size() - 1)(g_rng)];
	}

	std::vector<const Item*> Search(std::string_view a_text, std::size_t a_limit)
	{
		std::vector<const Item*> out;
		const auto wanted = Lower(a_text);
		std::uint32_t entry = 0;
		const auto [end, error] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), entry);
		const bool number = error == std::errc{} && end == a_text.data() + a_text.size();
		for (const auto& i : g_items) {
			if (out.size() >= a_limit) break;
			if (number ? i.entry == entry : Lower(i.name).find(wanted) != std::string::npos) out.push_back(&i);
		}
		return out;
	}
}
