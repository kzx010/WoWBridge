#include "Mounts.h"
#include "Cast.h"
#include "Integration.h"

#include "Forms.h"
#include "Gear.h"

#include "Audio.h"
#include "Avatar.h"
#include "Bridge.h"
#include "Carriers.h"
#include "AuraPersistence.h"
#include "CastPersistence.h"
#include "Character.h"
#include "ChannelTiming.h"
#include "ControlTiming.h"
#include "Frame.h"
#include "Input.h"
#include "LuaText.h"
#include "Picker.h"
#include "Progression.h"
#include "Service.h"
#include "WorldLog.h"
#include "SkyrimSpells.h"
#include "SpellVisuals.h"
#include "ui/WowUi.h"

namespace Cast
{
	namespace
	{
		using json = nlohmann::json;

		// The slice character: level 60, Stormrage Raiment 8/8, Staff of Jordan, Idol of Rejuvenation (STATUS M3).
		constexpr float kSkyrimToWowHp = 4.0f;   // the root-break roll weighs damage on WoW's health scale (docs/authority.md)
		constexpr float kUnitsPerYard = 64.0f;
		const char* const kSchool[] = { "Physical", "Holy", "Fire", "Nature", "Frost", "Shadow", "Arcane" };

		struct InFlight
		{
			std::uint32_t   spell = 0;
			json            outcome;
			RE::ActorHandle target;   // empty: yourself
			double          endAt = 0;
			std::uint64_t   loop = 0;   // the precast sound
			double          castTime = 0;
			int             delays = 0;   // pushbacks so far
			SpellVisuals::Handle precast = 0;
		};

		// Spell.h GetNextDelayAtDamageMsTime: 1000, 800, 600, 400, then 200 ms.
		int NextDelayMs(int& a_count) { return a_count < 5 ? 1000 - (a_count++) * 200 : 200; }

		struct Aura
		{
			std::uint32_t                        spell = 0;
			std::string                          name;
			RE::ActorHandle                      target;   // empty: yourself
			bool                                 harmful = false;
			int                                  school = 0;
			double                               start = 0, end = 0;
			std::vector<std::pair<double, int>>  ticks;    // seconds after start, amount (WoW)
			std::vector<std::string>             kinds;    // per tick: "damage" | "heal"
			std::size_t                          next = 0;
			bool                                 root = false;
			double                               controlEnd = 0;       // stun/root ends independently of a longer bleed
			RE::NiPoint3                         rootAt;          // where the root holds its victim
			double                               breakAt = -1;    // the heartbeat resist the service rolled (root)
			int                                  victimLevel = 0; // damage break (root)
			SpellVisuals::Handle                 state = 0;       // the state kit's effect on its unit
			std::vector<Carriers::Change>        skyrim;          // its Skyrim magic effects (Carriers::Buff) while it lasts
			int                                  shield = 0;      // Nature damage to each melee attacker (Thorns)
			bool                                 sleep = false;   // asleep (Hibernate) ...
			bool                                 wakes = false;   // ... until any damage
			bool                                 noPushback = false;   // blows do not delay her casting (Barkskin)
			std::string                          dispel;          // a periodic dispel's kind (Abolish Poison) ...
			double                               dispelEvery = 0, nextDispel = 0;
			int                                  dispelCount = 0;
			bool                                 form = false;          // her form, as the client shows it (a buff)
			bool                                 endsWithForm = false;  // Prowl, Dash, Tiger's Fury: gone when she shifts
			std::uint32_t                        trackingAura = 0, trackingMask = 0;
			json                                 formOutcome;   // resolved stance data needed when restoring tracking
			bool                                 stealth = false;       // Prowl or Shadowmeld
			bool                                 reveal = false;        // Faerie Fire blocks native concealment
			bool                                 lockout = false;       // Feral Charge's native spell interruption
			float                                armorReduction = 0;    // original WoW units, for later melee requests
			float                                wowPerTick = -1;      // Swiftmend's input, before native conversion
			json                                 tooltip;              // effect values at application (WoW units)
			bool                                 respite = false;      // Skyrim stamina alongside each healing tick
			float                                threat = 1;           // healing threat snapshot
		};

		// A channelled area (Hurricane): ticks on whatever is inside the circle each amplitude, while you stand still.
		struct Channel
		{
			std::uint32_t           spell = 0;
			std::string             name;
			int                     school = 0;
			RE::NiPoint3            at;               // the circle's centre
			float                   radius = 0;       // units
			double                  start = 0, end = 0, amplitude = 1, nextTick = 0;
			int                     tick = 0;
			std::set<RE::FormID>    hit;              // shown the aura's debuff
			std::uint64_t           loop = 0;         // the channel's sound
			std::uint32_t           interruptFlags = 0;   // ChannelInterruptFlags
			int                     delays = 0;
			SpellVisuals::Handle    visual = 0;
			SpellVisuals::Handle    area = 0;         // its storm where it was placed
			bool                    heal = false;     // a party heal (Tranquility): her and her followers in the circle
			float                   pushbackResist = 0;
		};

		bool                 g_pending = false;           // a resolve is in flight
		std::uint32_t        g_request = 0;               // Stop also cancels an unanswered resolve
		struct FormFollowup { std::uint32_t spell, shift; RE::ActorHandle target; bool targeted; double expires; };
		std::optional<FormFollowup> g_formFollowup;
		std::uint32_t        g_epoch = 0;                 // replies from before a Reset are dropped
		std::uint32_t        g_placing = 0;               // a ground spell waiting for its place
		std::optional<Channel> g_channel;
		std::optional<InFlight> g_cast;
		std::vector<Aura>    g_auras;
		double               g_gcdUntil = 0;

		std::map<std::uint32_t, double> g_cooldowns;
		std::mt19937         g_rng{ std::random_device{}() };
		std::optional<json>  g_loaded;   // what the save kept, until the load's Reset brings it back
		double               g_manaSpentAt = -100;
		json                 g_talentEvidence = json::object();
		std::set<std::uint32_t> g_incomingPending;
		std::map<std::uint32_t, float> g_spellRanges;

		float SpellRange(std::uint32_t a_spell)
		{
			const auto range = g_spellRanges.find(a_spell);
			return range == g_spellRanges.end() ? WowUi::Facts(a_spell).rangeMax : range->second;
		}

		double WallNow() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }
		std::string          Name(RE::Actor* a_actor) { return a_actor ? a_actor->GetDisplayFullName() : ""; }

		void Lua(std::string a_lua) { WowUi::Run(std::move(a_lua)); }

		std::string TooltipLua(const json& a_values)
		{
			if (!a_values.is_object()) return "nil";
			std::string text = "{effects={";
			for (const auto& range : a_values.at("effects")) {
				text += std::format("{{{}, {}}},", range.at(0).get<double>(), range.at(1).get<double>());
			}
			text += "},";
			for (const auto* key : { "duration_ms", "cast_ms", "range_max", "cooldown_ms" }) {
				text += std::format("{}={},", key, a_values.at(key).get<double>());
			}
			if (a_values.contains("proc_chance")) text += std::format("proc_chance={},", a_values.at("proc_chance").get<double>());
            for (const auto* key : {"health_factor","minimum_health"})
                if (a_values.contains(key)) text += std::string(key)+"="+a_values.at(key).dump()+",";
			return text + "}";
		}
		void UiError(const char* a_key) { Lua(std::format("__WBUiError({})", LuaQuote(a_key))); }

		// One combat log line, written with the client's GlobalStrings format.
		template <class... Args>
		void Log(const char* a_event, const char* a_format, const Args&... a_args)
		{
			std::string args;
			((args += ", " + [](const auto& v) {
				if constexpr (std::is_arithmetic_v<std::decay_t<decltype(v)>>) {
					return std::to_string(v);
				} else {
					return LuaQuote(v);
				}
			}(a_args)), ...);
			Lua(std::format("__WBLog({}, {}{})", LuaQuote(a_event), LuaQuote(a_format), args));
		}

		const char* Reaction(RE::Actor* a_actor) { return Bridge::ReactionName(a_actor); }   // as the service reads it
		bool MovingHerself();

		// The service's caster now: what she wears and her skills (Character), the WoW auras on her (their spell mods
		// apply: Barkskin's second of casting time) and the reagents in her bags.
		json CasterNow()
		{
			auto c = Character::Caster();
			auto reagents = json::object();
			for (const auto& [entry, n] : Gear::ReagentCounts()) {
				reagents[std::to_string(entry)] = n;
			}
			c["reagents"] = reagents;
			Forms::Augment(c);
			c["in_combat"] = Player()->IsInCombat();
			c["moving"] = MovingHerself() || Player()->IsMoving();
			return c;
		}

		// The native heal response has already converted/rounded its ticks. Preserve the service's original
		// magnitude too for Swiftmend, by resolving the same stateless request/seed without Skyrim conversion.
		void RequestOutcome(json a_request, Service::Reply a_reply)
		{
			const auto name = WowUi::Facts(a_request.value("spell", 0u)).name;
			if (name != "Rejuvenation" && name != "Regrowth") {
				Service::Request(std::move(a_request), std::move(a_reply));
				return;
			}
			auto raw = a_request;
			raw["caster"].erase("skyrim");
			Service::Request(std::move(a_request), [raw = std::move(raw), reply = std::move(a_reply)](const json& native) {
				if (!native.value("ok", false)) {
					reply(native);
					return;
				}
				Service::Request(raw, [native, reply](const json& wow) {
					if (!wow.value("ok", false)) {
						reply(wow);
						return;
					}
					auto out = native;
					for (auto& aura : out["auras"]) {
						if (aura.value("type", "") != "heal") {
							continue;
						}
						for (const auto& original : wow["auras"]) {
							if (original.value("type", "") == "heal" && original.value("eff", 0) == aura.value("eff", 0)) {
								aura["wow_per_tick"] = original.at("per_tick");
								break;
							}
						}
					}
					reply(out);
				});
			});
		}

		// Her party, as a party aura reaches it (Gift of the Wild, Tranquility): her and her living followers within a
		// radius of a place. An empty handle is herself.
		std::vector<RE::ActorHandle> Party(const RE::NiPoint3& a_at, float a_radius)
		{
			std::vector<RE::ActorHandle> out;
			auto*                        player = RE::PlayerCharacter::GetSingleton();
			if (player->GetPosition().GetDistance(a_at) <= a_radius) {
				out.emplace_back();
			}
			for (auto& handle : RE::ProcessLists::GetSingleton()->highActorHandles) {
				auto a = handle.get();
				if (a && a.get() != player && !a->IsDead() && a->IsPlayerTeammate() && a->GetPosition().GetDistance(a_at) <= a_radius) {
					out.push_back(handle);
				}
			}
			return out;
		}

		// Skyrim has no healing threat table. A reduced-threat heal can transfer
		// a pursuer to a nearby living follower instead of making the healer invisible.
		void HealingThreat(float a_multiplier)
		{
			g_talentEvidence["threat_multiplier"] = a_multiplier;
			if (a_multiplier >= 1) return;
			auto* player = Player();
			const auto party = Party(player->GetPosition(), 30 * kUnitsPerYard);
			for (auto& handle : RE::ProcessLists::GetSingleton()->highActorHandles) {
				auto actor = handle.get();
				auto* combat = actor ? actor->GetActorRuntimeData().combatController : nullptr;
				if (!actor || actor.get() == player || actor->IsDead() || actor->IsPlayerTeammate() ||
					!combat || combat->targetHandle.get().get() != player ||
					actor->GetPosition().GetDistance(player->GetPosition()) > 30 * kUnitsPerYard) continue;
				RE::Actor* follower = nullptr;
				for (const auto& h : party) {
					auto candidate = h.get();
					if (candidate && (!follower || candidate->GetPosition().GetDistance(actor->GetPosition()) <
						follower->GetPosition().GetDistance(actor->GetPosition()))) follower = candidate.get();
				}
				if (follower && std::uniform_real_distribution<float>(0, 1)(g_rng) >= a_multiplier) {
					actor->StopCombat();
					if (actor->StartCombat(follower)) g_talentEvidence["threat_redirects"] = g_talentEvidence.value("threat_redirects", 0) + 1;
				}
			}
		}

		// The living enemies around a place, nearest first, at most a_count (Swipe's, Demoralizing Roar's).
		std::vector<RE::ActorHandle> Hostiles(const RE::NiPoint3& a_at, float a_radius, std::size_t a_count, bool a_inFront = false)
		{
			auto*                                         player = RE::PlayerCharacter::GetSingleton();
			std::vector<std::pair<float, RE::ActorHandle>> found;
			const float                                   h = player->GetHeading(false);
			for (auto& handle : RE::ProcessLists::GetSingleton()->highActorHandles) {
				auto a = handle.get();
				if (!a || a.get() == player || a->IsDead() || !a->Is3DLoaded() || !Bridge::Attackable(a.get()) || a->IsPlayerTeammate() ||
					Bridge::Reaction(a.get()) > 2) {
					continue;
				}
				const auto  to = a->GetPosition() - a_at;
				const float d = a->GetPosition().GetDistance(a_at);
				if (d <= a_radius && (!a_inFront || std::sin(h) * to.x + std::cos(h) * to.y >= 0.0f)) {
					found.emplace_back(d, handle);
				}
			}
			std::ranges::sort(found, {}, &std::pair<float, RE::ActorHandle>::first);
			std::vector<RE::ActorHandle> out;
			for (std::size_t i = 0; i < found.size() && i < a_count; ++i) {
				out.push_back(found[i].second);
			}
			return out;
		}

		// Dispel: Skyrim's poisons on the unit (detrimental effects resisted by Poison Resist) for Cure and Abolish
		// Poison. Skyrim has no curses (Remove Curse finds nothing) and a druid dispels nothing else. Logged as the client
		// logs a dispel.
		int Dispel(RE::Actor* a_actor, const std::string& a_type, int a_count)
		{
			auto* effects = a_actor && a_type == "Poison" ? a_actor->GetActiveEffectList() : nullptr;
			int   removed = 0;
			if (!effects) {
				return 0;
			}
			std::vector<RE::ActiveEffect*> poisons;
			for (auto* e : *effects) {
				const auto* base = e ? e->GetBaseObject() : nullptr;
				if (base && base->IsDetrimental() && base->data.resistVariable == RE::ActorValue::kPoisonResist &&
					!e->flags.any(RE::ActiveEffect::Flag::kDispelled)) {
					poisons.push_back(e);
				}
			}
			for (auto* e : poisons) {
				if (removed >= a_count) {
					break;
				}
				const auto* spell = e->spell ? e->spell->As<RE::TESFullName>() : nullptr;
				const std::string name = spell && spell->GetFullName() ? spell->GetFullName() : "";
				e->Dispel(true);
				++removed;
				if (a_actor == Player()) {
					Log("CHAT_MSG_SPELL_BREAK_AURA", "AURADISPELSELF", name);
				} else {
					Log("CHAT_MSG_SPELL_BREAK_AURA", "AURADISPELOTHER", Name(a_actor), name);
				}
			}
			return removed;
		}

		// Rebirth: back to life with the health the service gives (Skyrim's), as much magicka as it gives.
		void Resurrect(RE::Actor* a_actor, int a_health, int a_mana)
		{
			if (!a_actor || !a_actor->IsDead()) {
				return;
			}
			a_actor->Resurrect(false, true);
			auto*       av = a_actor->AsActorValueOwner();
			const float max = av->GetPermanentActorValue(RE::ActorValue::kHealth);
			if (a_health > 0 && a_health < max) {
				av->DamageActorValue(RE::ActorValue::kHealth, max - static_cast<float>(a_health));
			}
			if (const float magicka = av->GetActorValue(RE::ActorValue::kMagicka); a_mana >= 0 && a_mana < magicka) {
				av->DamageActorValue(RE::ActorValue::kMagicka, magicka - static_cast<float>(a_mana));
			}
		}

		// Her mana is Skyrim's magicka (her gear's Intellect is Fortify Magicka; Skyrim regenerates it), and the
		// service's cost is in it. Magicka spent trains the spell's school, as a Skyrim spell's does.
		void Spend(const json& a_outcome)
		{
			for (const auto& r : a_outcome.value("reagents", json::array())) {   // Rebirth's seed, Gift's thornroot
				Gear::Consume(r.value("item", 0u), r.value("count", 1));
			}
			const float cost = a_outcome.value("cost", 0.0f);
			g_talentEvidence["paid_cost"] = cost;
			g_talentEvidence["paid_spell"] = a_outcome.value("spell", 0u);
			if (Forms::Pay(a_outcome)) {   // rage or energy: WoW's own, not her magicka
				Character::Train(a_outcome.value("skill", ""), cost);
				return;
			}
			if (cost > 0) {
				g_manaSpentAt = Frame::Now();
				Player()->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kMagicka, cost);
				Character::Train(a_outcome.value("skill", ""), cost);
			}
		}

		// Damage may break a root on its victim (Unit::HandleRemoveByDamageChanceProc): the service rolls it, for damage
		// from anything but the root's own spell.
		void DamageBreak(RE::Actor* a_victim, int a_amount, std::uint32_t a_from)
		{
			for (auto& a : g_auras) {   // any damage wakes a sleeping beast (Hibernate)
				if (a.sleep && a.wakes && a.target && a.target.get().get() == a_victim) {
					a.end = Frame::Now();
				}
			}
			for (const auto& a : g_auras) {
				if (!a.root || a.spell == a_from || a.target.get().get() != a_victim) {
					continue;
				}
				Service::Request({ { "op", "damage_taken" }, { "damage", std::lround(a_amount * kSkyrimToWowHp) }, { "victim_level", a.victimLevel },
									 { "seed", std::uniform_int_distribution<std::uint32_t>()(g_rng) } },
					[spell = a.spell, handle = a.target](const json& r) {
						if (!r.value("break", false)) {
							return;
						}
						for (auto& x : g_auras) {
							if (x.spell == spell && x.target == handle) {
								x.end = Frame::Now();   // ends on the next tick, with its fade line
							}
						}
					});
			}
		}

		void ApplyHealth(RE::Actor* a_target, const std::string& a_kind, int a_amount, std::uint32_t a_spell)
		{
			if (!a_target || a_amount <= 0) {
				return;
			}
			if (a_kind == "damage") {
				Carriers::Damage(a_target, static_cast<float>(a_amount));
				DamageBreak(a_target, a_amount, a_spell);
			} else {
				Carriers::Heal(a_target, static_cast<float>(a_amount));
			}
		}

		bool FullHealth(RE::Actor* a_actor)
		{
			auto* av = a_actor->AsActorValueOwner();
			return av->GetActorValue(RE::ActorValue::kHealth) >= av->GetPermanentActorValue(RE::ActorValue::kHealth) +
			                                                     a_actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kHealth) - 0.01f;
		}

		void Reveal(RE::Actor* a_actor)
		{
			if (!a_actor) return;
			std::vector<RE::ActiveEffect*> hidden;
			if (auto* effects = a_actor->GetActiveEffectList()) {
				for (auto* effect : *effects) {
					const auto* base = effect ? effect->GetBaseObject() : nullptr;
					if (base && (base->data.archetype == RE::EffectSetting::Archetype::kInvisibility ||
						base->data.primaryAV == RE::ActorValue::kInvisibility) && !effect->flags.any(RE::ActiveEffect::Flag::kDispelled)) {
						hidden.push_back(effect);
					}
				}
			}
			for (auto* effect : hidden) {
				effect->Dispel(true);
				g_talentEvidence["invisibility_dispelled"] = g_talentEvidence.value("invisibility_dispelled",0) + 1;
			}
		}

		// One spell's auras on one unit (an empty handle: her), as the service describes them: periodic ticks, a root,
		// Skyrim magic effects (buffs, Faerie Fire), a damage shield, sleep, pushback immunity, a periodic dispel.
		// Her form's aura and those that end with it (Prowl, Dash, Tiger's Fury): taken off as she shifts.
		void RemoveFormAuras(bool a_withForm)
		{
			std::erase_if(g_auras, [&](const Aura& x) {
				if (x.target || !(x.endsWithForm || (a_withForm && x.form))) {
					return false;
				}
				SpellVisuals::Stop(x.state);
				if (!x.skyrim.empty()) {
					Carriers::Unbuff(Player(), x.spell);
				}
				if (x.stealth) {
					Forms::EndStealth(x.spell);
				}
				Lua(std::format("__WBAuraRemove(\"player\", {})", x.spell));
				return true;
			});
		}

		void AddAura(std::uint32_t a_spell, const json& a_outcome, RE::ActorHandle a_on)
		{
			const auto auras = a_outcome.value("auras", json::array());
			if (const auto shift = std::ranges::find_if(auras, [](const json& a) { return a.value("type", "") == "shapeshift"; }); shift != auras.end()) {
				RemoveFormAuras(true);   // a new form replaces the old one; leaving one adds none
				if (shift->value("action", "") == "leave") {
					return;
				}
			}
			auto*      target = a_on.get().get();
			const bool onSelf = !a_on;
			Aura       aura;
			aura.spell = a_spell;
			aura.name = a_outcome.value("name", "");
			aura.tooltip = a_outcome.value("tooltip", json());
			aura.target = a_on;
			for (const auto& event : a_outcome.value("events",json::array())) {
				if (event.value("type", "") == "interrupt" && event.value("school_lockout_ms",0) > 0) aura.lockout = true;
			}
			aura.school = a_outcome.value("school", 0);
			aura.harmful = WowUi::Facts(a_spell).harmful;
			aura.start = Frame::Now();
			aura.threat = a_outcome.value("threat_multiplier", 1.0f);
			for (const auto& a : a_outcome.value("auras", json::array())) {
				const auto type = a.value("type", "");
				if (type == "heal") {
					aura.wowPerTick = a.value("wow_per_tick", -1.0f);
					aura.respite = a.value("respite", false);
				}
				aura.end = std::max(aura.end, a.value("duration_ms", 0) < 0 ? 1e12 : aura.start + a.value("duration_ms", 0) / 1000.0);
				aura.form = aura.form || type == "shapeshift";
				if (type == "shapeshift") aura.formOutcome = a_outcome;
				if (const auto kind = a.value("aura", 0u); kind == 44 || kind == 45 || kind == 151) {
					aura.trackingAura = kind;
					aura.trackingMask = a.value("mask", 0u);
				}
				aura.stealth = aura.stealth || type == "stealth";
				aura.endsWithForm = aura.endsWithForm || a.value("ends_on_form_loss", false);
				// A form's own Skyrim side is Forms'; these aura kinds have theirs here.
				if (type == "attack_speed") {
					aura.skyrim.push_back({ 0, -a.value("amount", 0.f) / 100.f, RE::ActorValue::kWeaponSpeedMult });
				} else if (type == "movement_speed") {   // Dash, Prowl
					aura.skyrim.push_back({ 0, a.value("amount", 0.0f), RE::ActorValue::kSpeedMult });
				} else if (type == "armor_percent") {   // Enrage: a share of her worn armor
					aura.skyrim.push_back({ 0, Forms::WornArmor() * a.value("amount", 0.0f) / 100.0f, RE::ActorValue::kDamageResist });
				} else if (type == "attack_power") {   // Demoralizing Roar: WoW's attack power off, as Skyrim's damage
					aura.skyrim.push_back({ 0, a.value("amount", 0.0f) / 1000.0f, RE::ActorValue::kAttackDamageMult });
				} else if (type == "hit_chance") {   // Insect Swarm: 2% fewer landed attacks -> 2% less native damage
					aura.skyrim.push_back({ 0, a.value("amount", 0.0f) / 100.0f, RE::ActorValue::kAttackDamageMult });
				} else if (type == "dispel_immunity" && a.value("dispel", "") == "Stealth") {
					aura.reveal = true;
					// Native sneaking uses detection rather than WoW aura categories.
					// Remove its skill advantage while marked; actual invisibility is dispelled below.
					aura.skyrim.push_back({0, -100.f, RE::ActorValue::kSneakingModifier});
				}
				if (type == "armor") aura.armorReduction += std::max(0.f, -a.value("amount",0.f));
				for (const auto& t : a.value("ticks", json::array())) {
					aura.ticks.emplace_back(t.value("t_ms", 0) / 1000.0, t.value("amount", 0));
					aura.kinds.push_back(type);
				}
				for (const auto& e : a.value("skyrim", json::array())) {
					aura.skyrim.push_back({ e.value("mgef", RE::FormID{ 0 }), e.value("magnitude", 0.0f) });
				}
				if (type == "root") {
					aura.root = true;
					aura.controlEnd = aura.start + a.value("duration_ms",0) / 1000.0;
					if (const auto& hb = a.value("heartbeat", json::object()); hb.contains("breaks_at_ms") && hb["breaks_at_ms"].is_number()) {
						aura.breakAt = aura.start + hb["breaks_at_ms"].get<double>() / 1000.0;
					}
					aura.victimLevel = a.value("damage_break", json::object()).value("victim_level", 0);
				} else if (type == "stun") {
					aura.sleep = true;
					aura.controlEnd = aura.start + a.value("duration_ms",0) / 1000.0;
					aura.wakes = a.value("breaks_on_damage", false);
					if (const auto& hb = a.value("heartbeat", json::object()); hb.contains("breaks_at_ms") && hb["breaks_at_ms"].is_number()) {
						aura.breakAt = aura.start + hb["breaks_at_ms"].get<double>() / 1000.0;
					}
					if (a.value("single_target", false)) {   // one at a time: the last one sleeping wakes
						for (auto& x : g_auras) {
							if (x.spell == a_spell && x.target != a_on) {
								x.end = aura.start;
							}
						}
					}
				} else if (type == "damage_shield") {
					aura.shield = a.value("amount", 0);
				} else if (type == "pushback_immunity") {
					aura.noPushback = true;
				} else if (type == "periodic_dispel") {
					aura.dispel = a.value("dispel", "");
					aura.dispelCount = a.value("count", 1);
					aura.dispelEvery = std::max(0.1, a.value("amplitude_ms", 2000) / 1000.0);
					aura.nextDispel = aura.start + aura.dispelEvery;
				}
			}
			// Ticks of all the spell's effects in time order; a new cast replaces the old one (same caster).
			std::vector<std::size_t> order(aura.ticks.size());
			std::iota(order.begin(), order.end(), 0);
			std::ranges::stable_sort(order, {}, [&](std::size_t i) { return aura.ticks[i].first; });
			Aura sorted = aura;
			for (std::size_t i = 0; i < order.size(); ++i) {
				sorted.ticks[i] = aura.ticks[order[i]], sorted.kinds[i] = aura.kinds[order[i]];
			}
			std::erase_if(g_auras, [&](const Aura& x) {
				const bool same = x.target == sorted.target &&
					(x.spell == a_spell || (!sorted.name.empty() && x.name == sorted.name) || (sorted.trackingAura && x.trackingAura) ||
						(sorted.reveal && x.reveal && sorted.armorReduction > 0 && x.armorReduction > 0)); // caster and Feral Faerie Fire share one debuff
				if (same) {
					SpellVisuals::Stop(x.state);
					if (x.spell != a_spell) {
						auto* actor = x.target ? x.target.get().get() : static_cast<RE::Actor*>(Player());
						if (actor) {
							Carriers::Unbuff(actor, x.spell);
							if (x.root) Carriers::Unroot(actor);
							if (x.sleep) Carriers::Wake(actor);
						}
						Lua(std::format("__WBAuraRemove({}, {})", onSelf ? std::string("\"player\"") : std::to_string(target->GetFormID()), x.spell));
					}
				}
				return same;
			});
			RE::Actor* who = target ? target : static_cast<RE::Actor*>(Player());
			const auto whoArg = onSelf ? std::string("\"player\"") : std::to_string(target->GetFormID());
			const double shown = sorted.end > 1e11 ? 0.0 : sorted.end - sorted.start;   // 0: until cancelled (a form)
			Lua(std::format("__WBAuraAdd({}, {}, {}, {:.3f}, {})", whoArg, a_spell, sorted.harmful ? "true" : "false", shown, TooltipLua(sorted.tooltip)));
			if (onSelf) {
				Log("CHAT_MSG_SPELL_PERIODIC_SELF_BUFFS", "AURAADDEDSELFHELPFUL", sorted.name);
			} else if (sorted.harmful) {
				Log("CHAT_MSG_SPELL_PERIODIC_CREATURE_DAMAGE", "AURAADDEDOTHERHARMFUL", Name(target), sorted.name);
			} else {
				Log("CHAT_MSG_SPELL_PERIODIC_FRIENDLYPLAYER_BUFFS", "AURAADDEDOTHERHELPFUL", Name(target), sorted.name);
			}
			const auto seconds = static_cast<float>(sorted.end - sorted.start);
			if (sorted.root && target) {
				Carriers::Root(target, static_cast<float>(sorted.controlEnd - sorted.start));
				sorted.rootAt = target->GetPosition();
			}
			if (sorted.sleep && target) {
				Carriers::Sleep(target, static_cast<float>(sorted.controlEnd - sorted.start));
			}
			if (!sorted.skyrim.empty()) {
				Carriers::Buff(who, a_spell, sorted.name, sorted.skyrim, seconds);
			}
			if (sorted.reveal) Reveal(who);
			sorted.state = SpellVisuals::Play(a_spell, SpellVisuals::Kit::State, sorted.target, true);
			if (const auto state = WowUi::Sounds(a_spell).state) {
				const auto at = who->GetPosition();
				WowUi::PlaySound(state, &at);
			}
			g_auras.push_back(std::move(sorted));
		}

		// Rebirth's target is dead, as it must be.
		bool Raises(const json& a_outcome)
		{
			return std::ranges::any_of(a_outcome.value("events", json::array()), [](const json& e) { return e.value("type", "") == "resurrect"; });
		}

		// A cast's auras: on its target, or (a party aura: Gift of the Wild) on her party around her.
		void AddAuras(const InFlight& a_cast)
		{
			const auto& out = a_cast.outcome;
			const auto  auras = out.value("auras", json::array());
			if (auras.empty()) {
				return;
			}
			std::vector<RE::ActorHandle> on{ a_cast.target };
			for (const auto& a : auras) {
				if (const auto area = a.value("area", json::object()); area.value("party", false) && !area.value("channel", false)) {
					on = Party(Player()->GetPosition(), area.value("radius_yd", 0.0f) * kUnitsPerYard);
					break;
				} else if (area.value("hostile", false)) {   // Demoralizing Roar: the enemies around her
					on = Hostiles(Player()->GetPosition(), area.value("radius_yd", 0.0f) * kUnitsPerYard, 64);
					break;
				}
			}
			for (const auto& h : on) {
				AddAura(a_cast.spell, out, h);
			}
			FormTracking(out);
		}

		// What a cast does where it lands: the outcome on the target, its impact, its auras, and the log lines.
		void Land(const InFlight& a_cast)
		{
			const auto&       out = a_cast.outcome;
			auto*             target = a_cast.target.get().get();
			if (a_cast.target && (!target || target->IsDead() != Raises(out))) {
				return;   // gone or dead while the missile flew (or, for Rebirth, alive again)
			}
			RE::Actor*        who = target ? target : static_cast<RE::Actor*>(Player());
			TalentEffects(out, target, false);   // charged buffs were consumed at completion, before missile travel
			if (const auto hot = out.value("consumed_target_hot", 0u); hot) {
				for (auto& aura : g_auras) {
					if (aura.spell == hot && aura.target == a_cast.target) {
						aura.end = Frame::Now();
						aura.next = aura.ticks.size();
					}
				}
			}
			const std::string spellName = out.value("name", "");
			const char*       school = kSchool[std::clamp(out.value("school", 0), 0, 6)];
			for (const auto& e : out.value("events", json::array())) {
				const auto type = e.value("type", "");
				const int  amount = e.value("amount", 0);
				const bool crit = e.value("crit", false);
				if (type == "damage") {
					ApplyHealth(who, "damage", amount, a_cast.spell);
					if (amount > 0) Forms::AttackThreat(out, who);
					if (e.value("school", out.value("school", 0)) == 0) {   // a physical blow (Claw, Maul) names no school
						Log("CHAT_MSG_SPELL_SELF_DAMAGE", crit ? "SPELLLOGCRITSELFOTHER" : "SPELLLOGSELFOTHER", spellName, Name(who), amount);
					} else {
						Log("CHAT_MSG_SPELL_SELF_DAMAGE", crit ? "SPELLLOGCRITSCHOOLSELFOTHER" : "SPELLLOGSCHOOLSELFOTHER", spellName, Name(who), amount, std::string(school));
					}
				} else if (type == "heal") {
					g_talentEvidence["heal"] = {{"spell", a_cast.spell}, {"amount", amount}, {"target", who->GetFormID()},
						{"health_before", who->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth)},
						{"consumed_target_hot", out.value("consumed_target_hot", 0u)}};
					const bool effective = amount > 0 && !FullHealth(who);
					ApplyHealth(who, "heal", amount, a_cast.spell);
					if (effective) HealingThreat(out.value("threat_multiplier", 1.0f));
					who->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kStamina, e.value("stamina", 0.0f));
					if (target) {
						Log("CHAT_MSG_SPELL_SELF_BUFF", crit ? "HEALEDCRITSELFOTHER" : "HEALEDSELFOTHER", spellName, Name(target), amount);
					} else {
						Log("CHAT_MSG_SPELL_SELF_BUFF", crit ? "HEALEDCRITSELFSELF" : "HEALEDSELFSELF", spellName, amount);
					}
				} else if (type == "miss") {
					Log("CHAT_MSG_SPELL_SELF_DAMAGE", "SPELLMISSSELFOTHER", spellName, Name(who));
					return;
				} else if (type == "resist") {
					Log("CHAT_MSG_SPELL_SELF_DAMAGE", "SPELLRESISTSELFOTHER", spellName, Name(who));
					return;
				} else if (type == "dodge" || type == "parry" || type == "block") {
					Log("CHAT_MSG_SPELL_SELF_DAMAGE", type == "dodge" ? "SPELLDODGEDSELFOTHER" : type == "parry" ? "SPELLPARRIEDSELFOTHER" : "SPELLBLOCKEDSELFOTHER",
						spellName, Name(who));
					Forms::Apply(out, nullptr);   // a refund, a broken prowl
					return;
				} else if (type == "dispel") {
					Dispel(who, e.value("dispel", ""), e.value("count", 1));
				} else if (type == "resurrect") {
					Resurrect(target, e.value("health", 0), e.value("mana", 0));
				} else if (type == "charge" && target) {
					const auto from = Player()->GetPosition();
					const auto to = target->GetPosition() - from;
					const float distance = to.Length();
					if (distance > kUnitsPerYard * 2.0f) {
						const auto at = target->GetPosition() - to * (kUnitsPerYard * 2.0f / distance);
						Player()->SetPosition(at, true);
					}
					g_talentEvidence["charge"] = {{"target",target->GetFormID()},
						{"from",{from.x,from.y,from.z}},
						{"moved",Player()->GetPosition().GetDistance(from)},
						{"landing_yards",Player()->GetPosition().GetDistance(target->GetPosition()) / kUnitsPerYard}};
				} else if (type == "interrupt" && target) {
					target->InterruptCast(false);
				}
			}
			for (const auto& hit : out.value("results", json::array())) {   // Swipe: each of the enemies it struck
				auto* struck = RE::TESForm::LookupByID<RE::Actor>(hit.value("id", RE::FormID{ 0 }));
				if (!struck || hit.value("type", "") != "damage") {
					continue;
				}
				const int amount = hit.value("amount", 0);
				ApplyHealth(struck, "damage", amount, a_cast.spell);
				if (amount > 0) Forms::AttackThreat(out, struck);
				Log("CHAT_MSG_SPELL_SELF_DAMAGE", hit.value("crit", false) ? "SPELLLOGCRITSELFOTHER" : "SPELLLOGSELFOTHER", spellName, Name(struck), amount);
			}
			Forms::Apply(out, target);
			if (!out.value("events", json::array()).empty() || !out.value("auras", json::array()).empty()) {
				const auto at = who->GetPosition();
				WowUi::PlaySound(WowUi::Sounds(a_cast.spell).impact, &at);
				SpellVisuals::Play(a_cast.spell, SpellVisuals::Kit::Impact, a_cast.target, false);
			}
			AddAuras(a_cast);
		}

		// She moves of her own accord - a movement key, auto-run, a jump or a fall - as the client's movement flags say;
		// turning on the spot does not count, nor does a blow nudging her (Skyrim's hits shift her a few units, and
		// that ended every cast: hits only push a cast back, Unit::InterruptOrDelaySpell).
		bool MovingHerself()
		{
			const auto* controls = RE::PlayerControls::GetSingleton();
			const bool  input = controls && (controls->data.moveInputVec.Length() > 0.01f || controls->data.autoMove);
			return input || Player()->IsInMidair();
		}

		// The cooldown a cast or channel starts as it goes off (Player::AddCooldown).
		void StartCooldown(std::uint32_t a_spell, const json& a_outcome)
		{
			if (const int cd = a_outcome.value("cooldown_ms", 0); cd > 0) {
				g_cooldowns[a_spell] = Frame::Now() + cd / 1000.0;
				Lua(std::format("__WBCooldown({}, {:.3f})", a_spell, cd / 1000.0));
			}
		}

		// A channelled area spell (Hurricane where it was placed; Tranquility around her): ticks every amplitude on
		// whatever is inside its circle while she stands still - enemies hurt, or her party healed.
		bool Channels(const json& a_outcome)
		{
			return std::ranges::any_of(a_outcome.value("auras", json::array()),
				[](const json& a) { return a.value("area", json::object()).value("channel", false); });
		}

		void StartChannel(std::uint32_t a_spell, const json& a_r, const RE::NiPoint3& a_at)
		{
			Channel c;
			c.spell = a_spell;
			c.name = a_r.value("name", "");
			c.school = a_r.value("school", 0);
			c.at = a_at;
			c.start = Frame::Now();
			const auto timing=ChannelTiming::From(a_r);
			if (!timing) { Lua("__WBCastFailed(\"SPELL_FAILED_ERROR\")"); return; }
			c.radius=timing->radius*kUnitsPerYard;
			c.end=c.start+timing->duration;
			c.amplitude=timing->period;
			c.heal=timing->heal;
			c.nextTick = c.start + c.amplitude;
			c.interruptFlags = a_r.value("channel_interrupt_flags", 0u);
			c.pushbackResist = a_r.value("pushback_resist_pct", 0.0f);
			const auto kit = WowUi::Sounds(a_spell);
			c.loop = WowUi::PlaySound(kit.channel);
			Avatar::Act(kit.channelAnim, true);
			c.visual = SpellVisuals::Play(a_spell, SpellVisuals::Kit::Channel, {}, true);
			c.area = SpellVisuals::Area(a_spell, c.at, c.radius / Avatar::kUnitsPerYard);
			Lua(std::format("__WBChannelStart({}, {})", a_spell, static_cast<int>((c.end - c.start) * 1000.0)));
			g_channel = std::move(c);
		}

		// The cast goes off: mana, the cast's sound, its cooldown; then it lands - at once, or when its missile arrives
		// (Spell.cpp: max(distance, 5 yards) / speed, from the caster to the target). A target that died or went away
		// during the cast fails it as the client's check at the end does (SPELL_FAILED_TARGETS_DEAD): nothing is paid.
		void Complete()
		{
			InFlight cast = std::move(*g_cast);
			g_cast.reset();
			const auto& out = cast.outcome;
            const auto fail=[&](const char* error) {
                Audio::Stop(cast.loop);
                Avatar::StopActing();
                SpellVisuals::Stop(cast.precast);
                Lua("__WBCastFailed("+LuaQuote(error)+")");
            };
            // The initial service answer describes the state at cast start. A
            // target can move/revive and inventory or mana can change before
            // release. Recheck before paying, starting cooldowns or applying it.
            if (Player()->IsDead()) { fail("SPELL_FAILED_CASTER_DEAD"); return; }
			if (cast.target) {
				auto* target = cast.target.get().get();
                const bool raises=Raises(out);
                if (!target || target->IsDisabled() || target->IsDeleted()) { fail("SPELL_FAILED_BAD_TARGETS"); return; }
                if (target->IsDead()!=raises) {
                    fail(raises ? "SPELL_FAILED_TARGET_NOT_DEAD" : "SPELL_FAILED_TARGETS_DEAD"); return;
                }
                const auto* base=target->GetActorBase();
                if (raises && (target->IsEssential() || target->IsProtected() ||
                    (base && (base->IsEssential() || base->IsProtected())))) { fail("SPELL_FAILED_BAD_TARGETS"); return; }
                const float range=SpellRange(cast.spell);
                if (range>0 && Player()->GetPosition().GetDistance(target->GetPosition())>range*kUnitsPerYard) {
                    fail("SPELL_FAILED_OUT_OF_RANGE"); return;
                }
                bool los=true;
                if (target!=Player() && !Player()->HasLineOfSight(target,los)) { fail("SPELL_FAILED_LINE_OF_SIGHT"); return; }
			}
            if (out.value("power","mana")=="mana" && out.value("cost",0.f)>Mana()) { fail("ERR_OUT_OF_MANA"); return; }
            const auto reagents=Gear::ReagentCounts();
            for (const auto& reagent : out.value("reagents",json::array())) {
                const auto entry=reagent.value("item",0u);
                const auto found=reagents.find(entry);
                if (found==reagents.end() || found->second<reagent.value("count",1)) { fail("SPELL_FAILED_REAGENTS"); return; }
            }
			Spend(out);
			for (const auto& id : out.value("consumed_auras", json::array())) {
				EndAura(id.get<std::uint32_t>());
			}
			Lua("__WBCastStop()");
			Audio::Stop(cast.loop);
			const auto kit = WowUi::Sounds(cast.spell);   // the visual kits: the cast's sound, her animation, its effects
			WowUi::PlaySound(kit.cast);
			Avatar::Act(kit.castAnim, false);
			SpellVisuals::Stop(cast.precast);
			// The cast kit's effects on her hands last until her cast animation lets the spell go - its $CSL, $CSR or
			// $CST event, 167 ms into SpellCastOmni - and their particles then run out: the client's Rejuvenation glow
			// is up by 0.25 s and gone by 0.5 (evidence/spellref/wow_rejuv_*).
			const auto castFx = SpellVisuals::Play(cast.spell, SpellVisuals::Kit::Cast, {}, false);
			for (const char* release : { "$CSL", "$CSR", "$CST" }) {
				if (const auto at = kit.castAnim >= 0 ? Avatar::EventAt(static_cast<std::uint32_t>(kit.castAnim), release) : std::nullopt) {
					Frame::After(*at, [castFx] { SpellVisuals::Stop(castFx); });
					break;
				}
			}
			StartCooldown(cast.spell, out);
			if (Channels(out)) {   // Tranquility: channelled around her from here
				StartChannel(cast.spell, out, Player()->GetPosition());
				return;
			}
			auto*       target = cast.target.get().get();
			const float speed = WowUi::Facts(cast.spell).speed;
			if (speed > 0.0f && target) {
				const float  yards = std::max(Player()->GetPosition().GetDistance(target->GetPosition()) / kUnitsPerYard, 5.0f);
				const double delay = std::floor(yards / speed * 1000.0f) / 1000.0;
				SpellVisuals::Missile(cast.spell, cast.target, static_cast<float>(delay));
				Frame::After(delay, [cast = std::move(cast), epoch = g_epoch] {
					if (epoch == g_epoch) {
						Land(cast);
					}
				});
				return;
			}
			Land(cast);
		}

		void TickAuras()
		{
			const double now = Frame::Now();
			bool         leftForm = false;
			for (auto it = g_auras.begin(); it != g_auras.end();) {
				auto&      a = *it;
				auto*      target = a.target.get().get();
				const bool onPlayer = !a.target;
				const bool gone = onPlayer ? ((a.trackingAura || a.form) && Player()->IsDead()) :
					(!target || target->IsDead() || target->IsDisabled());
				if (!gone && now < a.end) {
					if (a.reveal) Reveal(onPlayer ? static_cast<RE::Actor*>(Player()) : target);
					// Skyrim has no WoW school lockout. Prevent all native recasts
					// during Charge's four seconds, owned by its root aura.
					if (a.lockout && target) target->InterruptCast(false);
				}
				if (!gone && ControlTiming::Expired(now, a.controlEnd)) {
					if (a.root && target) Carriers::Unroot(target);
					if (a.sleep && target) Carriers::Wake(target);
					a.root = a.sleep = a.lockout = false;
				}
				while (!gone && a.next < a.ticks.size() && now >= a.start + a.ticks[a.next].first) {
					const int   amount = a.ticks[a.next].second;
					const auto& kind = a.kinds[a.next];
					++a.next;
					RE::Actor* who = target ? target : static_cast<RE::Actor*>(Player());
					if (kind == "heal") {
						if (FullHealth(who) && !a.respite) {
							continue;   // CMaNGOS skips a heal tick on a unit at full health (SpellAuras.cpp:4699)
						}
						ApplyHealth(who, "heal", amount, a.spell);
						HealingThreat(a.threat);
						if (a.respite) who->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kStamina, static_cast<float>(amount));
						if (target) {
							Log("CHAT_MSG_SPELL_PERIODIC_FRIENDLYPLAYER_BUFFS", "PERIODICAURAHEALSELFOTHER", Name(target), amount, a.name);
						} else {
							Log("CHAT_MSG_SPELL_PERIODIC_SELF_BUFFS", "PERIODICAURAHEALSELFSELF", amount, a.name);
						}
					} else {
						ApplyHealth(who, "damage", amount, a.spell);
						Log("CHAT_MSG_SPELL_PERIODIC_CREATURE_DAMAGE", "PERIODICAURADAMAGESELFOTHER", Name(who), amount,
							std::string(kSchool[std::clamp(a.school, 0, 6)]), a.name);
					}
				}
				if (a.root && target && !gone) {   // held where it was rooted; it may still turn and fight
					const auto p = target->GetPosition();
					if (std::hypot(p.x - a.rootAt.x, p.y - a.rootAt.y) > 1.0f) {
						target->SetPosition({ a.rootAt.x, a.rootAt.y, p.z }, true);
					}
				}
				if ((a.root || a.sleep) && a.breakAt >= 0 && now >= a.breakAt) {
					a.end = now;   // the heartbeat resist: the root or sleep breaks early
				}
				while (!gone && a.dispelEvery > 0 && now >= a.nextDispel && a.nextDispel <= a.end + 0.001) {   // Abolish Poison
					Dispel(target ? target : static_cast<RE::Actor*>(Player()), a.dispel, a.dispelCount);
					a.nextDispel += a.dispelEvery;
				}
				if (gone || now >= a.end) {
					const bool there = target && !target->IsDisabled();   // a unit taken out of the world has no effects to end
					if (a.root && there) {
						Carriers::Unroot(target);
					}
					if (a.sleep && there) {
						Carriers::Wake(target);
					}
					if (!a.skyrim.empty() && (onPlayer || there)) {
						Carriers::Unbuff(onPlayer ? static_cast<RE::Actor*>(Player()) : target, a.spell);
					}
					leftForm = leftForm || (onPlayer && a.form);   // her form's buff taken off: she leaves it
					if (onPlayer && a.stealth) {
						Forms::EndStealth(a.spell);
					}
					SpellVisuals::Stop(a.state);
					const auto whoArg = target ? std::to_string(target->GetFormID()) : std::string("\"player\"");
					Lua(std::format("__WBAuraRemove({}, {})", whoArg, a.spell));
					if (!gone) {
						if (target) {
							Log("CHAT_MSG_SPELL_AURA_GONE_OTHER", "AURAREMOVEDOTHER", a.name, Name(target));
						} else {
							Log("CHAT_MSG_SPELL_AURA_GONE_SELF", "AURAREMOVEDSELF", a.name);
						}
					}
					it = g_auras.erase(it);
				} else {
					++it;
				}
			}
			if (leftForm) {
				Forms::Leave();
				RemoveFormAuras(false);
			}
		}

	}

	namespace
	{
		// A blow lands (Skyrim's hit event, from anyone): Thorns hurts a melee attacker of the unit it is on, and a weapon's
		// hit wakes a sleeping beast. Spells (her carriers included) do neither here: hers wake it through DamageBreak.
		void Struck(RE::Actor* a_victim, RE::Actor* a_attacker, RE::FormID a_source, RE::FormID a_projectile)
		{
			const auto* source = a_source ? RE::TESForm::LookupByID(a_source) : nullptr;
			auto*       weapon = source ? source->As<RE::TESObjectWEAP>() : nullptr;
			const bool  blow = !a_projectile && (!source || (weapon && !weapon->IsBow() && !weapon->IsCrossbow()));
			if (!blow && !weapon) {
				return;
			}
			auto* player = Player();
			for (auto& a : g_auras) {
				auto* on = a.target ? a.target.get().get() : static_cast<RE::Actor*>(player);
				if (on != a_victim) {
					continue;
				}
				if (a.sleep && a.wakes) {
					a.end = Frame::Now();
				}
				if (a.shield > 0 && blow && a_attacker && a_attacker != player && !a_attacker->IsDead()) {
					ApplyHealth(a_attacker, "damage", a.shield, a.spell);
					const std::string school = kSchool[std::clamp(a.school, 0, 6)];
					if (on == player) {
						Log("CHAT_MSG_SPELL_DAMAGESHIELDS_ON_SELF", "DAMAGESHIELDSELFOTHER", a.shield, school, Name(a_attacker));
					} else {
						Log("CHAT_MSG_SPELL_DAMAGESHIELDS_ON_OTHERS", "DAMAGESHIELDOTHEROTHER", Name(on), a.shield, school, Name(a_attacker));
					}
				}
			}
		}

		void EndChannel(bool a_interrupted)
		{
			if (!g_channel) {
				return;
			}
			for (const auto id : g_channel->hit) {
				Lua(std::format("__WBAuraRemove({}, {})", id == Player()->GetFormID() ? std::string("\"player\"") : std::to_string(id), g_channel->spell));
			}
			Audio::Stop(g_channel->loop);
			Avatar::StopActing();
			SpellVisuals::Stop(g_channel->visual);
			SpellVisuals::Stop(g_channel->area);
			g_channel.reset();
			Lua("__WBChannelStop()");
			if (a_interrupted) {
				UiError("SPELL_FAILED_INTERRUPTED");
			}
		}

		// One tick: the units you could attack inside the circle, rolled by the service (a miss is silent).
		void ChannelTick()
		{
			auto&      c = *g_channel;
			auto*      player = Player();
			json       targets = json::array();
			if (c.heal) {   // Tranquility: her party in the circle, friendly and alive
				for (const auto& h : Party(c.at, c.radius)) {
					const auto  a = h.get();
					const auto* actor = h ? a.get() : static_cast<RE::Actor*>(player);
					if (actor) {
						targets.push_back({ { "id", actor->GetFormID() }, { "level", actor->GetLevel() }, { "friendly", true }, { "dead", false } });
					}
				}
			}
			for (auto& handle : RE::ProcessLists::GetSingleton()->highActorHandles) {
				auto  a = handle.get();
				auto* actor = a.get();
				if (c.heal || !actor || actor == player || actor->IsDead() || !actor->Is3DLoaded() ||
					!Bridge::Attackable(actor) || actor->IsPlayerTeammate()) {   // all in it she may strike but her followers
					continue;
				}
				const auto p = actor->GetPosition();
				if (std::hypot(p.x - c.at.x, p.y - c.at.y) <= c.radius && std::abs(p.z - c.at.z) <= c.radius) {
					targets.push_back({ { "id", actor->GetFormID() }, { "level", actor->GetLevel() }, { "is_player", false } });
				}
			}
			++c.tick;
			g_talentEvidence["channel"]={{"spell",c.spell},{"tick",c.tick},{"period",c.amplitude}};
			if (targets.empty()) {
				return;
			}
			Service::Request({ { "op", "tick_area" }, { "spell", c.spell }, { "tick", c.tick }, { "caster", CasterNow() }, { "targets", targets },
								 { "seed", std::uniform_int_distribution<std::uint32_t>()(g_rng) } },
				[spell = c.spell](const json& r) {
					if (!g_channel || g_channel->spell != spell || !r.value("ok", false)) {
						return;
					}
					const auto& c = *g_channel;
					for (const auto& hit : r.value("results", json::array())) {
						auto* actor = RE::TESForm::LookupByID<RE::Actor>(hit.value("id", RE::FormID{ 0 }));
						if (actor && hit.value("type", "") == "heal") {
							const int  amount = hit.value("amount", 0);
							const bool her = actor == Player();
							actor->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kStamina, hit.value("stamina", 0.0f));
							if (!FullHealth(actor)) {   // CMaNGOS skips a heal tick on a unit at full health
								ApplyHealth(actor, "heal", amount, spell);
								HealingThreat(r.value("threat_multiplier", 1.0f));
								if (her) {
									Log("CHAT_MSG_SPELL_PERIODIC_SELF_BUFFS", "PERIODICAURAHEALSELFSELF", amount, c.name);
								} else {
									Log("CHAT_MSG_SPELL_PERIODIC_FRIENDLYPLAYER_BUFFS", "PERIODICAURAHEALSELFOTHER", Name(actor), amount, c.name);
								}
							}
							if (g_channel->hit.insert(actor->GetFormID()).second) {
								Lua(std::format("__WBAuraAdd({}, {}, false, {:.3f})", her ? std::string("\"player\"") : std::to_string(actor->GetFormID()),
									spell, std::max(0.0, c.end - Frame::Now())));
							}
							continue;
						}
						if (!actor || hit.value("type", "") != "damage") {
							continue;
						}
						const int amount = hit.value("amount", 0);
						ApplyHealth(actor, "damage", amount, spell);
						Log("CHAT_MSG_SPELL_PERIODIC_CREATURE_DAMAGE", "PERIODICAURADAMAGESELFOTHER", Name(actor), amount,
							std::string(kSchool[std::clamp(c.school, 0, 6)]), c.name);
						Lua(std::format("__WBAuraAdd({}, {}, true, {:.3f})", actor->GetFormID(), spell, std::max(0.0, c.end - Frame::Now())));
						const auto at = actor->GetPosition();
						WowUi::PlaySound(WowUi::Sounds(spell).impact, &at);
						SpellVisuals::Play(spell, SpellVisuals::Kit::Impact, actor->CreateRefHandle(), false);
						g_channel->hit.insert(actor->GetFormID());
					}
				});
		}
	}

	// You were hit (Unit::InterruptOrDelaySpell): a cast with pushback is set back (its remaining time never more than
	// the whole cast time), or cancelled by damage; a channel loses time, or ends.
	void OnHit(bool a_byYourself)
	{
		// Barkskin: blows do not delay her casting (aura 149, all schools at 100%).
		const bool steady = std::ranges::any_of(g_auras, [](const Aura& a) { return !a.target && a.noPushback; });
		const float resist = g_cast ? g_cast->outcome.value("pushback_resist_pct", 0.0f) : g_channel ? g_channel->pushbackResist : 0.0f;
		const bool avoided = std::uniform_real_distribution<float>(0, 100)(g_rng) < resist;
		if ((steady || avoided) && (g_cast || g_channel)) {
			g_talentEvidence["pushbacks_resisted"] = g_talentEvidence.value("pushbacks_resisted", 0) + 1;
		}
		if (g_cast && g_cast->castTime > 0) {
			const auto flags = g_cast->outcome.value("interrupt_flags", 0u);
			if (flags & 0x10) {
				Stop();
			} else if ((flags & 0x2) && !steady && !avoided) {
				const double now = Frame::Now();
				const double remaining = g_cast->endAt - now;
				const double longer = std::min(remaining + NextDelayMs(g_cast->delays) / 1000.0, g_cast->castTime);
				const int    ms = static_cast<int>(std::lround((longer - remaining) * 1000.0));
				if (ms > 0) {
					g_cast->endAt = now + longer;
					Lua(std::format("__WBCastDelayed({})", ms));
				}
			}
		}
		if (g_channel && !a_byYourself) {
			if (g_channel->interruptFlags & 0x4000) {
				if (steady || avoided) {
					return;
				}
				const double now = Frame::Now();
				g_channel->end = std::max(now, g_channel->end - NextDelayMs(g_channel->delays) / 1000.0);
				Lua(std::format("__WBChannelUpdate({})", static_cast<int>(std::lround((g_channel->end - now) * 1000.0))));
			} else if (g_channel->interruptFlags & 0x2) {
				EndChannel(true);
			}
		}
	}

	bool Placing() { return g_placing != 0; }

	void CancelPlacing()
	{
		if (g_placing) {
			g_placing = 0;
			Lua("__WBTargeting(nil)");
		}
	}

	// The place for the ground spell being placed: in range and in sight of you, as the service rules.
	void Place(const RE::NiPoint3& a_point)
	{
		if (!g_placing || g_pending) {
			return;
		}
		auto*      player = Player();
		const auto eye = player->GetPosition() + RE::NiPoint3{ 0, 0, 100.0f };
		const auto aim = a_point + RE::NiPoint3{ 0, 0, 20.0f };
		const auto hit = Picker::Cast(eye, aim);
		const bool los = !hit.hit || hit.distance >= eye.GetDistance(aim) - 40.0f;
		const auto spell = g_placing;
		logger::info("cast: {} placed at {:.0f} {:.0f} {:.0f}", spell, a_point.x, a_point.y, a_point.z);
		const json req = { { "op", "resolve" }, { "spell", spell }, { "caster", CasterNow() },
			{ "target", { { "kind", "ground" }, { "distance_yd", player->GetPosition().GetDistance(a_point) / kUnitsPerYard }, { "los", los } } },
			{ "seed", std::uniform_int_distribution<std::uint32_t>()(g_rng) } };
		g_pending = true;
		Service::Request(req, [spell, a_point, epoch = g_epoch, request = ++g_request](const json& r) {
			if (epoch != g_epoch || request != g_request) {
				return;
			}
			g_pending = false;
			if (!r.value("ok", false)) {
				const auto err = r.value("error", "SPELL_FAILED_ERROR");
				UiError(err == "SERVICE_OFFLINE" ? "SPELL_FAILED_ERROR" : err.c_str());   // still placing: try another spot
				return;
			}
			if (r.value("power", "mana") == "mana" && r.value("cost", 0) > Mana()) {
				CancelPlacing();
				Lua("__WBCastFailed(\"ERR_OUT_OF_MANA\")");
				return;
			}
			CancelPlacing();
			Spend(r);   // a channel is paid for when it starts, and its cooldown starts then too
			Forms::BreakShadowmeld();
			StartCooldown(spell, r);
			if (const double gcd = r.value("gcd_ms", 0) / 1000.0; gcd > 0) {
				g_gcdUntil = Frame::Now() + gcd;
				Lua(std::format("__WBGcd({:.3f})", gcd));
			}
			StartChannel(spell, r, a_point);
		});
	}

	void Init()
	{
		// Hits on you from Skyrim (blows, arrows, spells - not damage over time) push your casting back.
		static struct HitSink : RE::BSTEventSink<RE::TESHitEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				// Hit events come on another thread: what they change (her cast, the auras) is changed on the main one.
				auto* victim = a_event && a_event->target ? a_event->target->As<RE::Actor>() : nullptr;
				if (!Integration::Active() || !victim) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto*      attacker = a_event->cause ? a_event->cause->As<RE::Actor>() : nullptr;
				const auto victimHandle = victim->CreateRefHandle();
				const auto attackerHandle = attacker ? attacker->CreateRefHandle() : RE::ActorHandle{};
				const bool blocked = a_event->flags.any(RE::TESHitEvent::Flag::kHitBlocked);
				SKSE::GetTaskInterface()->AddTask([victimHandle, attackerHandle, source = a_event->source, projectile = a_event->projectile, blocked] {
					auto* player = RE::PlayerCharacter::GetSingleton();
					auto  victim = victimHandle.get();
					auto  attacker = attackerHandle.get();
					if (!Integration::Active() || !victim) {
						return;
					}
					if (victim.get() == player) {
						OnHit(attacker.get() == player);
						IncomingHit(attacker.get(), source, projectile);
					}
					Struck(victim.get(), attacker.get(), source, projectile);
					WorldLog::Struck(victim.get(), attacker.get(), source, blocked);
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		} hits;
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESHitEvent>(&hits);
		// Native actions also interrupt her meld. Runtime carrier spells (the FF forms) apply her effects;
		// the stealth carrier must not immediately break its own Shadowmeld.
		static struct Actions : RE::BSTEventSink<SKSE::ActionEvent>, RE::BSTEventSink<RE::TESActivateEvent>,
		                       RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ActionEvent* a_event, RE::BSTEventSource<SKSE::ActionEvent>*) override
			{
				if (a_event && a_event->actor == Player()) {
					using T = SKSE::ActionEvent::Type;
					const bool spell = a_event->type == T::kSpellCast || a_event->type == T::kSpellFire;
					if ((!spell || !a_event->sourceForm || a_event->sourceForm->GetFormID() < 0xFF000000) &&
						(spell || a_event->type == T::kWeaponSwing || a_event->type == T::kBowDraw ||
						 a_event->type == T::kBowRelease || a_event->type == T::kVoiceCast || a_event->type == T::kVoiceFire ||
						 a_event->type == T::kBeginDraw || a_event->type == T::kBeginSheathe)) {
						SKSE::GetTaskInterface()->AddTask([] { Forms::BreakShadowmeld(); });
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				if (a_event && a_event->actionRef.get() == Player()) {
					SKSE::GetTaskInterface()->AddTask([] { Forms::BreakShadowmeld(); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				// Consuming a potion/food is an item action even when Skyrim raises no spell/activation event. The event can
				// come on a loader thread (a cell change): the form is looked up on the main one.
				if (a_event && a_event->oldContainer == 0x14) {
					SKSE::GetTaskInterface()->AddTask([form = a_event->baseObj] {
						if (RE::TESForm::LookupByID<RE::AlchemyItem>(form)) {
							Forms::BreakShadowmeld();
						}
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		} actions;
		SKSE::GetActionEventSource()->AddEventSink(&actions);
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESActivateEvent>(&actions);
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESContainerChangedEvent>(&actions);
		Carriers::Create();
		Service::Start();
	}

	void Use(std::uint32_t a_spell)
	{
		if (Mounts::Use(a_spell)) return;
		Mounts::Dismount();
		// Repeated presses keep one follow-up; another command replaces it.
		if (g_formFollowup && g_formFollowup->spell == a_spell) return;
		g_formFollowup.reset();
		const bool shadowmeld = a_spell == 20580 && Character::Get().looks.race == 4;
		if (a_spell == 20582 || a_spell == 20583 || a_spell == 20585 || (!shadowmeld && !Progression::Knows(a_spell))) {
			UiError("SPELL_FAILED_NOT_KNOWN");
			return;
		}
		const double now = Frame::Now();
		if (g_cast || g_pending || SkyrimSpells::Casting()) {   // a Skyrim spell of hers is casting too
			UiError("SPELL_FAILED_SPELL_IN_PROGRESS");
			return;
		}
		if (now < g_gcdUntil || now < g_cooldowns[a_spell]) {
			UiError("SPELL_FAILED_NOT_READY");
			return;
		}
		const auto& form = Forms::Current();
		const std::uint32_t mask = form == "cat" ? 1u : form == "travel" ? 4u : form == "aquatic" ? 8u :
			form == "bear" ? 16u : form == "dire_bear" ? 128u : form == "moonkin" ? (1u << 30) : 0u;
		const auto facts = WowUi::Facts(a_spell);
		if (!facts.known) {
			return;
		}
		const auto formError = WowUi::Eval(std::format("__WB.spellFormError(__WB.spellInfo({}), {})", a_spell, mask));
		std::uint32_t autoForm = 0;
		if (formError != "nil" && !formError.empty()) {
			const auto shift = WowUi::Eval(std::format("__WB.autoFormSpell(__WB.spellInfo({}), {}) or 0", a_spell, mask));
			autoForm = static_cast<std::uint32_t>(std::strtoul(shift.c_str(), nullptr, 10));
			if (!autoForm || !Progression::Knows(autoForm)) {
				UiError(formError.c_str());
				return;
			}
		}
		EndChannel(true);   // another spell ends a channel
		if (facts.ground) {
			CancelPlacing();
			g_placing = a_spell;   // the next click on the ground places it
			Lua(std::format("__WBTargeting({})", a_spell));
			return;
		}
		CancelPlacing();
		auto*      player = Player();
		RE::Actor* target = Bridge::Target();   // a dead one is the service's to refuse (SPELL_FAILED_TARGETS_DEAD)
		if (facts.self) {                       // the client sends no target for these: whoever is selected is not asked
			target = nullptr;
		} else if (target == player) {          // yourself targeted (F1)
			if (facts.harmful) {
				UiError("SPELL_FAILED_BAD_TARGETS");
				return;
			}
			target = nullptr;
		}
		json t;
		if (facts.harmful) {
			if (!target) {
				UiError("ERR_GENERIC_NO_TARGET");
				return;
			}
		} else if (facts.resurrect) {
            if (!target) { UiError("ERR_GENERIC_NO_TARGET"); return; }
            const auto* base=target->GetActorBase();
            if (target->IsDead() && (target->IsEssential() || target->IsProtected() ||
                (base && (base->IsEssential() || base->IsProtected())))) {
                UiError("SPELL_FAILED_BAD_TARGETS"); return;
            }
            // Preserve the selected corpse, including a hostile one. The service
            // checks its actual life state and allows hostile corpses without
            // substituting the living caster or paying for a rejected cast.
		} else if (target && !Bridge::Healable(target)) {
			target = nullptr;   // a helpful spell with no friendly target is cast on yourself
		}
		if (target) {
			bool los = true;
			// In front: the front half of where the character faces (WorldObject::HasInArc, pi).
			const auto  to = target->GetPosition() - player->GetPosition();
			const float h = player->GetHeading(false);
			const bool  inFront = std::sin(h) * to.x + std::cos(h) * to.y >= 0.0f;
			t = { { "kind", "unit" }, { "level", target->GetLevel() }, { "is_player", false }, { "in_front", inFront },
				  { "distance_yd", player->GetPosition().GetDistance(target->GetPosition()) / kUnitsPerYard },
				  { "los", player->HasLineOfSight(target, los) }, { "alive", !target->IsDead() }, { "reaction", Reaction(target) },
				  { "attackable", Bridge::Attackable(target) }, { "healable", Bridge::Healable(target) },
				  { "creature_type", Bridge::CreatureTypeName(target) }, { "dead", target->IsDead() },
				  { "friendly", Bridge::Healable(target) && !facts.harmful } };
			// Behind it (Shred, Ravage): she stands in the back half of where it faces.
			const float th = target->GetHeading(false);
			t["behind"] = std::sin(th) * -to.x + std::cos(th) * -to.y < 0.0f;
		} else {
			t = { { "kind", "self" } };
		}
		if (autoForm) {
			// Do not shift for a missing/bad target. Range, resources and combat rules
			// are checked again by the ordinary ability cast after the shift settles.
			Use(autoForm);
			if (g_pending) g_formFollowup = FormFollowup{ a_spell, autoForm,
				target ? target->CreateRefHandle() : RE::ActorHandle{}, !facts.self, now + 5.0 };
			return;
		}
		if (a_spell == 18562) {
			t["hots"] = json::array();
			for (const auto& aura : g_auras) {
				const bool on = target ? aura.target && aura.target.get().get() == target : !aura.target;
				if (on && aura.wowPerTick >= 0 && aura.end > now) {
					t["hots"].push_back({ { "spell", aura.spell }, { "per_tick", aura.wowPerTick }, { "remaining_ms", (aura.end - now) * 1000.0 } });
				}
			}
		}
		const auto* cell = player->GetParentCell();
		AugmentTarget(t, target);
		json req = { { "op", "resolve" }, { "spell", a_spell }, { "caster", CasterNow() }, { "target", t },
			{ "ctx", { { "indoors", cell && cell->IsInteriorCell() } } }, { "seed", std::uniform_int_distribution<std::uint32_t>()(g_rng) } };
		req["caster"]["combo_points"] = Forms::ComboPoints(target); // points on this captured target
		if (facts.name == "Swipe") {   // Swipe (any rank): up to three enemies in front of her, within reach
			auto targets = json::array();
			for (const auto& h : Hostiles(player->GetPosition(), 5.0f * kUnitsPerYard, 3, true)) {
				if (auto a = h.get()) {
					json victim = { { "id", a->GetFormID() }, { "level", a->GetLevel() }, { "is_player", false } };
					AugmentTarget(victim, a.get());
					targets.push_back(std::move(victim));
				}
			}
			req["targets"] = targets;
		}
		g_pending = true;
		const RE::ActorHandle handle = target ? target->CreateRefHandle() : RE::ActorHandle{};
		RequestOutcome(std::move(req), [a_spell, handle, epoch = g_epoch, request = ++g_request](const json& r) {
			if (epoch != g_epoch || request != g_request) {
				return;
			}
			g_pending = false;
			if (!r.value("ok", false)) {
				const auto err = r.value("error", "SPELL_FAILED_ERROR");
				Lua(std::format("__WBCastFailed({})", LuaQuote(err == "SERVICE_OFFLINE" ? "SPELL_FAILED_ERROR" : err)));
				if (err == "SERVICE_OFFLINE") {
					logger::warn("cast: the gameplay service is not running (python -m service.server)");
				}
				return;
			}
			if (r.value("power", "mana") == "mana" && r.value("cost", 0) > Mana()) {
				Lua("__WBCastFailed(\"ERR_OUT_OF_MANA\")");
				return;
			}
			// The state may have changed while the service answered this instant spell.
			if (a_spell == 20580 && (MovingHerself() || Player()->IsMoving() || Player()->IsDead())) {
				UiError(Player()->IsDead() ? "SPELL_FAILED_CASTER_DEAD" :
					"SPELL_FAILED_MOVING");
				return;
			}
			if (a_spell != 20580) {
				Forms::BreakShadowmeld();
			}
			const double gcd = r.value("gcd_ms", 0) / 1000.0;
			if (gcd > 0) {
				g_gcdUntil = Frame::Now() + gcd;
				Lua(std::format("__WBGcd({:.3f})", gcd));
			}
			InFlight c{ a_spell, r, handle, 0 };
			const int castMs = r.value("cast_ms", 0);
			g_cast = std::move(c);
			if (castMs > 0) {
				g_cast->endAt = Frame::Now() + castMs / 1000.0;
				g_cast->castTime = castMs / 1000.0;
				const auto kit = WowUi::Sounds(a_spell);
				g_cast->loop = WowUi::PlaySound(kit.precast);
				Avatar::Act(kit.precastAnim, true);
				g_cast->precast = SpellVisuals::Play(a_spell, SpellVisuals::Kit::Precast, {}, true);
				Lua(std::format("__WBCastStart({}, {})", a_spell, castMs));
			} else {
				Complete();
			}
		});
	}

	void Stop()
	{
		++g_request;
		g_pending = false;
		g_formFollowup.reset();
		SkyrimSpells::Stop();
		EndChannel(false);
		if (g_cast) {
			Audio::Stop(g_cast->loop);
			Avatar::StopActing();
			SpellVisuals::Stop(g_cast->precast);
			g_cast.reset();
			Lua("__WBCastInterrupted()");
		}
	}

	void Tick()
	{
		Service::Pump();
		if (g_formFollowup && !g_pending && !g_cast) {
			const auto queued = *g_formFollowup;
			const auto expected = queued.shift == 768 ? "cat" : queued.shift == 9634 ? "dire_bear" : "bear";
			if (Frame::Now() > queued.expires || Forms::Current() != expected || Player()->IsDead() ||
				(queued.targeted && Bridge::Target() != queued.target.get().get())) {
				g_formFollowup.reset();
			} else if (Frame::Now() >= g_gcdUntil) {
				g_formFollowup.reset();
				Use(queued.spell);
			}
		}
		// Placing a ground spell: its area under the pointer, green within its range and red beyond, as the client's.
		if (g_placing && Input::Active()) {
			const auto [x, y] = Input::Cursor();
			const auto  hit = Picker::PickScreen(x, y, 8000.0f, true);
			const auto  facts = WowUi::Facts(g_placing);
			const bool  inRange = Player()->GetPosition().GetDistance(hit.pos) / kUnitsPerYard <= SpellRange(g_placing);
			Avatar::SetArea(hit.pos, hit.hit ? facts.radius * kUnitsPerYard : 0.0f, inRange);
		} else {
			Avatar::SetArea({}, 0.0f, true);
		}
		if (g_cast) {
			if (MovingHerself() && (g_cast->outcome.value("interrupt_flags", 0u) & 0x1)) {   // moving interrupts it
				Audio::Stop(g_cast->loop);
				Avatar::StopActing();
				SpellVisuals::Stop(g_cast->precast);
				g_cast.reset();
				Lua("__WBCastInterrupted()");
			} else if (Frame::Now() >= g_cast->endAt) {
				Complete();
			}
		}
		if (g_channel) {
			if (MovingHerself() && (g_channel->interruptFlags & 0x8)) {   // moving ends it
				EndChannel(true);
			} else {
				while (g_channel && Frame::Now() >= g_channel->nextTick && g_channel->nextTick <= g_channel->end + 0.001) {
					g_channel->nextTick += g_channel->amplitude;
					ChannelTick();
				}
				if (g_channel && Frame::Now() >= g_channel->end) {
					EndChannel(false);
				}
			}
		}
		TickAuras();
		// Her spells' magicka (service op skyrim_costs), again as skills, talents or active modifiers change.
		if (static double next = 0; Frame::Now() >= next) {
			next = Frame::Now() + 1.0;
			static json told;
			auto caster = CasterNow();
			json signature = { { "skills", caster["skyrim"] }, { "level", caster["level"] }, { "talents", caster["talents"] },
				{ "items", caster["items"] }, { "indoors", caster["indoors"] },
				{ "perks", caster["perks"] }, { "class", caster["class"] },
				{ "auras", caster["auras"] }, { "equipment_modifiers", caster["equipment_modifiers"] }, { "form", caster["form"] }, { "epoch", g_epoch } };
			if (signature != told) {
				told = signature;
				Service::Request({ { "op", "skyrim_costs" }, { "caster", caster } }, [epoch = g_epoch, caster](const json& r) {
					auto* ui = RE::UI::GetSingleton();
					if (!WowUi::Ready() || !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) { told = json(); return; }
					const auto current = CasterNow();
					if (epoch != g_epoch || !r.value("ok", false) || current["talents"] != caster["talents"] ||
						current["level"] != caster["level"] || current["perks"] != caster["perks"] ||
						current["auras"] != caster["auras"] || current["form"] != caster["form"] ||
						current["items"] != caster["items"] || current["skyrim"] != caster["skyrim"] ||
						current["equipment_modifiers"] != caster["equipment_modifiers"] || current["indoors"] != caster["indoors"]) {
						told = json();
						return;
					}
					std::string lua = "__WB.skyrimCosts = {";
					for (const auto& [spell, cost] : r.value("costs", json::object()).items()) {
						lua += std::format(" [{}] = {},", spell, cost.get<int>());
					}
					lua += " } __WBSpellTooltips({";
					g_spellRanges.clear();
					for (const auto& [spell, values] : r.value("spell_tooltips", json::object()).items()) {
						g_spellRanges[static_cast<std::uint32_t>(std::stoul(spell))] = values.value("range_max", 0.0f);
						lua += std::format("[{}]={},", spell, TooltipLua(values));
					}
					Lua(lua + "}) __WBFire('ACTIONBAR_UPDATE_USABLE')");
				});
			}
		}
	}

	void Reset()
	{
		++g_epoch;
		g_incomingPending.clear();
		g_spellRanges.clear();
		g_manaSpentAt = -100;
		g_talentEvidence = json::object();
		g_pending = false;
		g_formFollowup.reset();
		if (g_cast) {
			Audio::Stop(g_cast->loop);
		}
		if (g_channel) {
			Audio::Stop(g_channel->loop);
		}
		g_cast.reset();
		g_channel.reset();
		SpellVisuals::Clear();
		g_placing = 0;
		g_auras.clear();
		g_gcdUntil = 0;
		g_cooldowns.clear();
		Lua("__WBResetCombat()");
		if (g_loaded) {
			Restore(*g_loaded);
			g_loaded.reset();
		}
	}

    void RefreshCooldownsForTesting()
    {
        g_gcdUntil=0; g_cooldowns.clear();
        Lua("__WBRefreshCooldowns()");
    }
	void ResetForTesting()
	{
		Stop();
		for (const auto& a : g_auras) {
			auto target = a.target.get();
			auto* actor = a.target ? target.get() : static_cast<RE::Actor*>(Player());
			if (actor) {
				Carriers::Unbuff(actor, a.spell);
				if (a.root) { Carriers::Unroot(actor); }
				if (a.sleep) { Carriers::Wake(actor); }
			}
		}
		Forms::Leave();
		g_loaded.reset();
		Reset();
		Forms::Reset();
	}

	std::function<bool()> PreserveForTesting()
	{
		// A runtime audit needs more than a logout record: retain NPC auras,
		// explicit actor-value carriers, root/sleep/dispel state and exact ticks.
		return [auras = g_auras, cooldowns = g_cooldowns, at = Frame::Now(), gcd = g_gcdUntil, spent = g_manaSpentAt]() {
			const double now = Frame::Now(), shift = now - at;
			bool restored = true;
			for (auto a : auras) {
				auto target = a.target.get();
				auto* who = a.target ? target.get() : static_cast<RE::Actor*>(Player());
				if (a.end <= at) continue;
				if (!who || who->IsDead() || who->IsDisabled()) { restored = false; continue; }
				a.start += shift; a.end += shift;
				if (a.breakAt >= 0) a.breakAt += shift;
				a.nextDispel += shift;
				const float remaining = static_cast<float>(a.end - now);
				if (!a.skyrim.empty()) Carriers::Buff(who, a.spell, a.name, a.skyrim, remaining);
				if (a.root) Carriers::Root(who, remaining);
				if (a.sleep) Carriers::Sleep(who, remaining);
				const auto whoArg = a.target ? std::to_string(who->GetFormID()) : std::string("\"player\"");
				Lua(std::format("__WBAuraAdd({}, {}, {}, {:.3f}, {})", whoArg, a.spell, a.harmful ? "true" : "false",
					a.stealth || a.end > 1e11 ? 0.0 : remaining, TooltipLua(a.tooltip)));
				a.state = SpellVisuals::Play(a.spell, SpellVisuals::Kit::State, a.target, true);
				g_auras.push_back(std::move(a));
			}
			for (const auto& [spell, until] : cooldowns) if (until > at) {
				g_cooldowns[spell] = until + shift;
				Lua(std::format("__WBCooldown({}, {:.3f})", spell, until - at));
			}
			g_gcdUntil = std::max(now, gcd + shift);
			g_manaSpentAt = spent + shift;
			if (g_gcdUntil > now) Lua(std::format("__WBGcd({:.3f})", g_gcdUntil - now));
			return restored;
		};
	}

	void FormForTesting(std::uint32_t a_spell, const json& a_outcome)
	{
		++g_epoch;
		g_incomingPending.clear();
		g_pending = false;
		Stop();
		CancelPlacing();
		RemoveFormAuras(true);
		Forms::EndProwl();
		if (!a_spell) { Forms::Leave(); return; }
		Forms::Apply(a_outcome, nullptr);
		AddAura(a_spell, a_outcome, {});
		FormTracking(a_outcome);
	}

	json Snapshot()
	{
		const double now = Frame::Now();
		json         j = { { "saved_at", WallNow() }, { "cooldowns", json::object() }, { "auras", json::array() } };
		for (const auto& [spell, until] : g_cooldowns) {
			if (until > now) {
				j["cooldowns"][std::to_string(spell)] = until - now;
			}
		}
		for (const auto& a : g_auras) {
			if (a.target || a.end <= now) {
				continue;
			}
			json ticks = json::array();   // seconds from now, amount, kind
			for (std::size_t i = a.next; i < a.ticks.size(); ++i) {
				ticks.push_back({ a.start + a.ticks[i].first - now, a.ticks[i].second, a.kinds[i] });
			}
			const auto skyrim = AuraPersistence::SaveChanges(a.skyrim);
			j["auras"].push_back({ { "spell", a.spell }, { "name", a.name }, { "harmful", a.harmful }, { "school", a.school },
				{ "tooltip", a.tooltip },
				{ "respite", a.respite },
				{ "threat_multiplier", a.threat },
				{ "remaining", a.end - now }, { "ticks", ticks }, { "skyrim", skyrim }, { "shield", a.shield }, { "steady", a.noPushback }, { "wow_per_tick", a.wowPerTick },
				{ "tracking_aura", a.trackingAura }, { "tracking_mask", a.trackingMask },
				{ "ends_with_form", a.endsWithForm }, { "form_outcome", a.formOutcome } });
		}
		return j;
	}

	void Restore(const json& a_saved)
	{
        if (!CastPersistence::Valid(a_saved)) {
            logger::warn("cast: invalid saved state; auras and cooldowns left unchanged");
            return;
        }
		const double now = Frame::Now();
		const double away = std::max(0.0, WallNow() - a_saved.value("saved_at", WallNow()));
		if (a_saved.contains("mana")) {   // saved while her magicka mirrored WoW mana, its base set to WoW's 5284
			if (const auto* race = Player()->GetRace()) {
				Player()->AsActorValueOwner()->SetBaseActorValue(RE::ActorValue::kMagicka, race->data.startingMagicka);
			}
		}
		for (const auto& [spell, left] : a_saved.value("cooldowns", json::object()).items()) {
			const double rest = left.get<double>() - away;   // a cooldown runs on while you are away
			if (rest > 0) {
				const auto id = static_cast<std::uint32_t>(std::stoul(spell));
				g_cooldowns[id] = now + rest;
				Lua(std::format("__WBCooldown({}, {:.3f})", id, rest));
			}
		}
		for (const auto& s : a_saved.value("auras", json::array())) {
			Aura a;
			a.spell = s.value("spell", 0u);
			a.stealth = AuraPersistence::IsStealth(a.spell);
			a.trackingAura = s.value("tracking_aura", 0u);
			a.trackingMask = s.value("tracking_mask", 0u);
			a.endsWithForm = s.value("ends_with_form", false);
			a.formOutcome = s.value("form_outcome", json());
			a.form = a.formOutcome.is_object();
			a.name = s.value("name", "");
			a.tooltip = s.value("tooltip", json());
			a.harmful = s.value("harmful", false);
			a.school = s.value("school", 0);
			const double remaining = s.value("remaining", 0.0) - (a.harmful ? away : 0.0);   // a helpful one waits for you
			if (remaining <= 0) {
				continue;
			}
			a.start = now;
			a.end = now + remaining;
			for (const auto& t : s.value("ticks", json::array())) {
				if (t[0].get<double>() <= remaining) {
					a.ticks.emplace_back(t[0].get<double>(), t[1].get<int>());
					a.kinds.push_back(t[2].get<std::string>());
				}
			}
			// Its Skyrim side again: the runtime spells that carried it are not in the save.
			a.skyrim = AuraPersistence::LoadChanges<Carriers::Change>(s.value("skyrim", json::array()), static_cast<std::int32_t>(RE::ActorValue::kTotal));
			a.shield = s.value("shield", 0);
			a.wowPerTick = s.value("wow_per_tick", -1.0f);
			a.respite = s.value("respite", false);
			a.threat = s.value("threat_multiplier", 1.0f);
			a.noPushback = s.value("steady", false);
			if (!a.skyrim.empty()) {
				Carriers::Buff(Player(), a.spell, a.name, a.skyrim, static_cast<float>(remaining));
			}
			Lua(std::format("__WBAuraAdd(\"player\", {}, {}, {:.3f}, {})", a.spell, a.harmful ? "true" : "false", a.stealth || a.end > 1e11 ? 0.0 : remaining, TooltipLua(a.tooltip)));
			a.state = SpellVisuals::Play(a.spell, SpellVisuals::Kit::State, {}, true);
			g_auras.push_back(std::move(a));
		}
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		const auto text = Snapshot().dump();
		const auto size = static_cast<std::uint32_t>(text.size());
		if (!a_intfc->OpenRecord(kRecord, kRecordVersion) || !a_intfc->WriteRecordData(size) || !a_intfc->WriteRecordData(text.data(), size)) {
			logger::error("cast: could not save her auras, mana and cooldowns");
		}
	}

	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length)
	{
		std::uint32_t size = 0;
		if (a_length < sizeof(size) || a_length > 4*1024*1024 || a_intfc->ReadRecordData(size) != sizeof(size) || size != a_length - sizeof(size)) {
			logger::error("cast: a damaged saved record; she starts fresh");
			return;
		}
		std::string text(size, '\0');
		if (a_intfc->ReadRecordData(text.data(), size) != size) {
            logger::error("cast: incomplete saved record; she starts fresh"); return;
        }
		auto j = json::parse(text, nullptr, false);
		if (!CastPersistence::Valid(j)) {
			logger::error("cast: an unreadable saved record; she starts fresh");
			return;
		}
		g_loaded = std::move(j);
	}

	void Damaged(RE::Actor* a_victim, int a_amount) { DamageBreak(a_victim, a_amount, 0); }

	bool ItemReady(std::uint32_t a_spell)
	{
		const auto it = g_cooldowns.find(a_spell);
		return it == g_cooldowns.end() || it->second <= Frame::Now();
	}
	void ItemCooldown(std::uint32_t a_spell, std::uint32_t a_ms)
	{
		StartCooldown(a_spell, {{"cooldown_ms", a_ms}});
	}

	int Mana() { return static_cast<int>(Player()->AsActorValueOwner()->GetActorValue(RE::ActorValue::kMagicka)); }

	int MaxMana()
	{
		auto* player = Player();
		return static_cast<int>(player->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kMagicka) +
								player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kMagicka));
	}
	bool Casting() { return g_cast.has_value() || g_pending || g_channel.has_value(); }
	bool InFiveSecondRule() { return Casting() || Frame::Now() - g_manaSpentAt < 5; }

	void IncomingHit(RE::Actor* a_attacker, RE::FormID a_source, RE::FormID a_projectile)
	{
		if (!a_attacker || a_attacker == Player() || a_attacker->IsDead()) return;
		const auto* source = a_source ? RE::TESForm::LookupByID(a_source) : nullptr;
		const bool weapon = !source || source->IsWeapon();
		if (!weapon) return;   // DBC incoming weapon-hit flags 0x28, not spells/carrier recursion
		auto caster = CasterNow();
		std::uint32_t grasp = 0;
		for (const auto& a : g_auras) if (!a.target && a.end > Frame::Now() &&
			(a.spell == 16689 || a.spell == 16810 || a.spell == 16811 || a.spell == 16812 || a.spell == 16813 || a.spell == 17329)) grasp = a.spell;
		if (g_incomingPending.contains(grasp)) return;
		g_incomingPending.insert(grasp);
		Service::Request({ { "op", "incoming_hit" }, { "weapon_hit", true }, { "caster", caster },
			{ "target", {{"kind", "unit"}, {"level", a_attacker->GetLevel()}, {"reaction", "hostile"}, {"alive", true},
				{"distance_yd", Player()->GetPosition().GetDistance(a_attacker->GetPosition()) / kUnitsPerYard}} },
			{ "seed", std::uniform_int_distribution<std::uint32_t>()(g_rng) } },
			[epoch = g_epoch, grasp, handle = a_attacker->CreateRefHandle()](const json& r) {
				if (epoch != g_epoch) return;
				g_incomingPending.erase(grasp);
				auto attacker = handle.get();
				if (!r.value("ok", false) || !attacker || attacker->IsDead() ||
					(grasp && !std::ranges::any_of(g_auras, [grasp](const Aura& a) { return !a.target && a.spell == grasp && a.end > Frame::Now(); }))) return;
				TalentEffects(r, attacker.get());
				if (grasp && std::ranges::any_of(r.value("procs",json::array()), [grasp](const json& proc) {
					return proc.value("source_spell",0u)==grasp; }))
					g_talentEvidence["grasp_procs"] = g_talentEvidence.value("grasp_procs", 0) + 1;
			});
	}

	void ApplyForTesting(std::uint32_t a_spell, const json& a_outcome, RE::Actor* a_target)
	{
		Land({ a_spell, a_outcome, a_target ? a_target->CreateRefHandle() : RE::ActorHandle{} });
	}

	void BeginForTesting(std::uint32_t a_spell, const json& a_outcome, RE::Actor* a_target)
	{
		Stop();
		g_cast = InFlight{a_spell, a_outcome, a_target ? a_target->CreateRefHandle() : RE::ActorHandle{}};
		g_cast->castTime = a_outcome.value("cast_ms", 0) / 1000.0;
		g_cast->endAt = Frame::Now() + g_cast->castTime;
		if (g_cast->castTime == 0) Complete();
	}

	json TalentDiagnostics()
	{
		auto out = g_talentEvidence;
		out["auras"] = json::array();
		for (const auto& a : g_auras) {
			const auto actor = a.target.get();
			if (a.end > Frame::Now() && (!a.target || actor)) out["auras"].push_back({ {"spell", a.spell}, {"target", a.target ? actor->GetFormID() : Player()->GetFormID()},
				{"root", a.root}, {"stun", a.sleep}, {"control_remaining",a.controlEnd - Frame::Now()},
				{"reveal",a.reveal}, {"lockout",a.lockout}, {"armor_reduction",a.armorReduction}, {"remaining", a.end - Frame::Now()} });
		}
		out["cast_end"] = g_cast ? g_cast->endAt : 0;
		out["channel_end"] = g_channel ? g_channel->end : 0;
		out["hurricane_range"] = SpellRange(16914);
		return out;
	}

	std::uint32_t TrackingMask()
	{
		for (const auto& a : g_auras) {
			if (!a.target && a.end > Frame::Now() && a.trackingAura == 44) return a.trackingMask;
		}
		return 0;
	}

	void FormTracking(const json& a_outcome)
	{
		if (!a_outcome.contains("form_tracking") || Forms::Current() != "cat" || !Progression::Knows(5225)) return;
		for (const auto& a : g_auras) {
			if (!a.target && a.end > Frame::Now() && (a.spell == 2383 || a.spell == 2580)) return;
		}
		AddAura(5225, a_outcome.at("form_tracking"), {});
	}

	void CancelAura(std::uint32_t a_spell)
	{
		for (auto& a : g_auras) {
			if (!a.target && a.spell == a_spell && !a.harmful) {
				a.end = Frame::Now();
			}
		}
	}

	void EndAura(std::uint32_t a_spell)
	{
		for (auto& a : g_auras) {
			if (!a.target && a.spell == a_spell) {
				a.end = Frame::Now();
			}
		}
	}

	void Augment(json& a_caster)
	{
		const double now = Frame::Now();
		a_caster["auras"] = json::array();
		for (const auto& aura : g_auras) {
			if (!aura.target && aura.end > now) {
				a_caster["auras"].push_back(aura.spell);
			}
		}
		a_caster["proc_cooldowns"] = json::object();
		for (const auto& [spell, until] : g_cooldowns) {
			if (until > now) {
				a_caster["proc_cooldowns"][std::to_string(spell)] = std::lround((until - now) * 1000.0);
			}
		}
	}

	void AugmentTarget(json& a_target, RE::Actor* a_actor)
	{
		if (!a_actor) return;
		float reduction = 0;
		for (const auto& aura : g_auras) if (aura.end > Frame::Now() && aura.target.get().get() == a_actor) {
			// Faerie Fire ranks and caster/feral versions do not stack.
			reduction = std::max(reduction, aura.armorReduction);
		}
		a_target["armor_reduction"] = reduction;
	}

	void TalentEffects(const json& a_outcome, RE::Actor* a_target, bool a_consume)
	{
		if (a_consume) {
			for (const auto& id : a_outcome.value("consumed_auras", json::array())) {
				EndAura(id.get<std::uint32_t>());
			}
		}
		for (auto proc : a_outcome.value("procs", json::array())) {
			const auto spell = proc.value("spell", 0u);
			StartCooldown(proc.value("source_spell", 0u), proc);
			Forms::Apply(proc, nullptr);   // rage/energy power events; combo gain is already in the parent outcome
			for (const auto& e : proc.value("events", json::array())) {
				const auto type = e.value("type", "");
				auto* recipient = e.value("target", proc.value("target", "caster")) == "target" ? a_target : Player();
				if (recipient && type == "damage") {
					const auto amount = e.value("amount", 0.f);
					Carriers::Damage(recipient, amount);
					Damaged(recipient, static_cast<int>(amount));
				} else if (recipient && type == "heal") {
					recipient->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kHealth, e.value("amount", 0.f));
				}
				if (type == "power" && e.value("power", "") == "mana") {
					Player()->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kMagicka, e.value("amount", 0.0f) * 0.2f);
				}
			}
			if (!proc.value("auras", json::array()).empty()) {
				proc["name"] = WowUi::Facts(spell).name;
				const auto on = proc.value("target", "caster") == "target" && a_target ? a_target->CreateRefHandle() : RE::ActorHandle{};
				AddAura(spell, proc, on);
			}
		}
	}
}
