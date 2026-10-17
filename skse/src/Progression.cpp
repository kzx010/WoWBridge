#include "Progression.h"

#include <fstream>

#include "Character.h"
#include "Frame.h"
#include "Integration.h"
#include "LuaText.h"
#include "Service.h"
#include "SpellVisuals.h"
#include "TalentWipe.h"
#include "ui/WowUi.h"

namespace Progression
{
	bool Knows(std::uint32_t a_spell);
	void Rebuild(std::vector<int> a_acquired, std::function<void(bool, std::size_t, std::string)> a_done);

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
		std::vector<int>  g_known;          // what the player knows, as last answered
		std::vector<int>  g_books;          // what their spell books taught (also in g_known once answered)
		std::optional<std::vector<int>> g_loadedBooks;
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
		// epoch would otherwise leave their book or talents waiting for good).
		std::uint64_t     g_bookRequest = 0, g_talentRequest = 0;
		double            g_nextTalents = 0;
		TalentWipe::State g_wipe;   // their last talent wipe: its price and game day
		std::optional<TalentWipe::State> g_loadedWipe;
		// The Shrine of Kynareth the player prayed at while its offer stands (1.12's talent master), and the offer's session.
		RE::ObjectRefHandle g_shrine;
		std::uint32_t     g_wipeSession = 0;
		bool              g_wipeClear = false;   // a load ended an offer: the interface hears it once it can

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

		constexpr RE::FormID kShrineOfKynareth = 0x000D987F;   // ACTI ShrineOfKynareth, Skyrim.esm

		double Today()
		{
			const auto* calendar = RE::Calendar::GetSingleton();
			return calendar ? calendar->GetDaysPassed() : 0.0;
		}

		// Within activation reach of the shrine: Skyrim's pick length from its edge (its bounds), as from where the player prays.
		bool AtShrine(RE::TESObjectREFR* a_shrine)
		{
			auto*       player = RE::PlayerCharacter::GetSingleton();
			const auto* base = a_shrine ? a_shrine->GetBaseObject() : nullptr;
			if (!base || a_shrine->IsDisabled() || a_shrine->IsDeleted() || !a_shrine->Is3DLoaded() || player->IsDead() ||
				!a_shrine->GetParentCell() || !player->GetParentCell()) return false;
			if (a_shrine->GetParentCell()->IsInteriorCell() || player->GetParentCell()->IsInteriorCell()) {
				if (a_shrine->GetParentCell() != player->GetParentCell()) return false;
			} else if (a_shrine->GetWorldspace() != player->GetWorldspace()) return false;
			auto*       settings = RE::GameSettingCollection::GetSingleton();
			const auto* pick = settings ? settings->GetSetting("fActivatePickLength") : nullptr;
			const auto& b = base->boundData;
			const float edge = std::hypot(static_cast<float>(std::max(std::abs(b.boundMin.x), std::abs(b.boundMax.x))),
				static_cast<float>(std::max(std::abs(b.boundMin.y), std::abs(b.boundMax.y)))) * a_shrine->GetScale();
			return a_shrine->GetPosition().GetDistance(player->GetPosition()) <= (pick ? pick->GetFloat() : 180.0f) + edge;
		}

		void CloseWipe()
		{
			g_shrine = {};
			++g_wipeSession;
			WowUi::Run("__WBTalentWipe(nil)");
		}

		// A prayer at a Shrine of Kynareth with points spent: the client's own CONFIRM_TALENT_WIPE, at this wipe's price.
		void OfferWipe(RE::TESObjectREFR* a_shrine)
		{
			if (!Integration::Active() || !CanPublish() || !Character::Get().classId || g_talents.empty() || !AtShrine(a_shrine)) {
				return;
			}
			g_shrine = a_shrine->CreateRefHandle();
			const int cost = TalentWipe::Cost(g_wipe, Today());
			WowUi::Run(std::format("__WBTalentWipe({}, {})", ++g_wipeSession, cost));
			logger::info("talents: a Shrine of Kynareth offers their talent wipe for {} septims", cost);
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
					g_talentAsked = json();   // asked again with what the player has now
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
		// to the first free slot of their main bar, then of the bottom-left bar. Where the player put them stays.
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
			if (Character::Get().classId == 11) {   // a druid's shapeshift bar: the forms the player knows, learned in its order
				std::size_t forms = 0;
				for (const int spell : { Knows(9634) ? 9634 : 5487, 1066, 768, 783 }) {
					if (!Knows(static_cast<std::uint32_t>(spell))) {
						break;
					}
					++forms;
				}
				WowUi::Run(std::format("__WBDruidForms({}, {}, {})", Knows(9634) ? 9634 : 5487, forms, Knows(24858) ? "true" : "false"));
			} else if (Character::Get().classId == 2) {
				WowUi::Run("__WBPaladinAuras()");   // their auras on the stance bar, once their book is placed
			}
			// Their spells' effects: the class's every rank (they share their models), loaded once in the background.
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
		if (std::exchange(g_wipeClear, false)) {
			WowUi::Run("__WBTalentWipe(nil)");
		}
		if (g_shrine && !AtShrine(g_shrine.get().get())) {
			CloseWipe();   // the player walked away: CheckTalentMasterDist fails and the popup's OnUpdate hides it
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
		// What the player learned stays learned (a skill lowered for a while does not unlearn it): their known spells go as acquired,
		// with their books' ranks (a rank just read is not known yet: this answer announces it).
		auto acquired = g_known;
		for (const int spell : g_books) {
			if (!std::ranges::contains(acquired, spell)) {
				acquired.push_back(spell);
			}
		}
		json req = { { "op", "known_spells" }, { "class", Character::Get().classId }, { "caster", caster },
			{ "since", g_known }, { "acquired_spells", std::move(acquired) } };
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
		g_books = g_loadedBooks ? std::move(*g_loadedBooks) : std::vector<int>{};
		g_loadedBooks.reset();
		g_quiet = true;
		g_ready = false;
		g_pending = false;
		g_next = 0;
		g_talents = g_loadedTalents ? std::move(*g_loadedTalents) : json::object();
		g_loadedTalents.reset();
		g_talentAsked = json();
		g_talentPending = false;
		g_nextTalents = 0;
		g_wipe = g_loadedWipe.value_or(TalentWipe::State{});
		g_loadedWipe.reset();
		if (g_shrine) {   // no publication here: kPreLoadGame can already be in a loading screen
			g_shrine = {};
			++g_wipeSession;
			g_wipeClear = true;
		}
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		const auto classId = Character::Get().classId;
		const auto count = static_cast<std::uint32_t>(g_known.size());
		if (!a_intfc->OpenRecord(kRecord, kRecordVersion) || !a_intfc->WriteRecordData(classId) ||
			!a_intfc->WriteRecordData(count) || (count && !a_intfc->WriteRecordData(g_known.data(), count * sizeof(int)))) {
			logger::error("progression: their known spells were not saved");
		}
		const auto books = static_cast<std::uint32_t>(g_books.size());
		if (!a_intfc->OpenRecord(kBookRecord, kBookRecordVersion) || !a_intfc->WriteRecordData(classId) || !a_intfc->WriteRecordData(books) ||
			(books && !a_intfc->WriteRecordData(g_books.data(), books * sizeof(int)))) {
			logger::error("progression: their books' ranks were not saved");
		}
		json record = { { "class", classId }, { "talents", g_talents } };
		TalentWipe::Write(record, g_wipe);
		const auto text = record.dump();
		if (!a_intfc->OpenRecord(kTalentRecord, kTalentRecordVersion) ||
			!a_intfc->WriteRecordData(text.data(), static_cast<std::uint32_t>(text.size()))) {
			logger::error("talents: their allocation was not saved");
		}
	}

	void LoadTalents(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length)
	{
		g_loadedTalents.reset();
		g_loadedWipe.reset();
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
		g_loadedWipe = TalentWipe::Read(saved);   // a record from before wipes: never wiped
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
			logger::error("progression: a damaged saved record; the player starts from their skills");
			return;
		}
		std::vector<int> known(count);
		if ((count && a_intfc->ReadRecordData(known.data(), count * sizeof(int)) != count * sizeof(int)) ||
			std::ranges::any_of(known, [](int a_spell) { return a_spell <= 0; })) {
			logger::error("progression: an unreadable saved record; the player starts from their skills");
			return;
		}
		if (classId != Character::Get().classId) {
			logger::warn("progression: saved class {} is not their class {}; the player starts from their skills", classId, Character::Get().classId);
			return;
		}
		g_loaded = std::move(known);
	}

	void LoadBooks(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length)
	{
		g_loadedBooks.reset();
		std::uint32_t classId = 0, count = 0;
		constexpr auto header = sizeof(classId) + sizeof(count);
		if (a_length < header || a_intfc->ReadRecordData(classId) != sizeof(classId) || a_intfc->ReadRecordData(count) != sizeof(count) ||
			count > 4096 || a_length - header != count * sizeof(int)) {
			logger::error("progression: a damaged record of their books' ranks");
			return;
		}
		std::vector<int> books(count);
		if ((count && a_intfc->ReadRecordData(books.data(), count * sizeof(int)) != count * sizeof(int)) ||
			std::ranges::any_of(books, [](int a_spell) { return a_spell <= 0; })) {
			logger::error("progression: an unreadable record of their books' ranks");
			return;
		}
		if (classId != Character::Get().classId) {   // the service refuses another class's ranks
			logger::warn("progression: books' ranks of class {}, not theirs ({})", classId, Character::Get().classId);
			return;
		}
		g_loadedBooks = std::move(books);   // Reset restores them
	}

	bool LearnBook(std::uint32_t a_spell)
	{
		const int spell = static_cast<int>(a_spell);
		if (spell <= 0 || Knows(a_spell) || std::ranges::contains(g_books, spell)) {
			return false;
		}
		g_books.push_back(spell);
		++g_epoch;   // asked again at once, with it
		g_pending = false;
		g_asked = json();
		g_next = 0;
		return true;
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
		Rebuild(g_books, std::move(a_done));   // the book from their skills and their books: the talents' spells go
	}

	void Init()
	{
		// Skyrim's activation goes on (Kynareth's blessing is the shrine's own script); this only listens. The event can
		// come off the main thread: the offer is made on the main one.
		static struct Prayers : RE::BSTEventSink<RE::TESActivateEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				const auto* base = a_event && a_event->objectActivated ? a_event->objectActivated->GetBaseObject() : nullptr;
				if (base && base->GetFormID() == kShrineOfKynareth && a_event->actionRef.get() == RE::PlayerCharacter::GetSingleton()) {
					SKSE::GetTaskInterface()->AddTask([shrine = a_event->objectActivated->CreateRefHandle()] { OfferWipe(shrine.get().get()); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		} prayers;
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESActivateEvent>(&prayers);
	}

	void ConfirmTalentWipe(std::uint32_t a_session, int a_cost)
	{
		// 1.12's MSG_TALENT_WIPE_CONFIRM: only for the offer still open, at the shrine the player still stands at. The offer stays
		// open after it (a repeated accept finds nothing to unlearn) until the player walks away.
		const auto shrine = g_shrine.get();
		if (a_session != g_wipeSession || !shrine || !CanPublish()) {
			return;
		}
		if (!AtShrine(shrine.get())) {
			CloseWipe();
			return;
		}
		if (g_talents.empty()) {
			return;
		}
		const int cost = TalentWipe::Cost(g_wipe, Today());
		if (cost != a_cost) {   // a month turned while the player read it: asked again at the new price
			OfferWipe(shrine.get());
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(0xF);
		if (!gold || player->GetGoldAmount() < cost) {
			WowUi::Run("__WBUiError('ERR_NOT_ENOUGH_MONEY')");   // the server's BUY_ERR_NOT_ENOUGHT_MONEY
			return;
		}
		player->RemoveItem(gold, cost, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
		g_wipe = { cost, Today() };
		logger::info("talents: wiped at a Shrine of Kynareth for {} septims", cost);
		ResetTalents([](bool a_ok, std::size_t, std::string a_error) {
			if (!a_ok) logger::warn("talents: their book after the wipe: {}", a_error);   // Tick asks again
		});
	}

	json SnapshotForTesting()
	{
		auto known = g_known, books = g_books;
		std::ranges::sort(known);
		std::ranges::sort(books);
		json saved = {{"talents", g_talents}, {"known_spells", known}, {"books", books}};
		TalentWipe::Write(saved, g_wipe);
		return saved;
	}

	void RestoreForTesting(const json& a_saved)
	{
		++g_epoch;
		++g_bookRequest;
		++g_talentRequest;
		g_known = a_saved.at("known_spells").get<std::vector<int>>();
		g_books = a_saved.value("books", std::vector<int>{});
		g_talents = a_saved.at("talents");
		if (a_saved.contains("reset_cost")) {
			g_wipe = TalentWipe::Read(a_saved);
		}
		g_loaded.reset();
		g_loadedBooks.reset();
		g_loadedTalents.reset();
		g_quiet = true;
		g_ready = g_pending = g_talentPending = false;
		g_asked = g_talentAsked = json();
		g_next = g_nextTalents = 0;
	}

	// Their book asked afresh from their skills and these ranks (TestBook, a talent wipe), published when it answers.
	void Rebuild(std::vector<int> a_acquired, std::function<void(bool, std::size_t, std::string)> a_done)
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
			{ "caster", { { "skyrim", skills } } }, { "acquired_spells", std::move(a_acquired) } };
		Augment(req["caster"]);   // helpers use the same actual level/allocation as ordinary leveling
		Service::Request(std::move(req), std::move(publish));
	}

	void TestBook(bool a_all, std::function<void(bool, std::size_t, std::string)> a_done)
	{
		if (!a_all) {
			g_books.clear();
			Rebuild({}, std::move(a_done));
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
			ranks.insert(g_books.begin(), g_books.end());
			Rebuild(std::vector<int>(ranks.begin(), ranks.end()), std::move(a_done));
		} catch (const std::exception& e) {
			a_done(false, g_known.size(), e.what());
		}
	}

	std::size_t Count() { return g_known.size(); }

	bool Ready() { return g_ready; }
	bool Knows(std::uint32_t a_spell) { return std::ranges::contains(g_known, static_cast<int>(a_spell)); }
	bool Taught(std::uint32_t a_spell) { return Knows(a_spell) || std::ranges::contains(g_books, static_cast<int>(a_spell)); }
}
