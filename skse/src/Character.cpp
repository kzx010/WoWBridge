#include "Character.h"
#include "Integration.h"
#include "Progression.h"
#include "Gear.h"
#include "Forms.h"
#include "Carriers.h"
#include "Cast.h"
#include "LuaText.h"

#include "Avatar.h"
#include "Config.h"
#include "Surrogate.h"
#include "ui/WowUi.h"

#include "wowcore/mpq.hpp"

#include <fstream>
#include <mutex>

namespace Bridge { nlohmann::json NativeActorValues(RE::Actor* actor); }

namespace Character
{
	namespace
	{
		Sheet                           g_sheet;
		Sheet                           g_catalog;
		std::vector<wowcore::CharacterClass> g_classes;
		std::map<std::uint32_t, Item>   g_gear;
		std::map<std::string, EquipmentSet> g_sets;
		std::mutex                      g_lock;   // the worn set: the avatar's loader reads it on its own thread
		std::optional<std::vector<Item>> g_worn;
		wowcore::Appearance              g_female;
		std::optional<wowcore::Appearance> g_male;
		std::map<std::pair<std::uint32_t, std::uint32_t>, wowcore::Appearance> g_defaults;
		using Record = wowcore::CharacterIdentity;
		std::optional<Record> g_loaded;   // the loading save's, until Skyrim has restored its player (or a newer choice)
		bool                  g_offer = false;

		const wowcore::Appearance& Packaged(std::uint32_t a_sex, std::uint32_t a_race = 4)
		{
			const auto it = g_defaults.find({a_race, a_sex});
			return it != g_defaults.end() ? it->second : g_female;
		}

		// A look their race and sex can have: a packaged one, or one within 1.12 character creation's choices (the client's
		// tables, read once).
		bool Allowed(const wowcore::Appearance& a_looks)
		{
			if (a_looks.sex > 1 || !g_defaults.contains({a_looks.race, a_looks.sex})) return false;
			if (a_looks == Packaged(a_looks.sex, a_looks.race)) return true;
			static const auto choices = [] {
				std::map<std::pair<std::uint32_t, std::uint32_t>, wowcore::Customization> c;
				try {
					const wowcore::Mpq mpq(Config::ClientData());
					for (const auto race : {1u,3u,4u,6u}) for (const auto sex : {0u, 1u}) c[{race,sex}] = wowcore::customization(mpq, race, sex);
				} catch (const std::exception& e) {
					logger::error("character: no customization choices: {}", e.what());
				}
				return c;
			}();
			return choices.contains({a_looks.race,a_looks.sex}) && choices.at({a_looks.race,a_looks.sex}).allows(a_looks);
		}

		// Their look, now: a different one redresses the player; the interface hears it either way.
		void Apply(const wowcore::Appearance& a_looks, bool a_restoring = false)
		{
			// Keep normal equipment reconciliation; P1 must not schedule the catalog's Druid kit.
			if (g_sheet.classId != 11 && !Gear::Given()) Gear::Reset();
			const bool raceChanged = Looks().race != a_looks.race;
			if (raceChanged && !a_restoring) { Forms::BreakShadowmeld(); Forms::Leave(); }
			bool changed;
			{
				std::lock_guard lock(g_lock);
				changed = a_restoring || g_sheet.looks != a_looks;
				g_sheet.looks = a_looks;
			}
			g_loaded.reset();   // a choice made since the save loaded is theirs now
			if (Integration::Active()) {
				Surrogate::SetSex(a_looks.sex == 1);
				Surrogate::SetSize(a_looks.race, a_looks.sex);
			}
			if (changed) { Avatar::Redress(); Carriers::Racials(); }
			WowUi::Run(AppearanceLua());
		}

		void RestoreSheet(std::uint32_t a_class, const wowcore::Appearance* a_looks = nullptr)
		{
			std::lock_guard lock(g_lock);
			g_sheet.classId = a_class;
			if (a_looks) g_sheet.looks = *a_looks;
			g_sheet.actions = a_class == g_catalog.classId ? g_catalog.actions : decltype(g_sheet.actions){};
			g_sheet.equipment = a_class == g_catalog.classId ? g_catalog.equipment : decltype(g_sheet.equipment){};
		}
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
				logger::warn("character: {} not found; their gear is not Skyrim items", path.string());
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
						// Only a transformation (Benediction <-> Anathema) names another entry; other uses have none.
						const auto& use = e.at("use");
						const auto& own = use.contains("spells") && !use.at("spells").empty() ? use.at("spells").at(0) : use;
						i.useSpell = use.value("spell", 0u), i.useEntry = use.value("entry", 0u), i.useCooldown = use.value("cooldown_ms", 0u);
						i.useItemCooldown = own.value("cooldown_ms", i.useCooldown);
						i.useCategory = use.value("category", 0u), i.useCategoryCooldown = use.value("category_cooldown_ms", 0u);
						i.useReadyCategory = use.value("ready_category", i.useCategory);   // an export before ready_category
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
			if (j.value("version",1u) > 2) throw std::runtime_error("unsupported character version");
			s.classId = j.value("class",11u);
			s.looks = { j.at("race"), j.at("sex"), j.at("skin"), j.at("face"), j.at("hairStyle"), j.at("hairColor"),
				j.at("facialHair") };
			if (!wowcore::eligibleCharacter(s.classId,s.looks.race) || s.looks.sex > 1)
				throw std::runtime_error("invalid character class/race/sex");
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
			g_defaults[{g_female.race,1}] = g_female;
		} catch (const std::exception& e) {
			logger::error("character: {}: {}", path.string(), e.what());
			return false;
		}
		LoadGear();
		g_catalog = g_sheet;
		try {
			std::ifstream male("Data/SKSE/Plugins/WoWBridge/character-male.json");
			const auto j = nlohmann::json::parse(male);
			wowcore::Appearance looks{ j.at("race"), j.at("sex"), j.at("skin"), j.at("face"), j.at("hairStyle"),
				j.at("hairColor"), j.at("facialHair") };
			if (looks.race != g_female.race || looks.sex != 0) {
				throw std::runtime_error("male appearance must match the character's race and have sex 0");
			}
			g_male = looks;
			g_defaults[{looks.race,0}] = looks;
		} catch (const std::exception& e) {
			logger::error("character: male appearance unavailable: {}", e.what());
		}
		for (const auto sex : {0u, 1u}) {
			try {
				std::ifstream in(std::format("Data/SKSE/Plugins/WoWBridge/character-tauren-{}.json", sex ? "female" : "male"));
				const auto j = nlohmann::json::parse(in);
				wowcore::Appearance looks{j.at("race"),j.at("sex"),j.at("skin"),j.at("face"),j.at("hairStyle"),j.at("hairColor"),j.at("facialHair")};
				const wowcore::Mpq mpq(Config::ClientData());
				if (looks.race != 6 || looks.sex != sex || !wowcore::customization(mpq,6,sex).allows(looks))
					throw std::runtime_error("invalid Tauren preset");
				g_defaults[{6,sex}] = looks;
			} catch (const std::exception& e) { logger::error("character: Tauren preset unavailable: {}", e.what()); }
		}
		try {
			const wowcore::Mpq mpq(Config::ClientData());
			g_classes = wowcore::characterClasses(mpq);
			for (const auto race : {1u,3u}) for (const auto sex : {0u,1u}) {
				// P1's packaged looks: the first creation options in the client tables.
				const wowcore::Appearance looks{race,sex,0,0,0,0,0};
				if (!wowcore::customization(mpq,race,sex).allows(looks)) throw std::runtime_error("invalid Paladin preset");
				g_defaults.try_emplace(std::pair{race,sex},looks);
			}
		} catch (const std::exception& e) { logger::error("character: class presets unavailable: {}", e.what()); }
		logger::info("character: {} (level {}, class {}) wearing {} items; {} WoW items are Skyrim items", g_sheet.name, g_sheet.level,
			g_sheet.classId, g_sheet.equipment.size(), g_gear.size());
		return true;
	}

	const Sheet& Get() { return g_sheet; }
	const std::vector<wowcore::CharacterClass>& Classes() { return g_classes; }

	wowcore::Appearance Looks()
	{
		std::lock_guard lock(g_lock);
		return g_sheet.looks;
	}

	bool SetSex(std::uint32_t a_sex)
	{
		const auto looks = Looks();
		if (a_sex > 1 || !g_defaults.contains({looks.race,a_sex})) { return false; }
		Apply(looks.sex == a_sex ? looks : Packaged(a_sex, looks.race));
		return true;
	}

	bool SetLooks(const wowcore::Appearance& a_looks)
	{
		if (!wowcore::eligibleCharacter(g_sheet.classId,a_looks.race) || !Allowed(a_looks) ||
			(a_looks.race != 4 && !Surrogate::ClientSize(a_looks.race, a_looks.sex))) { return false; }
		g_offer = false;
		Integration::UpdateClassChoice();
		Apply(a_looks);
		return true;
	}
	bool SetIdentity(std::uint32_t a_class, const wowcore::Appearance& a_looks, bool a_testing)
	{
		logger::info("character: SetIdentity class {} race {} sex {} testing {}", a_class, a_looks.race, a_looks.sex, a_testing);
		if (g_sheet.classId != a_class && !a_testing && !Integration::CanChooseClass()) return false;
		const auto c = std::ranges::find(g_classes,a_class,&wowcore::CharacterClass::id);
		if (c == g_classes.end() || !std::ranges::contains(c->races,a_looks.race) ||
			!wowcore::eligibleCharacter(a_class,a_looks.race) || !Allowed(a_looks) ||
			(a_looks.race != 4 && !Surrogate::ClientSize(a_looks.race,a_looks.sex))) return false;
		if (g_sheet.classId != a_class) {
			Cast::ResetForTesting();
			Progression::RestoreForTesting({{"known_spells",nlohmann::json::array()},{"books",nlohmann::json::array()},{"talents",nlohmann::json::object()}});
			RestoreSheet(a_class);
		}
		g_offer = false;
		Integration::UpdateClassChoice();
		Apply(a_looks);
		return true;
	}
	bool SetIdentity(std::uint32_t a_class, std::uint32_t a_race, std::uint32_t a_sex, bool a_testing)
	{
		if (!g_defaults.contains({a_race,a_sex})) return false;
		const auto own = Looks();
		return SetIdentity(a_class,own.race == a_race && own.sex == a_sex ? own : Packaged(a_sex,a_race),a_testing);
	}
	void KeepLooks() { OfferCustomization(false); }
	void OfferCustomization(bool a_offer)
	{
		g_offer = a_offer;
		Integration::UpdateClassChoice();
		WowUi::Run(std::format("if __WB.appearance then __WB.appearance.chooseClass={} end",
			Integration::CanChooseClass() ? "true" : "false"));
	}
	bool CustomizationOffered() { return g_offer; }

	std::string AppearanceLua()
	{
		wowcore::Appearance looks;
		std::uint32_t classId;
		{
			std::lock_guard lock(g_lock);
			looks = g_sheet.looks;
			classId = g_sheet.classId;
		}
		const auto lua = [](const wowcore::Appearance& a) {
			return std::format("{{race={},sex={},skin={},face={},hairStyle={},hairColor={},facialHair={}}}", a.race, a.sex, a.skin,
				a.face, a.hairStyle, a.hairColor, a.facialHair);
		};
		std::string defaults = "{";
		for (const auto race : {1u,3u,4u,6u}) {
			defaults += std::format("[{}]={{",race);
			for (const auto sex : {0u,1u}) if (g_defaults.contains({race,sex})) defaults += std::format("[{}]={},",sex,lua(Packaged(sex,race)));
			defaults += "},";
		}
		defaults += "}";
		std::string classes = "{";
		for (const auto& c : g_classes) {
			classes += std::format("[{}]={{name={},token={},races={{",c.id,LuaQuote(c.name),LuaQuote(c.token));
			for (const auto race : c.races) classes += std::format("[{}]=true,",race);
			classes += "}},";
		}
		return std::format("__WBAppearance({},{},{},{},{})", lua(looks), defaults, classId, classes+"}",
			Integration::CanChooseClass() ? "true" : "false");
	}

	void ResetSex()
	{
		Surrogate::PrepareLoad(); g_loaded.reset(); g_offer = false;
		Integration::UpdateClassChoice();
		RestoreSheet(g_catalog.classId);
		Apply(g_female);
	}
	void PrepareLoad() {
		Surrogate::PrepareLoad();
		RestoreSheet(g_catalog.classId);
		g_loaded = Record{ g_female, false, g_catalog.classId }; g_offer = false;
		Integration::UpdateClassChoice();
	}
	void RestoreIdentity()
	{
		// Forms::Reset needs the saved class and race before it restores displays and racial stealth.
		// Keep the record pending: the surrogate and redress still wait for gameplay controls.
		if (g_loaded) {
			RestoreSheet(g_loaded->classId, &g_loaded->looks);
			g_offer = g_loaded->offer;
			Integration::UpdateClassChoice();
		}
	}
	void OnGameLoaded()
	{
		const auto loaded = g_loaded;
		if (loaded) {
			// The record was validated at read time. Restoring it must keep Cast's restored state,
			// including when class metadata or surrogate sizing is temporarily unavailable.
			RestoreSheet(loaded->classId);
			if (loaded->classId != 11) {
				Progression::RestoreForTesting({{"known_spells",nlohmann::json::array()},{"books",nlohmann::json::array()},{"talents",nlohmann::json::object()}});
			}
			g_offer = loaded->offer;
			Integration::UpdateClassChoice();
			Apply(loaded->looks, true);
		} else Apply(Looks());
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		Surrogate::SaveSize(a_intfc);
		const auto record = g_loaded.value_or(Record{ Looks(), g_offer, g_sheet.classId });
		if (!wowcore::writeIdentityRecords(record, [&](auto version, const auto& data, auto size) {
			return a_intfc->OpenRecord(kRecord, version) && a_intfc->WriteRecordData(data.data(), size);
		})) {
			logger::error("character: their look was not saved");
		}
	}

	void LoadLooks(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length, std::uint32_t a_version)
	{
		wowcore::IdentityData data{};
		const auto size = wowcore::identitySize(a_version);
		if (!size || a_length != size || a_intfc->ReadRecordData(data.data(), size) != size) {
			logger::error("character: unreadable saved look; using female");
			return;
		}
		const auto record = wowcore::readIdentity(a_version, data, Packaged(0), g_female);
		if (!record || !Allowed(record->looks)) {
			logger::error("character: invalid or unavailable saved look; using female");
			return;
		}
		g_loaded = record;
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

	std::atomic<bool> g_showHelm = true, g_showCloak = true;

	void SetShown(bool a_helm, bool a_cloak)
	{
		const bool changed = (g_showHelm.exchange(a_helm) != a_helm) | (g_showCloak.exchange(a_cloak) != a_cloak);
		if (changed) {
			Avatar::Redress();
		}
	}

	std::vector<wowcore::Worn> Worn()
	{
		std::vector<wowcore::Worn> worn;
		for (const auto& i : WornItems()) {
			if ((i.slot == 0 && !g_showHelm) || (i.slot == 14 && !g_showCloak)) {   // head, back
				continue;
			}
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
		// Their live native defences and regeneration (Standing Stones, Fortify/Resist effects) for the service's conversions.
		caster["actor_values"] = Bridge::NativeActorValues(RE::PlayerCharacter::GetSingleton());
		Progression::Augment(caster);
		if (g_sheet.classId != 11 && g_sheet.classId != 2) caster["talents"] = nlohmann::json::object();
		caster["class"] = g_sheet.classId;   // every op builds the player by class and race (known spells and talents too)
		caster["race"] = Looks().race;
		return caster;
	}

	void Train(std::string_view a_skill, float a_use)
	{
		const auto it = std::ranges::find(kSkills, a_skill, &Skill::name);
		if (it != std::end(kSkills) && a_use > 0) {
			RE::PlayerCharacter::GetSingleton()->AddSkillExperience(it->value, a_use);
		}
	}

	bool HasActions()
	{
		std::lock_guard lock(g_lock);
		return g_sheet.classId != 11 || !g_sheet.actions.empty();
	}

	std::string ActionsLua()
	{
		std::vector<std::pair<std::uint32_t, std::uint32_t>> actions;
		{
			std::lock_guard lock(g_lock);
			if (g_sheet.classId != 11) return "__WB.actions = {}";
			actions = g_sheet.actions;
		}
		std::string lua = "__WB.actions = {";
		for (const auto& [slot, spell] : actions) {
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
