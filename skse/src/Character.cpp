#include "Character.h"
#include "Integration.h"
#include "Progression.h"
#include "Gear.h"

#include "Avatar.h"
#include "Surrogate.h"

#include <fstream>
#include <mutex>

namespace Character
{
	namespace
	{
		Sheet                           g_sheet;
		std::map<std::uint32_t, Item>   g_gear;
		std::map<std::string, EquipmentSet> g_sets;
		std::mutex                      g_lock;   // the worn set: the avatar's loader reads it on its own thread
		std::optional<std::vector<Item>> g_worn;
		wowcore::Appearance              g_female;
		std::optional<wowcore::Appearance> g_male;
		std::optional<std::uint32_t>       g_loadedSex;
	}

	// WoW InventoryType -> the equipment slot it goes in (EquipmentSlots, ItemPrototype.h).
	std::uint32_t SlotOf(std::uint32_t a_type)
	{
		static const std::map<std::uint32_t, std::uint32_t> kSlots = { { 1, 0 }, { 2, 1 }, { 3, 2 }, { 4, 3 }, { 5, 4 }, { 20, 4 },
			{ 6, 5 }, { 7, 6 }, { 8, 7 }, { 9, 8 }, { 10, 9 }, { 11, 10 }, { 12, 12 }, { 16, 14 }, { 13, 15 }, { 17, 15 }, { 21, 15 },
			{ 14, 16 }, { 22, 16 }, { 23, 16 }, { 15, 17 }, { 25, 17 }, { 26, 17 }, { 28, 17 }, { 19, 18 } };
		const auto it = kSlots.find(a_type);
		return it == kSlots.end() ? 19 : it->second;
	}

	namespace
	{
		std::map<std::uint32_t, std::uint32_t> g_reagents;

		std::vector<Item> WornItems()
		{
			std::lock_guard lock(g_lock);
			return g_worn ? *g_worn : g_sheet.equipment;
		}

		void LoadGear()
		{
			const std::filesystem::path path = "Data/SKSE/Plugins/WoWBridge/gear.json";
			std::ifstream               in(path);
			if (!in) {
				logger::warn("character: {} not found; her gear is not Skyrim items", path.string());
				return;
			}
			try {
				const auto gear = nlohmann::json::parse(in);
				for (const auto& [key, e] : gear.value("reagents", nlohmann::json::object()).items()) {
					g_reagents[e.at("entry")] = e.at("form");
				}
				for (const auto& [key, e] : gear.at("items").items()) {
					Item i{ SlotOf(e.at("InventoryType")), e.at("entry"), e.at("displayid"), e.at("InventoryType"), e.at("class"),
						e.at("subclass"), e.at("sheath"), e.at("Material"), e.at("delay"), e.at("form"), e.value("ability", 0u) };
					if (e.contains("use")) {
						i.useSpell = e["use"].at("spell"), i.useEntry = e["use"].at("entry"), i.useCooldown = e["use"].at("cooldown_ms");
					}
					g_gear[i.entry] = i;
				}
				for (const auto& [key, e] : gear.value("sets", nlohmann::json::object()).items()) {
					EquipmentSet set{ e.at("name").get<std::string>(), e.at("entries").get<std::vector<std::uint32_t>>() };
					if (set.entries.empty() || !std::ranges::all_of(set.entries, [](auto entry) { return g_gear.contains(entry); })) {
						throw std::runtime_error("equipment set has missing gear entries: " + key);
					}
					for (const auto& b : e.value("bonuses", nlohmann::json::array())) {
						set.bonuses.push_back({b.at("spell"), b.at("threshold"), b.value("ability", 0u)});
					}
					g_sets.emplace(key, std::move(set));
				}
			} catch (const std::exception& e) {
				logger::error("character: {}: {}", path.string(), e.what());
			}
			for (auto& i : g_sheet.equipment) {   // the realm's items know their Skyrim forms too
				if (const auto it = g_gear.find(i.entry); it != g_gear.end()) {
					i.form = it->second.form, i.ability = it->second.ability;
				}
			}
		}
	}

	bool Load()
	{
		const std::filesystem::path path = "Data/SKSE/Plugins/WoWBridge/character.json";
		std::ifstream               in(path);
		if (!in) {
			logger::error("character: {} not found", path.string());
			return false;
		}
		try {
			const auto j = nlohmann::json::parse(in);
			Sheet      s;
			s.name = j.at("name").get<std::string>();
			s.level = j.at("level");
			s.classId = j.at("class");
			s.looks = { j.at("race"), j.at("sex"), j.at("skin"), j.at("face"), j.at("hairStyle"), j.at("hairColor"),
				j.at("facialHair") };
			for (const auto& e : j.at("equipment")) {
				s.equipment.push_back({ e.at("slot"), e.at("entry"), e.at("displayid"), e.at("InventoryType"), e.at("class"),
					e.at("subclass"), e.at("sheath"), e.at("Material"), e.at("delay") });
			}
			for (const auto& a : j.value("actions", nlohmann::json::array())) {
				s.actions.emplace_back(a.at("slot"), a.at("spell"));
			}
			g_sheet = std::move(s);
			g_female = g_sheet.looks;
			g_female.sex = 1;
		} catch (const std::exception& e) {
			logger::error("character: {}: {}", path.string(), e.what());
			return false;
		}
		LoadGear();
		try {
			std::ifstream male("Data/SKSE/Plugins/WoWBridge/character-male.json");
			const auto j = nlohmann::json::parse(male);
			wowcore::Appearance looks{ j.at("race"), j.at("sex"), j.at("skin"), j.at("face"), j.at("hairStyle"),
				j.at("hairColor"), j.at("facialHair") };
			if (looks.race != g_female.race || looks.sex != 0) {
				throw std::runtime_error("male appearance must match the character's race and have sex 0");
			}
			g_male = looks;
		} catch (const std::exception& e) {
			logger::error("character: male appearance unavailable: {}", e.what());
		}
		logger::info("character: {} (level {}, class {}) wearing {} items; {} WoW items are Skyrim items", g_sheet.name, g_sheet.level,
			g_sheet.classId, g_sheet.equipment.size(), g_gear.size());
		return true;
	}

	const Sheet& Get() { return g_sheet; }

	wowcore::Appearance Looks()
	{
		std::lock_guard lock(g_lock);
		return g_sheet.looks;
	}

	bool SetSex(std::uint32_t a_sex)
	{
		if (a_sex > 1 || (a_sex == 0 && !g_male)) { return false; }
		bool changed;
		{
			std::lock_guard lock(g_lock);
			changed = g_sheet.looks.sex != a_sex;
			g_sheet.looks = a_sex ? g_female : *g_male;
		}
		if (Integration::Active()) Surrogate::SetSex(a_sex == 1);
		if (changed) { Avatar::Redress(); }
		return true;
	}

	void ResetSex() { g_loadedSex.reset(); SetSex(1); }
	void PrepareLoad() { g_loadedSex.reset(); }
	void OnGameLoaded()
	{
		SetSex(g_loadedSex.value_or(1));
		g_loadedSex.reset();
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		const auto sex = Looks().sex;
		if (!a_intfc->OpenRecord(kRecord, kRecordVersion) || !a_intfc->WriteRecordData(sex)) {
			logger::error("character: sex was not saved");
		}
	}

	void LoadSex(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length)
	{
		std::uint32_t sex = 1;
		if (a_length != sizeof(sex) || a_intfc->ReadRecordData(sex) != sizeof(sex) || sex > 1 || (sex == 0 && !g_male)) {
			logger::error("character: invalid or unavailable saved sex; using female");
			g_loadedSex.reset();
			return;
		}
		g_loadedSex = sex;
	}

	const std::map<std::uint32_t, std::uint32_t>& Reagents() { return g_reagents; }

	const std::map<std::uint32_t, Item>& Gear() { return g_gear; }
	const std::map<std::string, EquipmentSet>& Sets() { return g_sets; }

	void SetWorn(std::vector<Item> a_items)
	{
		std::lock_guard lock(g_lock);
		g_worn = std::move(a_items);
	}

	std::optional<Item> InSlot(std::uint32_t a_slot)
	{
		for (const auto& i : WornItems()) {
			if (i.slot == a_slot) {
				return i;
			}
		}
		return std::nullopt;
	}

	std::vector<wowcore::Worn> Worn()
	{
		std::vector<wowcore::Worn> worn;
		for (const auto& i : WornItems()) {
			worn.push_back({ i.slot, i.displayId, i.sheath });
		}
		return worn;
	}

	// The skills the service answers in (service/skyrim.py SKILLS): the name an answer gives, the caster's key for its
	// level, the actor value.
	struct Skill
	{
		std::string_view name, key;
		RE::ActorValue   value;
	};
	constexpr Skill kSkills[] = { { "Destruction", "destruction", RE::ActorValue::kDestruction },
		{ "Restoration", "restoration", RE::ActorValue::kRestoration }, { "Alteration", "alteration", RE::ActorValue::kAlteration },
		{ "Marksman", "archery", RE::ActorValue::kArchery },
		{ "OneHanded", "one_handed", RE::ActorValue::kOneHanded }, { "TwoHanded", "two_handed", RE::ActorValue::kTwoHanded } };

	nlohmann::json Caster()
	{
		auto items = nlohmann::json::array();
		for (const auto& i : WornItems()) {
			if (i.entry) {   // Skyrim mappings describe a look, not an item whose stats the service should grant
				items.push_back(i.entry);
			}
		}
		auto skyrim = nlohmann::json::object();   // with these the service answers in Skyrim's health and magicka
		auto level = g_sheet.level;
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			level = player->GetLevel();
			for (const auto& skill : kSkills) {
				skyrim[std::string(skill.key)] = player->AsActorValueOwner()->GetActorValue(skill.value);
			}
		}
		nlohmann::json caster = { { "level", level }, { "items", items }, { "skyrim", skyrim }, { "equipment_modifiers", Gear::Modifiers() } };
		Progression::Augment(caster);
		return caster;
	}

	void Train(std::string_view a_skill, float a_use)
	{
		const auto it = std::ranges::find(kSkills, a_skill, &Skill::name);
		if (it != std::end(kSkills) && a_use > 0) {
			RE::PlayerCharacter::GetSingleton()->AddSkillExperience(it->value, a_use);
		}
	}

	std::string ActionsLua()
	{
		std::string lua = "__WB.actions = {";
		for (const auto& [slot, spell] : g_sheet.actions) {
			lua += std::format(" [{}] = {{ spell = {} }},", slot, spell);
		}
		return lua + " }";
	}

	std::atomic<bool> g_shifted = false;
	void SetShapeshifted(bool a_shifted) { g_shifted = a_shifted; }

	wowcore::MeleeAnims Melee()
	{
		if (g_shifted) {   // a bear or a cat fights with what it has
			return wowcore::MeleeAnims{};
		}
		const auto hand = InSlot(15);
		return hand ? wowcore::meleeAnims(hand->itemClass, hand->subclass, hand->inventoryType) : wowcore::MeleeAnims{};
	}
}
