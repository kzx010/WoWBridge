#pragma once
#include <nlohmann/json_fwd.hpp>

// Skyrim owns counts, item instances and use. The co-save owns pile counts, logical bag places and the visible page.
namespace Bags
{
	constexpr std::uint32_t kRecord = 'BAGS', kRecordVersion = 2;
	// Shared description for bags, merchant and loot; extra lists are borrowed only for this call. Main thread.
	nlohmann::json DescribeItem(RE::TESBoundObject* a_form, RE::ExtraDataList* a_extra, int a_count);
	// What Skyrim's own menus list: playable and named (its own or a custom name). Nameless records (draugr hair,
	// dragon skins and blood, clutter tokens) stay hidden there; GetDisplayName() calls them "<Missing Name>".
	bool Named(RE::TESBoundObject* a_form, RE::ExtraDataList* a_extra);
	bool Listed(RE::TESBoundObject* a_form, RE::ExtraDataList* a_extra);
	std::string ItemLua(const nlohmann::json& a_items);
	// Selected-pile accounting for native transfers. Main thread.
	nlohmann::json Sale(const std::string& a_key);
	void Sold(const std::string& a_key, int a_count);
	void Init();
	void Reset(); // before loading records / new game
	void Tick();
	void Cursor(std::string a_action, std::string a_key, int a_count, std::uint32_t a_to, std::string a_expected, std::string a_splitKey, std::uint32_t a_revision);
	void Use(std::string a_key, bool a_effect = false);
	void Page(std::uint32_t a_page);
	void Save(SKSE::SerializationInterface* a_intfc);
	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length, std::uint32_t a_version);
}
