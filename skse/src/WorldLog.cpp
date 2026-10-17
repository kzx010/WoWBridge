#include "WorldLog.h"

#include "Frame.h"
#include "LuaText.h"
#include "ui/WowUi.h"

namespace WorldLog
{
	namespace
	{
		constexpr float kUnitsPerYard = 64.0f;

		// Each unit's health over the last frames: a blow is what it took, from the highest of them to a moment after.
		struct Health
		{
			float samples[3]{};
		};
		std::map<RE::FormID, Health> g_health;
		std::set<RE::FormID>         g_dead;   // deaths told (the event comes as it dies and when it is dead)

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

		bool Near(RE::Actor* a_actor)
		{
			return a_actor && a_actor->GetPosition().GetDistance(Player()->GetPosition()) / kUnitsPerYard <= kRadius;
		}

		// The client's unit kinds, as the player sees them now (docs/combat-world.md).
		std::string Unit(RE::Actor* a_actor)
		{
			const char* category = a_actor == Player()          ? "self" :
			                       a_actor->IsPlayerTeammate()   ? "party" :
			                       a_actor->IsHostileToActor(Player()) ? "hostileCreature" :
			                                                       "friendlyCreature";
			return std::format("{{ name = {}, category = \"{}\" }}", LuaQuote(a_actor->GetDisplayFullName()), category);
		}

		// A Skyrim spell's school as the client's: fire, frost, shock (nature, as WoW's lightning), poison (nature), else arcane.
		int School(const RE::MagicItem* a_spell)
		{
			const auto* effect = a_spell ? a_spell->GetCostliestEffectItem() : nullptr;
			switch (effect && effect->baseEffect ? effect->baseEffect->data.resistVariable : RE::ActorValue::kNone) {
			case RE::ActorValue::kResistFire:
				return 2;
			case RE::ActorValue::kResistFrost:
				return 4;
			case RE::ActorValue::kResistShock:
			case RE::ActorValue::kPoisonResist:
				return 3;
			default:
				return 6;
			}
		}

		void Report(std::string a_fields) { WowUi::Run("__WBCombatEvent({ " + a_fields + " })"); }

		class DeathSink : public RE::BSTEventSink<RE::TESDeathEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				auto* dying = a_event && a_event->actorDying ? a_event->actorDying->As<RE::Actor>() : nullptr;
				if (!dying) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto*      killer = a_event->actorKiller ? a_event->actorKiller->As<RE::Actor>() : nullptr;
				const auto handle = dying->CreateRefHandle();
				const bool hers = killer && killer->IsPlayerRef();
				SKSE::GetTaskInterface()->AddTask([handle, hers] {
					auto dead = handle.get();
					if (!dead || !Near(dead.get()) || !g_dead.insert(dead->GetFormID()).second) {
						return;
					}
					Report("kind = \"death\", target = " + Unit(dead.get()));
					if (hers && dead.get() != Player()) {
						Report("kind = \"kill\", target = " + Unit(dead.get()));
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Init()
	{
		static DeathSink deaths;
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESDeathEvent>(&deaths);
	}

	void Tick()
	{
		auto* player = Player();
		if (!player || !player->Is3DLoaded()) {
			return;
		}
		std::map<RE::FormID, Health> next;
		auto sample = [&](RE::Actor* a_actor) {
			auto&       h = next[a_actor->GetFormID()];
			const auto& was = g_health[a_actor->GetFormID()];
			h.samples[2] = was.samples[1], h.samples[1] = was.samples[0];
			h.samples[0] = a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
			if (!a_actor->IsDead()) {
				g_dead.erase(a_actor->GetFormID());   // alive again (raised): its next death is told
			}
		};
		sample(player);
		for (auto& h : RE::ProcessLists::GetSingleton()->highActorHandles) {
			if (auto a = h.get(); a && Near(a.get())) {
				sample(a.get());
			}
		}
		g_health = std::move(next);
	}

	void Struck(RE::Actor* a_victim, RE::Actor* a_attacker, RE::FormID a_source, bool a_blocked)
	{
		if (!a_victim || !a_attacker || a_attacker == Player() || (!Near(a_victim) && !Near(a_attacker))) {
			return;   // theirs are logged where the player deals them
		}
		const auto it = g_health.find(a_victim->GetFormID());
		const float before = it == g_health.end() ? -1.0f : std::max({ it->second.samples[0], it->second.samples[1], it->second.samples[2] });
		auto*       spell = RE::TESForm::LookupByID<RE::MagicItem>(a_source);
		const auto  victim = a_victim->CreateRefHandle();
		std::string head = std::format("source = {}, target = {}", Unit(a_attacker), Unit(a_victim));
		std::string named = spell && spell->GetFullName() && *spell->GetFullName() ?
		                        std::format("kind = \"damage\", spell = {}, school = {}, ", LuaQuote(spell->GetFullName()), School(spell)) :
		                        std::string("kind = \"swing\", ");
		// What it took, a moment after (Skyrim's health, as their frames show it).
		Frame::After(0.1, [victim, before, head = std::move(head), named = std::move(named), a_blocked] {
			auto v = victim.get();
			if (!v || before < 0.0f) {
				return;
			}
			const int amount = static_cast<int>(std::lround(before - v->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth)));
			if (amount > 0) {
				Report(named + head + std::format(", amount = {}", amount));
			} else if (a_blocked && named.starts_with("kind = \"swing\"")) {
				Report(named + head + ", outcome = \"block\"");
			}
		});
	}

	void Defense(RE::Actor* a_attacker, const nlohmann::json& a_defense, float a_applied)
	{
		const float damage = a_defense.value("damage", 0.f);
		const float absorbed = std::max(0.f, damage - a_applied);
		auto outcome = a_defense.value("outcome", std::string("hit"));
		// Partial blocks are landed hits with a trailer; full blocks and full absorbs use the client's miss strings.
		if (outcome == "block" && a_applied > 0) outcome = "hit";
		if (outcome == "crush") outcome = "hit"; // the world path accepts a normal hit with its actual crushing damage
		if (damage > 0 && a_applied <= 0) outcome = "absorb";
		const auto source = a_attacker ? Unit(a_attacker) : std::string("{ name = \"Unknown\", category = \"hostileCreature\" }");
		Report(std::format("kind = \"swing\", source = {}, target = {}, outcome = {}, school = {}, amount = {}, blocked = {}, absorbed = {}",
			source, Unit(Player()), LuaQuote(outcome), a_defense.value("school", 0), std::lround(a_applied),
			std::lround(a_defense.value("blocked", 0.f)), std::lround(absorbed)));
	}
}
