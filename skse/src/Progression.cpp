#include "Progression.h"

#include <fstream>

#include "Character.h"
#include "Frame.h"
#include "LuaText.h"
#include "Service.h"
#include "SpellVisuals.h"
#include "ui/WowUi.h"

namespace Progression
{
	bool Knows(std::uint32_t a_spell);

	namespace
	{
		using json = nlohmann::json;

		// The service's skill keys (service/skyrim.py SKILLS) and Skyrim's skills.
		constexpr std::pair<const char*, RE::ActorValue> kSkills[] = { { "destruction", RE::ActorValue::kDestruction },
			{ "restoration", RE::ActorValue::kRestoration }, { "alteration", RE::ActorValue::kAlteration },
			{ "one_handed", RE::ActorValue::kOneHanded }, { "two_handed", RE::ActorValue::kTwoHanded }, { "archery", RE::ActorValue::kArchery },
			{ "block", RE::ActorValue::kBlock }, { "sneak", RE::ActorValue::kSneak }, { "alchemy", RE::ActorValue::kAlchemy },
			{ "lockpicking", RE::ActorValue::kLockpicking }, { "conjuration", RE::ActorValue::kConjuration }, { "illusion", RE::ActorValue::kIllusion } };

		constexpr std::pair<const char*, RE::FormID> kPerks[] = {
#include "Perks.inc"
		};

		json              g_asked;          // the skills last asked about
		std::vector<int>  g_known;          // what she knows, as last answered
		bool              g_quiet = true;   // the next answer is published without announcing or placing
		bool              g_ready = false;
		bool              g_pending = false;
		std::uint32_t     g_epoch = 0;      // replies from before a Reset are dropped
		std::optional<std::vector<int>> g_loaded;   // what the save kept, until the load's Reset brings it back
		double            g_next = 0;
		json              g_talents = json::object();
		std::optional<json> g_loadedTalents;
		json              g_talentAsked;
		bool              g_talentPending = false;
		// The latest request of each kind: its reply, from before a Reset or not, ends the wait (a reply dropped for its
		// epoch would otherwise leave her book or talents waiting for good).
		std::uint64_t     g_bookRequest = 0, g_talentRequest = 0;
		double            g_nextTalents = 0;

		// An answer as a Lua table (__WBClassSpells takes the response body as it is).
		std::string Lua(const json& a_value)
		{
			std::string out;
			switch (a_value.type()) {
			case json::value_t::object:
				out = "{";
				for (const auto& [key, value] : a_value.items()) {
					out += "[" + LuaQuote(key) + "]=" + Lua(value) + ",";
				}
				return out + "}";
			case json::value_t::array:
				out = "{";
				for (const auto& value : a_value) {
					out += Lua(value) + ",";
				}
				return out + "}";
			case json::value_t::string:
				return LuaQuote(a_value.get<std::string>());
			case json::value_t::boolean:
				return a_value.get<bool>() ? "true" : "false";
			case json::value_t::number_float:
				return std::format("{}", a_value.get<double>());
			case json::value_t::number_integer:
			case json::value_t::number_unsigned:
				return a_value.dump();
			default:
				return "nil";
			}
		}

		json TalentRequest()
		{
			json req = { { "op", "talents" }, { "class", Character::Get().classId }, { "caster", Character::Caster() } };
			if (g_ready) {
				req["known_spells"] = g_known;
			}
			return req;
		}

		bool CanPublish()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* ui = RE::UI::GetSingleton();
			return player && player->Is3DLoaded() && WowUi::Ready() && ui && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		}

		void TickTalents()
		{
			if (g_talentPending || Frame::Now() < g_nextTalents) {
				return;
			}
			g_nextTalents = Frame::Now() + 1.0;
			auto req = TalentRequest();
			if (req == g_talentAsked) {
				return;
			}
			g_talentAsked = req;
			g_talentPending = true;
			Service::Request(req, [epoch = g_epoch, id = ++g_talentRequest, level = req["caster"]["level"]](const json& r) {
				if (id != g_talentRequest) return;
				g_talentPending = false;
				if (epoch != g_epoch || !CanPublish()) {
					g_talentAsked = json();   // asked again with what she has now
					return;
				}
				if (!r.value("ok", false) || Character::Caster()["level"] != level) {
					g_talentAsked = json();
					if (!r.value("ok", false)) {
						logger::warn("talents: {}", r.value("error", std::string("no answer")));
					}
					return;
				}
				WowUi::Run("__WBTalents(" + Lua(r) + ")");
			});
		}

		// __WBClassSpells reconciles all saved bar/cursor ranks, including quiet publication.
		// A newly learned spell (not a passive) goes
		// to the first free slot of her main bar, then of the bottom-left bar. Where she put them stays.
		constexpr const char* kPlace = R"(
local reply, announce = ...
local a = __WB.actions or {}
__WB.actions = a
local meta, onBar = {}, {}
for _, s in ipairs(reply.spells or {}) do meta[s.spell] = s end
for _, s in ipairs(reply.unlearned_spells or {}) do meta[s.spell] = meta[s.spell] or s end
for _, x in pairs(a) do local s = meta[x.spell] if s then onBar[s.chain] = true end end
if announce then
  for _, id in ipairs(reply.newly_learned or {}) do
    local s = meta[id]
    if s and not s.passive and not onBar[s.chain] then
      local best = reply.highest_ranks[tostring(s.chain)] or id
      -- A form's ability goes to that form's page (after Attack): the cat's 74-84, the bear's 98-108.
      local slots = __WBClassActionSlots(__WB.spellInfo(best))
      for _, slot in ipairs(slots) do
        if not a[slot] then a[slot] = { spell = best } onBar[s.chain] = true break end
      end
    end
  end
end
__WBFire("ACTIONBAR_SLOT_CHANGED", 0)
)";

		void Publish(const json& a_reply)
		{
			const bool quiet = std::exchange(g_quiet, false);
			std::string lua = "local reply = " + Lua(a_reply) + " __WBClassSpells(reply)";
			if (!quiet) {
				for (const auto& id : a_reply.value("newly_learned", json::array())) {
					lua += std::format(" __WBLearnedSpell({})", id.get<int>());
				}
			}
			lua += std::format("; (function(...) {} end)(reply, {})", kPlace, quiet ? "false" : "true");
			WowUi::Run(std::move(lua));
			g_known = a_reply.value("known_spells", std::vector<int>{});
			if (Character::Get().classId == 11) {   // a druid's shapeshift bar: the forms she knows, learned in its order
				std::size_t forms = 0;
				for (const int spell : { Knows(9634) ? 9634 : 5487, 1066, 768, 783 }) {
					if (!Knows(static_cast<std::uint32_t>(spell))) {
						break;
					}
					++forms;
				}
				WowUi::Run(std::format("__WBDruidForms({}, {}, {})", Knows(9634) ? 9634 : 5487, forms, Knows(24858) ? "true" : "false"));
			}
			// Her spells' effects: the class's every rank (they share their models), loaded once in the background.
			std::vector<std::uint32_t> spells(g_known.begin(), g_known.end());
			for (const auto& s : a_reply.value("unlearned_spells", json::array())) {
				spells.push_back(s.value("spell", 0u));
			}
			SpellVisuals::Want(spells);
			g_ready = true;
			logger::info("progression: {} spells known{}", g_known.size(), quiet ? " (published quietly)" : "");
		}
	}

	void Tick()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		const double now = Frame::Now();
		if (!CanPublish() || !Character::Get().classId) {
			return;
		}
		TickTalents();
		if (g_pending || now < g_next) {
			return;
		}
		g_next = now + 1.0;
		json skills = json::object();
		for (const auto& [key, value] : kSkills) {
			skills[key] = static_cast<int>(player->AsActorValueOwner()->GetBaseActorValue(value));
		}
		json caster = Character::Caster();
		caster["skyrim"] = skills;
		if (caster == g_asked) {
			return;
		}
		g_asked = caster;
		g_pending = true;
		// What she learned stays learned (a skill lowered for a while does not unlearn it): her known spells go as acquired.
		json req = { { "op", "known_spells" }, { "class", Character::Get().classId }, { "caster", caster },
			{ "since", g_known }, { "acquired_spells", g_known } };
		Service::Request(std::move(req), [epoch = g_epoch, id = ++g_bookRequest](const json& r) {
			if (id != g_bookRequest) return;
			g_pending = false;
			if (epoch != g_epoch || !CanPublish()) {
				g_asked = json();
				return;
			}
			if (!r.value("ok", false)) {
				logger::warn("progression: {}", r.value("error", std::string("no answer")));
				g_asked = json();   // ask again
				return;
			}
			Publish(r);
		});
	}

	void Reset()
	{
		++g_epoch;
		g_asked = json();
		g_known = g_loaded ? std::move(*g_loaded) : std::vector<int>{};
		g_loaded.reset();
		g_quiet = true;
		g_ready = false;
		g_pending = false;
		g_next = 0;
		g_talents = g_loadedTalents ? std::move(*g_loadedTalents) : json::object();
		g_loadedTalents.reset();
		g_talentAsked = json();
		g_talentPending = false;
		g_nextTalents = 0;
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		const auto classId = Character::Get().classId;
		const auto count = static_cast<std::uint32_t>(g_known.size());
		if (!a_intfc->OpenRecord(kRecord, kRecordVersion) || !a_intfc->WriteRecordData(classId) ||
			!a_intfc->WriteRecordData(count) || (count && !a_intfc->WriteRecordData(g_known.data(), count * sizeof(int)))) {
			logger::error("progression: her known spells were not saved");
		}
		const auto text = json({ { "class", classId }, { "talents", g_talents } }).dump();
		if (!a_intfc->OpenRecord(kTalentRecord, kTalentRecordVersion) ||
			!a_intfc->WriteRecordData(text.data(), static_cast<std::uint32_t>(text.size()))) {
			logger::error("talents: her allocation was not saved");
		}
	}

	void LoadTalents(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length)
	{
		g_loadedTalents.reset();
		if (!a_length || a_length > 65536) {
			logger::error("talents: a damaged saved record");
			return;
		}
		std::string text(a_length, '\0');
		if (a_intfc->ReadRecordData(text.data(), a_length) != a_length) {
			return;
		}
		const auto saved = json::parse(text, nullptr, false);
		if (!saved.is_object() || !saved.contains("class") || !saved["class"].is_number_unsigned() ||
			saved["class"].get<std::uint32_t>() != Character::Get().classId || !saved.contains("talents") || !saved["talents"].is_object()) {
			logger::error("talents: an unreadable or other class's saved record");
			return;
		}
		for (const auto& [id, rank] : saved["talents"].items()) {
			if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos || !rank.is_number_integer() ||
				rank <= 0 || rank > 5) {
				logger::error("talents: a damaged saved allocation");
				return;
			}
		}
		g_loadedTalents = saved["talents"];   // the service validates class IDs, budget and prerequisite reachability
	}

	void Augment(json& a_caster)
	{
		a_caster["class"] = Character::Get().classId;
		a_caster["talents"] = g_talents;
		a_caster["perks"] = json::object();
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			for (const auto& [key, form] : kPerks) {
				if (auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(form); perk && player->HasPerk(perk)) {
					a_caster["perks"][key] = true;
				}
			}
			a_caster["stealthed"] = player->IsSneaking();
			a_caster["wearing_armor"] = false;
			for (const auto& [form, item] : player->GetInventory([](RE::TESBoundObject& obj) { return obj.IsArmor(); })) {
				if (item.second && item.second->IsWorn()) {
					const auto* armor = form->As<RE::TESObjectARMO>();
					if (armor && (armor->IsLightArmor() || armor->IsHeavyArmor())) {
						a_caster["wearing_armor"] = true;
						break;
					}
				}
			}
			a_caster["level"] = player->GetLevel();
			const auto* cell = player->GetParentCell();
			a_caster["indoors"] = cell && cell->IsInteriorCell();
		}
	}

	void LearnTalent(int a_talent, int a_rank)
	{
		if (g_talentPending || !g_ready) {
			logger::info("talents: learning {} waits (pending {}, ready {})", a_talent, g_talentPending, g_ready);
			WowUi::Run("__WBTalents({ok=false,error='Please wait for the talent update'})");
			return;
		}
		auto req = TalentRequest();
		req["learn"] = { { "talent", a_talent }, { "rank", a_rank } };
		g_talentPending = true;
		Service::Request(req, [epoch = g_epoch, id = ++g_talentRequest, level = req["caster"]["level"]](const json& r) {
			if (id != g_talentRequest) return;
			g_talentPending = false;
			if (epoch != g_epoch || !CanPublish()) {
				g_talentAsked = json();
				return;
			}
			if (!r.value("ok", false)) {
				logger::info("talents: learning refused: {}", r.value("error", std::string("no answer")));
				WowUi::Run("__WBTalents(" + Lua(r) + ")");
				return;
			}
			if (Character::Caster()["level"] != level) {   // no spend against a level that changed during validation
				g_talentAsked = json();
				return;
			}
			g_talents = r.at("talents");
			WowUi::Run("__WBTalents(" + Lua(r) + ")");
			++g_epoch;   // the old book's in-flight answer did not contain this allocation
			g_pending = false;
			g_asked = json();
			g_next = 0;
			g_talentAsked = json();
		});
	}

	void RefreshTalents()
	{
		g_talentAsked = json();
		g_nextTalents = 0;
	}

	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length)
	{
		g_loaded.reset();
		std::uint32_t classId = 0, count = 0;
		constexpr auto header = sizeof(classId) + sizeof(count);
		if (a_length < header || a_intfc->ReadRecordData(classId) != sizeof(classId) ||
			a_intfc->ReadRecordData(count) != sizeof(count) || count > 16384 ||
			a_length - header != count * sizeof(int)) {
			logger::error("progression: a damaged saved record; she starts from her skills");
			return;
		}
		std::vector<int> known(count);
		if ((count && a_intfc->ReadRecordData(known.data(), count * sizeof(int)) != count * sizeof(int)) ||
			std::ranges::any_of(known, [](int a_spell) { return a_spell <= 0; })) {
			logger::error("progression: an unreadable saved record; she starts from her skills");
			return;
		}
		if (classId != Character::Get().classId) {
			logger::warn("progression: saved class {} is not her class {}; she starts from her skills", classId, Character::Get().classId);
			return;
		}
		g_loaded = std::move(known);
	}

	void Acquire(std::span<const int> a_spells)
	{
		for (const int spell : a_spells) {
			if (spell > 0 && !std::ranges::contains(g_known, spell)) {
				g_known.push_back(spell);
			}
		}
		++g_epoch;   // an answer already in flight did not ask about these acquisitions
		g_pending = false;
		g_talentPending = false;
		g_talentAsked = json();
		g_asked = json();
	}

	void ResetTalents(std::function<void(bool, std::size_t, std::string)> a_done)
	{
		g_talents = json::object();
		g_talentAsked = json();   // the frame is published again with every point free
		TestBook(false, std::move(a_done));   // the book from her skills alone: the talents' spells go
	}

	json SnapshotForTesting()
	{
		auto known = g_known;
		std::ranges::sort(known);
		return {{"talents", g_talents}, {"known_spells", known}};
	}

	void RestoreForTesting(const json& a_saved)
	{
		++g_epoch;
		++g_bookRequest;
		++g_talentRequest;
		g_known = a_saved.at("known_spells").get<std::vector<int>>();
		g_talents = a_saved.at("talents");
		g_loaded.reset();
		g_loadedTalents.reset();
		g_quiet = true;
		g_ready = g_pending = g_talentPending = false;
		g_asked = g_talentAsked = json();
		g_next = g_nextTalents = 0;
	}

	void TestBook(bool a_all, std::function<void(bool, std::size_t, std::string)> a_done)
	{
		const auto epoch = ++g_epoch;
		g_pending = true;
		g_ready = false;
		g_loaded.reset();
		json skills = json::object();
		for (const auto& [key, value] : kSkills) {
			skills[key] = static_cast<int>(RE::PlayerCharacter::GetSingleton()->AsActorValueOwner()->GetActorValue(value));
		}
		g_asked = skills;
		auto publish = [epoch, id = ++g_bookRequest, done = std::move(a_done)](const json& r) {
			if (id != g_bookRequest) return;
			g_pending = false;
			if (epoch != g_epoch || !CanPublish()) { g_asked = json(); return; }
			if (!r.value("ok", false)) {
				g_asked = json();
				done(false, g_known.size(), r.value("error", std::string("no answer")));
				return;
			}
			Publish(r);
			done(true, g_known.size(), "");
		};
		json req{ { "op", "known_spells" }, { "class", Character::Get().classId }, { "since", g_known },
			{ "caster", { { "skyrim", skills } } } };
		Augment(req["caster"]);   // helpers use the same actual level/allocation as ordinary leveling
		if (!a_all) {
			Service::Request(std::move(req), std::move(publish));
			return;
		}
		try {
			std::ifstream in("Data/SKSE/Plugins/WoWBridge/progression.json");
			const auto data = json::parse(in);
			std::set<int> ranks;
			for (const auto& definition : data.at("classes")) {
				if (definition.at("id") != Character::Get().classId) { continue; }
				for (const auto* key : { "spells", "excluded" }) {
					for (const auto& s : definition.at(key)) {
						if (s.value("source", "") != "talent") { ranks.insert(s.at("spell").get<int>()); }
					}
				}
			}
			if (ranks.empty()) { throw std::runtime_error("no class ranks in progression.json"); }
			req["acquired_spells"] = std::vector<int>(ranks.begin(), ranks.end());
			Service::Request(std::move(req), std::move(publish));
		} catch (const std::exception& e) {
			publish({ { "ok", false }, { "error", e.what() } });
		}
	}

	std::size_t Count() { return g_known.size(); }

	bool Ready() { return g_ready; }
	bool Knows(std::uint32_t a_spell) { return std::ranges::contains(g_known, static_cast<int>(a_spell)); }
}
