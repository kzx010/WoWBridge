#include "Mounts.h"
#include "Cast.h"
#include "SkyrimAuras.h"
#include "Integration.h"

#include "Forms.h"
#include "Melee.h"
#include "Gear.h"
#include "Armory.h"

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
#include "Hooks.h"
#include "Input.h"
#include "LuaText.h"
#include "Picker.h"
#include "Progression.h"
#include "Service.h"
#include "WorldLog.h"
#include "SkyrimSpells.h"
#include "SpellVisuals.h"
#include "ui/WowUi.h"

#include <mutex>

namespace Bridge { nlohmann::json NativeActorValues(RE::Actor* actor); }

namespace Cast
{
	namespace
	{
		using json = nlohmann::json;

		// The slice character: level 60, Stormrage Raiment 8/8, Staff of Jordan, Idol of Rejuvenation (STATUS M3).
		constexpr float kSkyrimToWowHp = 4.0f;   // the root-break roll weighs damage on WoW's health scale (docs/authority.md)
		constexpr float kUnitsPerYard = 64.0f;
		std::atomic<bool> g_areaFriendly = false;   // Interface Options: area spells strike friendly people too
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
			std::string                          stack;    // SpellStacker's positive family siblings replace each other ...
			std::string                          stackRule;   // unique_per_caster: any of the group replaces another (a Paladin's
			                                                  // seal, judgement, blessing, aura: spell_group rules)
			std::map<std::string, float>         stackAmounts;   // ... where a similar aura effect overlaps (effect:aura:sign)
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
			bool                                 fear = false;
			std::uint32_t                        immunity = 0;
			bool                                 purges = false;   // its immunity takes existing effects of those schools off
			bool                                 pacify = false;
			bool                                 freedom = false;
			bool                                 partySource = false, partyMember = false;
			json                                 effects;   // service aura types, including WoW-only talent effects
			std::uint64_t                        serial = 0, areaOwner = 0;
			int                                  charges = 0;
			bool                                 calm = false;    // soothed (Soothe Animal): an unengaged beast keeps the peace until hurt
			bool                                 noPushback = false;   // Barkskin (100%); partial resistance rolls per hit
			float                                pushbackResist = 0;
			double                               duration = 0;   // full judgement duration, including Lasting Judgement
			bool                                 unattackable = false;
			std::string                          dispel;          // a periodic dispel's kind (Abolish Poison) ...
			double                               dispelEvery = 0, nextDispel = 0;
			int                                  dispelCount = 0;
			bool                                 form = false;          // their form, as the client shows it (a buff)
			bool                                 endsWithForm = false;  // Prowl, Dash, Tiger's Fury: gone when the player shifts
			std::uint32_t                        trackingAura = 0, trackingMask = 0;
			json                                 formOutcome;   // resolved stance data needed when restoring tracking
			bool                                 stealth = false;       // Prowl or Shadowmeld
			bool                                 reveal = false;        // Faerie Fire blocks native concealment
			bool                                 lockout = false;       // Feral Charge's native spell interruption
			float                                armorReduction = 0;    // original WoW units, for later melee requests
			float                                wowPerTick = -1;      // Swiftmend's input, before native conversion
			json                                 tooltip;              // effect values at application (WoW units)
			bool                                 respite = false;      // Skyrim stamina alongside each healing tick
			float                                holyThreat = 1;
			float                                threat = 1;           // healing threat snapshot
			bool                                 taunt = false;        // made to attack the player while it lasts (SPELL_AURA_MOD_TAUNT) ...
			bool                                 tauntHolds = false;   // ... and after: Growl matched their threat to the top (EffectTaunt)
			RE::ActorHandle                      tauntedFrom;          // whom it attacked before (Challenging Roar returns it there)
			double                               tauntAt = 0;          // its next look at its target
			// One unit at a time (SpellMgr::IsSingleTargetSpell): Hibernate, Entangling Roots. IsSingleTargetSpells
			// matches another of theirs by its spell chain, or by one family's overlapping flags (Nature's Grasp's roots).
			struct Single
			{
				std::uint32_t chain = 0, family = 0;
				std::uint64_t flags = 0;
				bool Matches(const Single& o) const { return chain == o.chain || (family && family == o.family && (flags & o.flags)); }
			};
			std::optional<Single>                single;
		};

		// SpellStacking.cpp IsStackableAuraEffect: one positive family group, with a similar aura effect in common.
		bool SameStack(const Aura& a_aura, const std::string& a_stack, const std::map<std::string, float>& a_amounts)
		{
			if (a_stack.empty() || a_aura.stack != a_stack) return false;
			if (a_aura.stackAmounts.empty() || a_amounts.empty()) return true;   // a save from before the amounts
			return std::ranges::any_of(a_amounts, [&](const auto& kv) { return a_aura.stackAmounts.contains(kv.first); });
		}
		// IsSimilarExistingAuraStronger: a similar effect of the aura already there is larger.
		bool Stronger(const Aura& a_aura, const std::map<std::string, float>& a_amounts)
		{
			return std::ranges::any_of(a_amounts, [&](const auto& kv) {
				const auto it = a_aura.stackAmounts.find(kv.first);
				return it != a_aura.stackAmounts.end() && it->second > kv.second; });
		}
		// Spell::CheckCast: a stronger similar aura of the group already on the target bounces the cast before anything is
		// paid (the same spell too: a talent's snapshot can be stronger than a new cast).
		bool Bounces(const json& a_outcome, RE::ActorHandle a_on);

		// A party spell (Gift of the Wild's target 37) is an area spell to Spell::CheckCast, which bounces none; each
		// member keeps a stronger similar aura instead (AddAura).
		bool PartyWide(const json& a_outcome)
		{
			return std::ranges::any_of(a_outcome.value("auras", json::array()), [](const json& a) {
				return a.value("area", json::object()).value("party", false); });
		}


		// A channelled area (Hurricane): ticks on whatever is inside the circle each amplitude, while you stand still.
		struct Channel
		{
			std::uint32_t           spell = 0;
			std::uint64_t           serial = 0;
			std::string             name;
			int                     school = 0;
			RE::NiPoint3            at;               // the circle's centre
			float                   radius = 0;       // units
			double                  start = 0, end = 0, amplitude = 1, nextTick = 0;
			int                     tick = 0;
			std::set<RE::FormID>    hit;              // shown the aura's debuff
			std::set<RE::FormID>    slowed;           // Hurricane's storm on their attacks (a native carrier)
			std::uint64_t           loop = 0;         // the channel's sound
			std::uint32_t           interruptFlags = 0;   // ChannelInterruptFlags
			int                     delays = 0;
			SpellVisuals::Handle    visual = 0;
			SpellVisuals::Handle    area = 0;         // its storm where it was placed
			bool                    heal = false;     // a party heal (Tranquility): the player and their followers in the circle
			bool                    selfAura = false; // item self-channel: its already resolved auras own the ticks
			float                   pushbackResist = 0;
		};

		bool                 g_pending = false;           // a resolve is in flight
		std::uint32_t        g_request = 0;               // Stop also cancels an unanswered resolve
		struct FormFollowup { std::uint32_t spell, shift; RE::ActorHandle target; bool targeted; double expires; };
		std::optional<FormFollowup> g_formFollowup;
		std::uint32_t        g_epoch = 0;                 // replies from before a Reset are dropped
		std::uint32_t        g_placing = 0;               // a ground spell waiting for its place
		std::optional<Channel> g_channel;
		std::vector<Channel> g_areas;   // Consecration: fixed ground, independent of cast/channel ownership
		std::uint64_t g_areaSerial = 0;
		std::map<std::uint32_t, std::uint64_t> g_groundSerial;
		std::optional<InFlight> g_cast;
		std::vector<Aura>    g_auras;
		std::atomic<bool> g_paladin = false;
		json g_mechanicProfile = json::object(), g_mechanicAsked;
		bool g_mechanicPending = false;
		double g_mechanicNext = 0;
		std::uint64_t g_mechanicEpoch = 0;
		// Start runs on the engine's effect thread too. Publish amounts, never the live aura table, to that thread.
		std::array<std::atomic<float>, 31> g_mechanicResist{};
		std::array<std::atomic<unsigned>, 31> g_mechanicRolls{}, g_mechanicResisted{};
		void PublishMechanics()
		{
			static const std::map<int, const char*> names{{2,"disoriented"},{5,"fear"},{9,"silence"},{26,"interrupt"}};
			for (const auto& [mechanic, name] : names) {
				float amount = Character::Get().classId == 2 ? g_mechanicProfile.value(name, 0.f) : 0.f;
				if (Character::Get().classId == 2) for (const auto& a : g_auras) if (!a.target && a.end > Frame::Now())
					for (const auto& e : a.effects) if (e.value("type", "") == "mechanic_resistance" && e.value("mechanic", 0) == mechanic)
						amount += e.value("amount", 0.f);
				g_mechanicResist[mechanic] = std::clamp(amount, 0.f, 100.f);
			}
		}
		bool ResistMechanic(int mechanic)
		{
			if (!Integration::Active() || !g_paladin || mechanic <= 0 || mechanic >= 31) return false;
			const float amount = g_mechanicResist[mechanic];
			// Unit::CalculateSpellResistChance: additive mechanic resistance, clamped to 0..100; one application roll.
			thread_local std::mt19937 rng{std::random_device{}()};
			const bool resisted = std::uniform_real_distribution<float>(0, 100)(rng) < amount;
			++g_mechanicRolls[mechanic];
			if (resisted) ++g_mechanicResisted[mechanic];
			return resisted;
		}
		int NativeMechanic(const RE::EffectSetting* base)
		{
			if (!base) return 0;
			using A = RE::EffectSetting::Archetype;
			// Skyrim adaptation: Demoralize -> FEAR (5); magic Stagger, a moment's loss of control -> DISORIENTED (2);
			// Paralysis, frozen where the player stands, is a STUN (12), which nothing of a Paladin resists. Neither physical
			// hit stagger nor arbitrary stat drains are a spell mechanic.
			if (base->data.archetype == A::kDemoralize) return 5;
			if (base->data.archetype == A::kStagger) return 2;
			if (base->data.archetype == A::kParalysis) return 12;
			return 0;
		}
		template<int N> struct MechanicStart
		{
			static void thunk(RE::ActiveEffect* effect)
			{
				const auto* base = effect->GetBaseObject();
				// Not WoWBridge's own control carriers (its fear/sleep spells): a WoW fear on the player was already judged by
				// the service. Other runtime spells (a test's fixture, another mod's) are native to the player.
				const bool resisted = effect->GetTargetActor() == RE::PlayerCharacter::GetSingleton() && effect->spell &&
					!Carriers::IsControlCarrier(effect->spell) && ResistMechanic(NativeMechanic(base));
				// Stagger has no modifier to unwind and starts an immediate animation. Refuse that Start on success;
				// the other effects must finish their own recorded AV changes normally when dispelled.
				if (!resisted || !base || base->data.archetype != RE::EffectSetting::Archetype::kStagger) func(effect);
				if (resisted) effect->Dispel(true);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		json MechanicCaster()
		{
			auto caster = Character::Caster();
			caster.erase("actor_values");
			// Only passives/equipment here. Live aura amounts come from their actual outcomes above, once.
			caster["auras"] = json::array();
			return caster;
		}
		void PollMechanics()
		{
			if (Character::Get().classId != 2) {
				g_mechanicProfile = json::object(); g_mechanicAsked = json();
				PublishMechanics(); return;
			}
			if (g_mechanicPending || Frame::Now() < g_mechanicNext) return;
			g_mechanicNext = Frame::Now() + 1;
			const auto caster = MechanicCaster();
			if (caster == g_mechanicAsked) return;
			g_mechanicAsked = caster; g_mechanicPending = true;
			g_mechanicProfile = json::object(); PublishMechanics();
			Service::Request({{"op","stats"},{"caster",caster}}, [caster, epoch = g_mechanicEpoch](const json& out) {
				if (epoch != g_mechanicEpoch) return;
				g_mechanicPending = false;
				if (!out.value("ok",false) || Character::Get().classId != 2 || MechanicCaster() != caster) {
					g_mechanicAsked = json(); return;
				}
				g_mechanicProfile = out.value("paladin",json::object()).value("mechanic_resist_percent",json::object());
				PublishMechanics();
			});
		}
		bool Bounces(const json& a_outcome, RE::ActorHandle a_on)
		{
			const auto stack = a_outcome.value("stack", std::string());
			if (stack.empty() || PartyWide(a_outcome)) return false;
			const auto amounts = a_outcome.value("stack_amounts", std::map<std::string, float>{});
			return std::ranges::any_of(g_auras, [&](const Aura& x) {
				return x.target == a_on && x.end > Frame::Now() && SameStack(x, stack, amounts) && Stronger(x, amounts); });
		}
		double               g_gcdUntil = 0;

		std::map<std::uint32_t, double> g_cooldowns;
		struct Absorb { float remaining = 0; std::uint32_t schools = 0; double end = 0; };
		std::map<std::uint32_t, Absorb> g_absorbs;   // self shields; accessed by native AV damage, before death checks
		std::mutex g_absorbMutex;
		std::atomic<std::uint32_t> g_defenseEpoch = 0;
		struct Immunity { std::uint32_t schools; double end; bool pacify, freedom, unattackable; };
		struct PartyProtection {
			json auras = json::array(), casterAuras = json::array();
			std::array<float, 7> reduction{}, transfer{};
			bool casterAlive = true;
		};
		std::map<RE::Actor*, PartyProtection> g_partyProtection;
		std::uint64_t g_auraSerial = 0;
		double g_partyPollAt = 0, g_partyPollDeadline = 0;
		std::uint64_t g_partyPollSerial = 0;
		bool g_partyPollBusy = false;
		std::mutex g_immunityMutex;
		std::map<RE::Actor*, std::vector<Immunity>> g_immunities;
		std::map<RE::Actor*, json> g_judgedTargets;   // request-time aura IDs, including a follower's killing blow
		std::map<std::uint32_t, double> g_paladinProcCooldowns;
		std::set<std::uint32_t> g_paladinManaSources;   // keep conversion identity after an aura expires during a reply
		void PublishImmunities()
		{
			PublishMechanics();
			g_paladin = Character::Get().classId == 2;
			std::scoped_lock lock(g_immunityMutex);
			g_immunities.clear();
			g_judgedTargets.clear();
			g_partyProtection.clear();
			if (!g_paladin) return;
			json casterAuras = json::array();
			for (const auto& a : g_auras) if (!a.target && a.end > Frame::Now()) casterAuras.push_back(a.spell);
			for (const auto& a : g_auras) if (a.end > Frame::Now()) {
				auto* who = a.target ? a.target.get().get() : static_cast<RE::Actor*>(RE::PlayerCharacter::GetSingleton());
				if (!who) continue;
				g_immunities[who].push_back({a.immunity, a.end, a.pacify, a.freedom, a.unattackable});
				if (a.stack == "Judgement") {
					auto& ids = g_judgedTargets[who]; if (!ids.is_array()) ids = json::array(); ids.push_back(a.spell);
				}
				// Their blessings' protection, not a judgement's damage_taken on an enemy (Judgement of the Crusader).
				if (!a.harmful) for (const auto& e : a.effects) {
					const auto type = e.value("type", "");
					if (type != "damage_taken" && type != "damage_transfer" && type != "block_damage") continue;
					if (!a.target && type != "damage_taken") continue;
					auto& protection = g_partyProtection[who];
					protection.casterAuras = casterAuras;
					if (std::ranges::none_of(protection.auras, [&](const json& id) { return id == a.spell; })) protection.auras.push_back(a.spell);
					for (int school = 0; school < 7; ++school) if (e.value("school_mask", 127u) & (1u << school)) {
						if (type == "damage_taken") protection.reduction[school] -= e.value("native_per_hit", 0.f);
						if (type == "damage_transfer") protection.transfer[school] = e.value("native_per_hit", 0.f);
					}
				}
			}
		}
		bool ImmuneTo(RE::Actor* actor, std::uint32_t school)
		{
			std::scoped_lock lock(g_immunityMutex);
			const auto it = g_immunities.find(actor);
			return it != g_immunities.end() && std::ranges::any_of(it->second, [school](const Immunity& i) {
				return i.end > Frame::Now() && (i.schools & school || (i.schools == 127 && !school)); });
		}
		thread_local RE::Actor* g_damageAttacker = nullptr;
		thread_local const char* g_attackKind = "spell";
		thread_local bool g_defenseApplied = false;
		thread_local bool g_physicalRanged = false;
		thread_local bool g_physicalBlocked = false;
		thread_local bool g_transferDamage = false;
		bool PartyDamage(RE::Actor* actor, float& damage);
		void ObserveNativeHit(RE::Actor* victim, float damage, float reduced = 0);
		float SelfSpellReduction(RE::Actor* victim, float damage);
		bool DefenseAlly(RE::Actor* actor)
		{
			return actor && (actor == RE::PlayerCharacter::GetSingleton() || actor->IsPlayerTeammate() || Bridge::Reaction(actor) >= 5);
		}
		struct AttackContext
		{
			RE::Actor* previous = g_damageAttacker;
			const char* kind = g_attackKind;
			AttackContext(RE::Actor* attacker, const char* attack) { g_damageAttacker = attacker; g_attackKind = attack; }
			~AttackContext() { g_damageAttacker = previous; g_attackKind = kind; }
		};
		void QueueDefense(RE::Actor* attacker, float damage, const char* kind, std::uint32_t school,
			std::uint32_t seed, bool inFront, bool sitting, bool casting, bool stunned);
		bool DeferDefense(RE::Actor* actor, float damage);
		thread_local std::uint32_t g_damageSchool = 0;   // unknown native damage only matches an all-school shield
		struct DamageSchool
		{
			std::uint32_t previous = g_damageSchool;
			explicit DamageSchool(std::uint32_t school) { g_damageSchool = school; }
			~DamageSchool() { g_damageSchool = previous; }
		};

		thread_local bool g_reported = false;   // their WoW damage (the damage carrier): DamageBreak has judged it already
		struct Reported
		{
			bool previous = g_reported;
			explicit Reported(bool a_reported) { g_reported = previous || a_reported; }
			~Reported() { g_reported = previous; }
		};

		// Whom their crowd control holds (Watch, each frame), compared only, and the native damage each took since its
		// last break roll (NpcHealthDamage, on any thread; NativeDamage takes it to DamageBreak on the main one).
		struct Hurt { float amount = 0; double rolled = -1e9; };
		std::map<RE::Actor*, Hurt> g_hurt;
		std::mutex                 g_hurtMutex;
		std::atomic<bool>          g_holding = false;
		constexpr double           kNativeRollEvery = 0.1;

		float AbsorbDamage(float damage)
		{
			std::scoped_lock lock(g_absorbMutex);
			for (auto& [spell, shield] : g_absorbs) {
				if (shield.end <= Frame::Now() || (shield.schools != 127 && !(shield.schools & g_damageSchool))) continue;
				const float used = std::min(damage, shield.remaining);
				shield.remaining -= used; damage -= used;
				if (damage <= 0) break;
			}
			return damage;
		}

		struct NativeProcHit
		{
			RE::ActorHandle attacker, victim;
			float damage, reduced;
			std::string kind;
			std::uint32_t school, epoch;
			double ready;
			json judgements;
		};
		std::mutex g_nativeProcMutex;
		std::vector<NativeProcHit> g_nativeProcHits;
		struct NativePartyTick
		{
			RE::ActorHandle member, attacker;
			PartyProtection protection;
			std::uint32_t school, epoch;
			double ready;
			float damage = 0, applied = 0, reduced = 0, transferred = 0, transferLimit = 0;
		};
		std::vector<NativePartyTick> g_nativePartyTicks; // the same mutex as native proc observations
		struct NativeReductionTick
		{
			RE::ActorHandle attacker;
			std::uint32_t school, epoch;
			double ready;
			float remaining;
		};
		std::vector<NativeReductionTick> g_nativeReductions;
		float SelfSpellReduction(RE::Actor* victim, float damage)
		{
			if (!g_paladin || g_reported || g_defenseApplied || g_transferDamage || !g_damageAttacker ||
				victim != RE::PlayerCharacter::GetSingleton() || std::string_view(g_attackKind) != "spell") return 0;
			float reduction = 0;
			const auto mask = g_damageSchool;
			const std::uint32_t school = mask & 1 ? 0 : mask & 2 ? 1 : mask & 4 ? 2 : mask & 8 ? 3 : mask & 16 ? 4 : mask & 32 ? 5 : mask & 64 ? 6 : 0;
			{ std::scoped_lock lock(g_immunityMutex);
				const auto it = g_partyProtection.find(victim);
				if (it != g_partyProtection.end()) reduction = it->second.reduction[school];
			}
			if (reduction <= 0) return 0;
			const auto attacker = g_damageAttacker->CreateRefHandle();
			std::scoped_lock lock(g_nativeProcMutex);
			auto it = std::ranges::find_if(g_nativeReductions, [&](const NativeReductionTick& tick) {
				return tick.attacker == attacker && tick.school == school && tick.epoch == g_defenseEpoch && tick.ready > Frame::Now();
			});
			if (it == g_nativeReductions.end()) {
				g_nativeReductions.push_back({attacker, school, g_defenseEpoch.load(), Frame::Now() + 1, reduction});
				it = std::prev(g_nativeReductions.end());
			}
			const float used = std::min(damage, it->remaining);
			it->remaining -= used;
			return used;
		}
		void ObserveNativeHit(RE::Actor* victim, float damage, float reduced)
		{
			if (!Integration::Active() || !g_paladin || g_reported || g_defenseApplied || g_transferDamage ||
				!g_damageAttacker || g_damageAttacker == victim || damage <= 0) return;
			const bool onPlayer = victim == RE::PlayerCharacter::GetSingleton();
			// Any thread (spell damage): allies are left out by NativeHitProcs on the main thread, not here.
			if (onPlayer ? std::string_view(g_attackKind) != "spell" :
				g_damageAttacker != RE::PlayerCharacter::GetSingleton() && !g_damageAttacker->IsPlayerTeammate()) return;
			json judgements = json::array();
			if (!onPlayer && g_damageAttacker != RE::PlayerCharacter::GetSingleton()) {
				std::scoped_lock lock(g_immunityMutex);
				const auto it = g_judgedTargets.find(victim);
				if (it == g_judgedTargets.end()) return;
				judgements = it->second;
			}
			const auto attacker = g_damageAttacker->CreateRefHandle(), target = victim->CreateRefHandle();
			const auto mask = g_damageSchool;
			const std::uint32_t school = mask & 1 ? 0 : mask & 2 ? 1 : mask & 4 ? 2 : mask & 8 ? 3 : mask & 16 ? 4 : mask & 32 ? 5 : mask & 64 ? 6 : 0;
			std::scoped_lock lock(g_nativeProcMutex);
			// Native concentration spells keep their synchronous health changes. One accumulated observation per
			// attacker/victim/school per tick asks only for procs; health changes are never replayed.
			if (std::string_view(g_attackKind) == "spell") for (auto& hit : g_nativeProcHits) {
				if (hit.attacker == attacker && hit.victim == target && hit.kind == "spell" && hit.school == school && hit.epoch == g_defenseEpoch && hit.ready > Frame::Now() && hit.judgements == judgements) {
					hit.damage += damage; hit.reduced += reduced; return;
				}
			}
			g_nativeProcHits.push_back({attacker, target, damage, reduced, g_attackKind, school, g_defenseEpoch.load(),
				Frame::Now() + (std::string_view(g_attackKind) == "spell" ? (onPlayer ? 1 : .5) : 0), std::move(judgements)});
		}

		// Where their health actually drops (1.7.104, audited with tools/addrlib.py and llvm-objdump): every damage
		// modifier goes through Actor's ModActorValue implementation (0x6C5220), which asks the virtual
		// CheckClampDamageModifier (0x127) for the delta, stores the new health and only then calls
		// HandleHealthDamage (0x104). Actor::DoDamage (37335) calls that implementation directly, so weapon,
		// arrow and caster-owned spell damage never pass the ActorValueOwner vtable; the clamp sees them all.
		struct HealthDamage
		{
			static float thunk(RE::Actor* actor, RE::ActorValue av, float delta)
			{
				float reduced = 0;
				if (Integration::Active() && av == RE::ActorValue::kHealth && delta < 0) {
					if (!g_transferDamage && !g_defenseApplied && DeferDefense(actor, -delta)) return func(actor, av, 0);
					if (!g_transferDamage && ImmuneTo(actor, g_damageSchool)) delta = 0;
					reduced = SelfSpellReduction(actor, -delta);
					delta = -AbsorbDamage(-delta - reduced);
				}
				const float applied = func(actor, av, delta);
				if (av == RE::ActorValue::kHealth && (applied < 0 || reduced > 0)) ObserveNativeHit(actor, std::max(0.f, -applied) + reduced, reduced);
				return applied;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		// The same clamp on Character: whatever hurts an NPC the player holds (their followers, other NPCs, traps, falls, Skyrim
		// spells, theirs too), by what its health actually loses. Their WoW damage reached DamageBreak with its spell.
		struct NpcHealthDamage
		{
			static float thunk(RE::Actor* actor, RE::ActorValue av, float delta)
			{
				if (Integration::Active() && av == RE::ActorValue::kHealth && delta < 0) {
					if (ImmuneTo(actor, g_damageSchool)) delta = 0;
					else if (!g_defenseApplied && !g_reported) {
						float damage = -delta;
						if (PartyDamage(actor, damage)) delta = -damage;
					}
				}
				const float applied = func(actor, av, delta);
				if (av == RE::ActorValue::kHealth && applied < 0) ObserveNativeHit(actor, -applied);
				if (applied < 0 && av == RE::ActorValue::kHealth && !g_reported && g_holding.load(std::memory_order_relaxed)) {
					std::scoped_lock lock(g_hurtMutex);
					if (const auto it = g_hurt.find(actor); it != g_hurt.end()) it->second.amount -= applied;
				}
				return applied;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		// A native hit (melee, unarmed, arrows and bolts, physical explosions and traps): Actor's hit processing
		// (38586) applies its HitData damage through this one DoDamage call. A known spell school is kept.
		struct PhysicalDamage
		{
			static bool thunk(RE::Actor* actor, float damage, RE::Actor* attacker, bool noDifficulty)
			{
				DamageSchool school(g_damageSchool ? g_damageSchool : 1);
				AttackContext context(attacker, g_physicalRanged ? "ranged" : "melee");
				if (Integration::Active() && ((attacker && Pacified(attacker)) || Unattackable(actor))) return false;   // no attacker: a trap or a fall
				return func(actor, damage, attacker, noDifficulty);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		// 1.7.104 HitData's mitigation (44014+0x21C) asks IsBlocking before setting kBlocked or reducing
		// totalDamage. Returning false here leaves native armor intact and gives the WoW roll the unblocked hit.
		struct NativeBlock
		{
			static bool thunk(RE::Actor* actor)
			{
				if (Integration::Active() && g_paladin && actor == RE::PlayerCharacter::GetSingleton()) return false;
				return func(actor);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		// The hit record at 38586+0xFA, before its damage is read, distinguishes arrows from melee for the clamp.
		struct NativeHit
		{
			static void thunk(RE::AIProcess* process, RE::HitData* hit)
			{
				g_physicalBlocked = hit && hit->flags.any(RE::HitData::Flag::kBlocked);
				g_physicalRanged = hit && hit->weapon && (hit->weapon->IsBow() || hit->weapon->IsCrossbow());
				if (Integration::Active() && g_paladin && g_physicalBlocked && !g_physicalRanged && hit->totalDamage <= 0) {
					// A full native block may skip DoDamage and the health clamp, but Sanctuary still procs.
					auto member = hit->target.get(); auto source = hit->aggressor.get();
					DamageSchool school(g_damageSchool ? g_damageSchool : 1);
					AttackContext context(source.get(), "melee");
					float damage = 0;
					if (member && !ImmuneTo(member.get(), g_damageSchool)) PartyDamage(member.get(), damage);
				}
				func(process, hit);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		template<int N> struct MovementStart
		{
			static void thunk(RE::ValueModifierEffect* effect)
			{
				const auto* base = effect->GetBaseObject();
				if (Integration::Active() && base && base->data.primaryAV == RE::ActorValue::kSpeedMult &&
					(base->IsDetrimental() || effect->magnitude < 0) && MovementImmune(effect->GetTargetActor())) {
					// Zero before Start records the modifier, so Finish cannot restore a slow that never applied.
					effect->magnitude = 0;
				}
				func(effect);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		template<int N> struct MagicDamage
		{
			static void thunk(RE::ValueModifierEffect* effect, RE::Actor* actor, float amount, RE::ActorValue av)
			{
				const auto* base = effect->GetBaseObject();
				const auto resist = base ? base->data.resistVariable : RE::ActorValue::kNone;
				// The same mapping as service/skyrim_effects.py: Nature is Shock;
				// Arcane and Shadow share native Resist Magic.
				const std::uint32_t mask = resist == RE::ActorValue::kResistFire ? 4 :
					resist == RE::ActorValue::kResistFrost ? 16 : resist == RE::ActorValue::kResistShock ? 8 :
					resist == RE::ActorValue::kResistMagic ? 96 : 0;
				DamageSchool school(g_damageSchool ? g_damageSchool : mask);   // our carriers keep their WoW school
				Reported reported(effect->spell && Carriers::IsDamage(effect->spell->GetFormID()));
				auto caster = effect->GetCasterActor();
				AttackContext context(caster.get(), "spell");
				if (Integration::Active() && av == RE::ActorValue::kHealth && amount < 0 && ImmuneTo(actor, g_damageSchool)) return;
				func(effect, actor, amount, av);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
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
		float MaximumHealth(RE::Actor* actor)
		{
			return actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth) +
				actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kHealth);
		}
		std::string          Name(RE::Actor* a_actor) { return a_actor ? a_actor->GetDisplayFullName() : ""; }

		void Lua(std::string a_lua) { WowUi::Run(std::move(a_lua)); }

		// A spell tooltip profile (the service's talent_effects.tooltip, skyrim_costs and known_spells answers) as a Lua
		// table, every key as the service sent it. A list of keys here once dropped the Skyrim skill line (skill), the
		// linked form and Pounce bleed values (linked_spells) and Frenzied Regeneration's rate (health_rates).
		std::string TooltipLua(const json& a_values)
		{
			if (!a_values.is_object()) return "nil";
			const auto value = [](const auto& a_self, const json& a_value) -> std::string {
				switch (a_value.type()) {
				case json::value_t::object: {
					std::string out = "{";
					for (const auto& [key, item] : a_value.items()) out += "[" + LuaQuote(key) + "]=" + a_self(a_self, item) + ",";
					return out + "}";
				}
				case json::value_t::array: {
					std::string out = "{";
					for (const auto& item : a_value) out += a_self(a_self, item) + ",";
					return out + "}";
				}
				case json::value_t::string: return LuaQuote(a_value.get<std::string>());
				case json::value_t::boolean: return a_value.get<bool>() ? "true" : "false";
				case json::value_t::number_integer:
				case json::value_t::number_unsigned: return a_value.dump();
				case json::value_t::number_float: {
					const double d = a_value.get<double>();
					return std::isfinite(d) ? std::format("{}", d) : "0";
				}
				default: return "nil";
				}
			};
			return value(value, a_values);
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

		// The service's caster now: what the player wears and their skills (Character), the WoW auras on the player (their spell mods
		// apply: Barkskin's second of casting time) and the reagents in their bags.
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

		// Their party, as a party aura reaches it (Gift of the Wild, Tranquility): the player and their living followers within a
		// radius of a place. An empty handle is the player.
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
			if (Character::Get().classId == 2 && a_multiplier > 1) {
				for (const auto& h : RE::ProcessLists::GetSingleton()->highActorHandles) if (auto a = h.get(); a && !a->IsDead())
					Forms::AttackThreat({{"threat_multiplier", a_multiplier}}, a.get());
				return;
			}
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

		// A unit's victim (ThreatManager::getHostileTarget) is Skyrim's combat target.
		RE::Actor* Victim(RE::Actor* a_actor)
		{
			auto* combat = a_actor ? a_actor->GetActorRuntimeData().combatController : nullptr;
			return combat ? combat->targetHandle.get().get() : nullptr;
		}

		// It turns on someone else, as HealingThreat's transfer does (StartCombat alone keeps a current target).
		bool Retarget(RE::Actor* a_actor, RE::Actor* a_on)
		{
			if (!a_actor || !a_on || a_actor->IsDead() || a_on->IsDead() || Unattackable(a_on)) return false;
			if (Victim(a_actor) == a_on) return true;
			a_actor->StopCombat();
			return a_actor->StartCombat(a_on);
		}

		// A follower fights when in combat, with a combat target, or as some living enemy's target (a passive one too).
		bool Fighting(RE::Actor* a_follower)
		{
			if (a_follower->AsActorState()->IsBleedingOut()) return false;   // down: nothing can hurt it, so nothing turns on it
			if (a_follower->IsInCombat() || Victim(a_follower)) return true;
			for (auto& handle : RE::ProcessLists::GetSingleton()->highActorHandles)
				if (const auto other = handle.get(); other && !other->IsDead() && Victim(other.get()) == a_follower) return true;
			return false;
		}

		// Cower (EffectThreat, negative): its target's threat on the player falls. Skyrim keeps no threat table: deterministically,
		// one attacking the player turns on the nearest follower fighting (in combat, or with a combat target) within 30 yards, the
		// one the fall leaves on top; with none, nothing changes, as the fall changes no victim. Only its target: the
		// effect is single-target.
		void Cower(RE::Actor* a_target)
		{
			auto* player = Player();
			if (!a_target || a_target->IsDead() || Victim(a_target) != player) return;
			RE::Actor* follower = nullptr;
			for (const auto& h : Party(player->GetPosition(), 30 * kUnitsPerYard)) {
				auto candidate = h.get();   // an empty handle is the player
				if (candidate && Fighting(candidate.get()) && (!follower || candidate->GetPosition().GetDistance(a_target->GetPosition()) <
					follower->GetPosition().GetDistance(a_target->GetPosition()))) follower = candidate.get();
			}
			if (follower && Retarget(a_target, follower)) g_talentEvidence["cower_redirects"] = g_talentEvidence.value("cower_redirects", 0) + 1;
		}

		// HandleAuraModShapeshift, taking a form and leaving one alike: roots and snares on the player end.
		void FreeMovement()
		{
			if (const int removed = Carriers::Unsnare(Player()); removed > 0) {
				g_talentEvidence["snares_removed"] = g_talentEvidence.value("snares_removed", 0) + removed;
			}
		}

		bool Shapeshifts(const json& a_outcome)
		{
			return std::ranges::any_of(a_outcome.value("auras", json::array()), [](const json& a) {
				return a.value("type", "") == "shapeshift" && !a.value("removes", json::array()).empty(); });
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

		// Dispel (SpellEffects.cpp Spell::EffectDispel, ProcessDispelList): count times, one of the unit's auras of that
		// type at random goes, an aura being a spell from one caster with all its effects. A Skyrim effect has the type
		// its aura shows (SkyrimAuras::DispelType): a disease (Cure Disease, Purify, Cleanse), any the player has contracted
		// even while a vanilla condition rests its effect, leaves the player as a shrine cures it; Magic leaves a friend only when harmful (SpellAuraHolder::IsDispellableByMask). Skyrim has no curses
		// (Remove Curse finds nothing); the plugin's own WoW auras are not Skyrim effects. Logged as the client logs it.
		using DispelAuras = std::map<std::pair<RE::MagicItem*, std::uint32_t>, std::vector<RE::ActiveEffect*>>;
		DispelAuras DispelsOn(RE::Actor* a_actor, const std::string& a_type)
		{
			if (!a_actor) return {};
			auto* effects = a_actor ? a_actor->GetActiveEffectList() : nullptr;
			DispelAuras auras;
			if (effects) for (auto* e : *effects) {
				const auto* base = e ? e->GetBaseObject() : nullptr;
				if (base && SkyrimAuras::Visible(e) && SkyrimAuras::DispelType(e) == a_type && (a_type != "Magic" || base->IsDetrimental())) {
					auras[{ e->spell, e->caster.native_handle() }].push_back(e);
				}
			}
			if (a_type == "Disease") {   // a disease the player has contracted, its effect resting on a vanilla condition meanwhile
				for (auto* spell : a_actor->GetActorRuntimeData().addedSpells) {
					if (spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kDisease &&
						std::ranges::none_of(auras, [&](const auto& a_aura) { return a_aura.first.first == spell; })) {
						auras[{ spell, 0 }];
					}
				}
			}
			return auras;
		}

		json Dispellable(RE::Actor* a_actor)
		{
			auto types = json::array();
			for (const char* type : {"Curse", "Poison", "Disease", "Magic"})
				if (!DispelsOn(a_actor, type).empty()) types.push_back(type);
			return types;
		}

		int Dispel(RE::Actor* a_actor, const std::string& a_type, int a_count)
		{
			auto auras = DispelsOn(a_actor, a_type);
			int removed = 0;
			for (; removed < std::max(a_count, 1) && !auras.empty(); ++removed) {   // a count of 0 means 1
				auto it = auras.begin();
				std::advance(it, std::uniform_int_distribution<std::size_t>(0, auras.size() - 1)(g_rng));
				auto* spell = it->first.first;
				const std::string name = spell && spell->GetFullName() ? spell->GetFullName() : "";
				auto* disease = spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kDisease ? spell->As<RE::SpellItem>() : nullptr;
				if (!disease || !a_actor->RemoveSpell(disease)) {   // a contracted disease is a spell the player has
					for (auto* e : it->second) e->Dispel(true);
				}
				auras.erase(it);
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

		// Their mana is Skyrim's magicka (their gear's Intellect is Fortify Magicka; Skyrim regenerates it), and the
		// service's cost is in it. Magicka spent trains the spell's school, as a Skyrim spell's does.
		void Spend(const json& a_outcome)
		{
			for (const auto& r : a_outcome.value("reagents", json::array())) {   // Rebirth's seed, Gift's thornroot
				Gear::Consume(r.value("item", 0u), r.value("count", 1));
			}
			const float cost = a_outcome.value("use_all_mana", false) ? static_cast<float>(Mana()) : a_outcome.value("cost", 0.0f);
			g_talentEvidence["paid_cost"] = cost;
			g_talentEvidence["paid_spell"] = a_outcome.value("spell", 0u);
			if (Forms::Pay(a_outcome)) {   // rage or energy: WoW's own, not their magicka
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
		// from anything but the root's own spell. Their WoW damage comes with its spell; native damage (NativeDamage) with none.
		void DamageBreak(RE::Actor* a_victim, float a_amount, std::uint32_t a_from)
		{
			for (auto& a : g_auras) {   // any damage wakes a sleeping beast (Hibernate) or ends its soothing
				if ((a.sleep && a.wakes || a.calm) && a.target && a.target.get().get() == a_victim) {
					a.end = Frame::Now();
				}
			}
			for (const auto& a : g_auras) {
				if ((!a.root && !a.fear) || a.spell == a_from || a.target.get().get() != a_victim) {
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

		// Native damage on those the player holds, to DamageBreak. Skyrim also hurts every frame (a concentration spell, a
		// burning or poison effect), as WoW never does: that is rolled at most every tenth of a second, as one blow of
		// all it dealt (the chance grows with the damage, so the odds stay about the same).
		void NativeDamage(double a_now)
		{
			std::vector<std::pair<RE::Actor*, float>> hurt;
			{
				std::scoped_lock lock(g_hurtMutex);
				for (auto& [actor, h] : g_hurt) {
					if (h.amount > 0 && a_now - h.rolled >= kNativeRollEvery) {
						hurt.emplace_back(actor, h.amount);
						h = { 0, a_now };
					}
				}
			}
			for (const auto& [actor, amount] : hurt) {
				g_talentEvidence["native_damage"] = g_talentEvidence.value("native_damage", 0) + 1;
				DamageBreak(actor, amount, 0);
			}
		}

		// Whom their root, sleep or soothing holds now: the NPCs NpcHealthDamage reports on.
		void Watch()
		{
			std::set<RE::Actor*> held;
			for (const auto& a : g_auras) {
				if (a.root || a.fear || (a.sleep && a.wakes) || a.calm) {
					if (auto* actor = a.target.get().get()) held.insert(actor);
				}
			}
			std::scoped_lock lock(g_hurtMutex);
			std::erase_if(g_hurt, [&](const auto& h) { return !held.contains(h.first); });
			for (auto* actor : held) g_hurt.try_emplace(actor);
			g_holding = !g_hurt.empty();
		}

		void ApplyHealth(RE::Actor* a_target, const std::string& a_kind, int a_amount, std::uint32_t a_spell, int a_school = -1)
		{
			if (!a_target || a_amount <= 0) {
				return;
			}
			if (a_kind == "damage") {
				DamageSchool school(a_school >= 0 ? 1u << std::clamp(a_school, 0, 6) : 0);
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

		std::uint32_t NativeSchool(const RE::EffectSetting* base)
		{
			const auto resist = base ? base->data.resistVariable : RE::ActorValue::kNone;
			return resist == RE::ActorValue::kResistFire ? 4u : resist == RE::ActorValue::kResistFrost ? 16u :
				resist == RE::ActorValue::kResistShock || resist == RE::ActorValue::kPoisonResist ? 8u :
				resist == RE::ActorValue::kResistMagic ? 96u : 1u;
		}

		void PurgeImmunity(RE::Actor* who, std::uint32_t schools)
		{
			if (!who) return;
			std::vector<RE::ActiveEffect*> remove;
			if (auto* effects = who->GetActiveEffectList()) for (auto* effect : *effects) {
				const auto* base = effect ? effect->GetBaseObject() : nullptr;
				// A helpful WoW aura can have a negative native modifier (Divine Shield's attack speed).
				const bool helpful = effect && effect->spell && std::ranges::any_of(g_auras, [&](const Aura& aura) {
					auto* actor = aura.target ? aura.target.get().get() : static_cast<RE::Actor*>(Player());
					return actor == who && !aura.harmful && aura.end > Frame::Now() && (Carriers::IsBuff(effect->spell->GetFormID(), aura.spell) || (aura.unattackable && Carriers::IsSleep(effect->spell->GetFormID()))); });
				if (base && base->IsDetrimental() && !effect->flags.any(RE::ActiveEffect::Flag::kDispelled) &&
					!helpful && (NativeSchool(base) & schools)) remove.push_back(effect);
			}
			for (auto* effect : remove) effect->Dispel(true);
			for (auto& aura : g_auras) {
				auto* actor = aura.target ? aura.target.get().get() : static_cast<RE::Actor*>(Player());
				if (actor == who && aura.harmful && aura.spell != 25771 && (schools & (1u << std::clamp(aura.school, 0, 6)))) aura.end = Frame::Now();
			}
		}

		// Skyrim has one native combat target instead of a suppressible threat list.
		void SuppressImmunityThreat()
		{
			if (!g_paladin) return;
			for (const auto& aura : g_auras) if ((aura.immunity || aura.unattackable) && aura.end > Frame::Now()) {
				auto* who = aura.target ? aura.target.get().get() : static_cast<RE::Actor*>(Player());
				if (!who) continue;
				if (aura.unattackable) { who->StopCombat(); who->InterruptCast(false); }
				if (aura.purges) PurgeImmunity(who, aura.immunity);
				for (const auto& h : RE::ProcessLists::GetSingleton()->highActorHandles) {
					auto attacker = h.get();
					if (!attacker || attacker->IsDead() || Victim(attacker.get()) != who) continue;
					RE::Actor* next = nullptr;
					for (const auto& member : Party(who->GetPosition(), 30 * kUnitsPerYard)) {
						auto candidate = member.get();
						auto* actor = member ? candidate.get() : static_cast<RE::Actor*>(Player());
						if (actor && actor != who && !actor->IsDead() && !Unattackable(actor) && !ImmuneTo(actor, 1) &&
							(!next || actor->GetPosition().GetDistance(attacker->GetPosition()) < next->GetPosition().GetDistance(attacker->GetPosition()))) next = actor;
					}
					// A suppressed threat reference is still chosen when it is the only one (ThreatManager's
					// selectNextVictim): only Divine Intervention's unattackable target sends its attackers away.
					if (next) Retarget(attacker.get(), next); else if (aura.unattackable) attacker->StopCombat();
				}
			}
		}

		// One spell's auras on one unit (an empty handle: the player), as the service describes them: periodic ticks, a root,
		// Skyrim magic effects (buffs, Faerie Fire), a damage shield, sleep, pushback immunity, a periodic dispel.
		// Their form's aura and those that end with it (Prowl, Dash, Tiger's Fury): taken off as the player shifts.
		void RemoveFormAuras(bool a_withForm)
		{
			const auto before = g_auras.size();
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
			std::string kept;   // which auras a form change leaves on the player, and their form marks
			for (const auto& x : g_auras) if (!x.target) kept += std::format("{}{}{} ", x.spell, x.endsWithForm ? "e" : "", x.form ? "f" : "");
			logger::info("form auras: {} removed ({}), kept: {}", before - g_auras.size(), a_withForm ? "form change" : "form kept", kept);
		}

		void AddAura(std::uint32_t a_spell, const json& a_outcome, RE::ActorHandle a_on)
		{
			// Divine Intervention triggers 19753 on its recipient, independently of the caster's death.
			if (Character::Get().classId == 2 && a_spell == 19752) a_spell = 19753;
			if (Character::Get().classId == 2) {
				auto parent = a_outcome;
				bool split = false;
				parent["auras"] = json::array();
				for (const auto& a : a_outcome.value("auras", json::array())) {
					if (a.value("precast", false) && a.value("spell", a_spell) != a_spell) {
						AddAura(a.at("spell"), {{"name", "Forbearance"}, {"school", 1}, {"auras", json::array({a})}}, a_on);
						split = true;
					} else parent["auras"].push_back(a);
				}
				if (split) { AddAura(a_spell, parent, a_on); return; }
			}
			auto auras = a_outcome.value("auras", json::array());
			auto* recipient = a_on ? a_on.get().get() : static_cast<RE::Actor*>(Player());
			if (MovementImmune(recipient)) std::erase_if(auras.get_ref<json::array_t&>(), [](const json& a) {
				return a.value("type", "") == "root" || (a.value("type", "") == "movement_speed" && a.value("amount", 0.f) < 0);
			});
			if (Character::Get().classId == 2 && auras.empty()) return;
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
			aura.serial = ++g_auraSerial;
			aura.effects = auras;
			aura.partySource = Character::Get().classId == 2 && !a_on && a_outcome.contains("party_aura");
			aura.partyMember = a_outcome.value("party_member", false);
			aura.areaOwner = a_outcome.value("area_owner", std::uint64_t{0});
			aura.name = a_outcome.value("name", "");
			aura.stack = a_outcome.value("stack", "");
			if (Character::Get().classId == 2 && (aura.stack == "Seal" || aura.stack == "Judgement")) g_paladinManaSources.insert(a_spell);
			aura.stackAmounts = a_outcome.value("stack_amounts", std::map<std::string, float>{});
			aura.stackRule = a_outcome.value("stack_rule", "");
			aura.tooltip = a_outcome.value("tooltip", json());
			if (!aura.stack.empty() && PartyWide(a_outcome) && std::ranges::any_of(g_auras, [&](const Aura& x) {
					return x.target == a_on && x.end > Frame::Now() && SameStack(x, aura.stack, aura.stackAmounts) && Stronger(x, aura.stackAmounts); })) {
				return;   // this member keeps its stronger one
			}
			aura.target = a_on;
			for (const auto& event : a_outcome.value("events",json::array())) {
				if (event.value("type", "") == "interrupt" && event.value("school_lockout_ms",0) > 0) aura.lockout = true;
				if (event.value("type", "") == "taunt" && event.value("match_top_threat", false)) aura.tauntHolds = true;   // Growl
			}
			aura.school = a_outcome.value("school", 0);
			aura.harmful = a_spell != 19753 && (WowUi::Facts(a_spell).harmful || (Character::Get().classId == 2 && (a_spell == 25771 || aura.stack == "Judgement" ||
				std::ranges::any_of(auras, [](const json& a) { return a.value("debuff", false) || a.value("type", "") == "stun" || a.value("type", "") == "fear"; }))));
			aura.start = Frame::Now();
			aura.threat = a_outcome.value("threat_multiplier", 1.0f);
			for (const auto& a : auras) {
				const auto type = a.value("type", "");
				if (Character::Get().classId == 2) {
					if (type == "threat" && (a.value("school_mask", 0u) & 2)) aura.holyThreat = a.value("multiplier", 1.f);
					if (type == "school_immunity") {
						aura.immunity |= a.value("school_mask", 0u);
						// SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT: Divine Intervention's immunity purges nothing.
						aura.purges = aura.purges || a.value("purges_existing", true);
					}
					aura.pacify = aura.pacify || type == "pacify";
					aura.unattackable = aura.unattackable || type == "unattackable";
					if (type == "mechanic_immunity" && (a.value("mechanic", 0) == 7 || a.value("mechanic", 0) == 11)) aura.freedom = true;
					aura.charges = std::max(aura.charges, a.value("charges", 0));
				}
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
				} else if (type == "prevent_fleeing" && Character::Get().classId == 2 && target) {
					// Skyrim's Confidence 4 is foolhardy: it never chooses native fleeing.
					aura.skyrim.push_back({0, std::max(0.f, 4.f - target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kConfidence)), RE::ActorValue::kConfidence});
				} else if (type == "movement_speed") {   // Dash, Prowl
					aura.skyrim.push_back({ 0, a.value("amount", 0.0f), RE::ActorValue::kSpeedMult });
				} else if (type == "melee_haste") {
					const float amount = a.value("amount", 0.f);
					const float speed = amount >= 0 ? amount / 100.f : 1.f / (1.f - amount / 100.f) - 1.f;
					aura.skyrim.push_back({0, speed, RE::ActorValue::kWeaponSpeedMult});
					if (!a.value("main_hand_only", false)) aura.skyrim.push_back({0, speed, RE::ActorValue::kLeftWeaponSpeedMultiply});
				} else if (type == "ranged_haste") {
					aura.skyrim.push_back({0, a.value("amount", 0.f) / 100.f, RE::ActorValue::kBowSpeedBonus});
				} else if (type == "block_value") {
					aura.skyrim.push_back({0, a.value("amount", 0.f), RE::ActorValue::kBlockModifier});
				} else if (type == "ranged_attack_power") {
					aura.skyrim.push_back({0, a.value("amount", 0.f) * .1f, RE::ActorValue::kMarksmanModifier});
				} else if (Character::Get().classId == 2 && (type == "stat_percent" || type == "damage_done_percent")) {
					// No native conversion for Vindication/Vengeance in skyrim_balance.py: retain the WoW aura
					// for display and caster/target requests. Kings still takes its supplied skyrim effects below.
				} else if (type == "armor_from_items_percent" && Character::Get().classId == 2) {
					// As Enrage's carrier: their worn armor value, or an NPC's worn base ratings (Skyrim has no NPC GetArmorValue).
					auto* wearer = target ? target : static_cast<RE::Actor*>(Player());
					float worn = 0;
					for (const auto& [form, data] : wearer->GetInventory([](RE::TESBoundObject& item) { return item.IsArmor(); })) {
						if (data.second && data.second->IsWorn()) worn += wearer == Player() ? Player()->GetArmorValue(data.second.get()) : form->As<RE::TESObjectARMO>()->GetArmorRating();
					}
					aura.skyrim.push_back({0, worn * a.value("amount", 0.f) / 100.f, RE::ActorValue::kDamageResist});
				} else if (type == "armor_percent") {   // Enrage: a share of their worn armor
					aura.skyrim.push_back({ 0, Forms::WornArmor() * a.value("amount", 0.0f) / 100.0f, RE::ActorValue::kDamageResist });
				} else if (type == "attack_power" && (Character::Get().classId != 2 || a.value("skyrim", json::array()).empty())) {   // Demoralizing Roar: WoW's attack power off, as Skyrim's damage
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
					aura.kinds.push_back(type == "power_restore" ? "power_" + a.value("power", std::string("mana")) : type);
				}
				for (const auto& e : a.value("skyrim", json::array())) {
					aura.skyrim.push_back({ e.value("mgef", RE::FormID{ 0 }), e.value("magnitude", 0.0f) });
				}
				if (type == "root" || type == "fear") {
					aura.root = type == "root";
					aura.fear = type == "fear";
					aura.controlEnd = aura.start + a.value("duration_ms",0) / 1000.0;
					if (const auto& hb = a.value("heartbeat", json::object()); hb.contains("breaks_at_ms") && hb["breaks_at_ms"].is_number()) {
						aura.breakAt = aura.start + hb["breaks_at_ms"].get<double>() / 1000.0;
					}
					aura.victimLevel = a.value("damage_break", json::object()).value("victim_level", 0);
					if (const auto one = a.value("single_target", json()); one.is_object()) {
						aura.single = Aura::Single{ one.value("chain", 0u), one.value("family", 0u), one.value("flags", std::uint64_t{ 0 }) };
					}
				} else if (type == "stun") {
					aura.sleep = true;
					aura.controlEnd = aura.start + a.value("duration_ms",0) / 1000.0;
					aura.wakes = a.value("breaks_on_damage", false);
					if (const auto& hb = a.value("heartbeat", json::object()); hb.contains("breaks_at_ms") && hb["breaks_at_ms"].is_number()) {
						aura.breakAt = aura.start + hb["breaks_at_ms"].get<double>() / 1000.0;
					}
					if (const auto one = a.value("single_target", json()); one.is_object()) {
						aura.single = Aura::Single{ one.value("chain", 0u), one.value("family", 0u), one.value("flags", std::uint64_t{ 0 }) };
					}
				} else if (type == "detect_range" && a.value("amount", 0) < 0) {
					aura.calm = true;
				} else if (type == "damage_shield") {
					aura.shield = a.value("amount", 0);
				} else if (type == "pushback_immunity") {
					if (Character::Get().classId == 2) aura.pushbackResist += a.value("amount", 100.f);
					else aura.noPushback = true;
				} else if (type == "taunt") {
					aura.taunt = true;
				} else if (type == "periodic_dispel") {
					aura.dispel = a.value("dispel", "");
					aura.dispelCount = a.value("count", 1);
					aura.dispelEvery = std::max(0.1, a.value("amplitude_ms", 2000) / 1000.0);
					aura.nextDispel = aura.start + aura.dispelEvery;
				}
			}
			aura.duration = aura.end - aura.start;
			// Ticks of all the spell's effects in time order; a new cast replaces the old one (same caster).
			std::vector<std::size_t> order(aura.ticks.size());
			std::iota(order.begin(), order.end(), 0);
			std::ranges::stable_sort(order, {}, [&](std::size_t i) { return aura.ticks[i].first; });
			Aura sorted = aura;
			for (std::size_t i = 0; i < order.size(); ++i) {
				sorted.ticks[i] = aura.ticks[order[i]], sorted.kinds[i] = aura.kinds[order[i]];
			}
			RE::ActorHandle tauntedFrom;   // a renewed taunt keeps whom its first application took it from
			std::erase_if(g_auras, [&](const Aura& x) {
				const bool same = x.target == sorted.target &&
					(x.spell == a_spell || (!sorted.name.empty() && x.name == sorted.name) || SameStack(x, sorted.stack, sorted.stackAmounts) ||
						(sorted.stackRule == "unique_per_caster" && !sorted.stack.empty() && x.stack == sorted.stack) ||
						(sorted.trackingAura && x.trackingAura) ||
						(sorted.reveal && x.reveal && sorted.armorReduction > 0 && x.armorReduction > 0)); // caster and Feral Faerie Fire share one debuff
				if (same) {
					if (x.taunt && x.tauntedFrom) tauntedFrom = x.tauntedFrom;
					if (!x.target) { std::scoped_lock lock(g_absorbMutex); g_absorbs.erase(x.spell); }
					SpellVisuals::Stop(x.state);
					if (x.spell != a_spell) {
						auto* actor = x.target ? x.target.get().get() : static_cast<RE::Actor*>(Player());
						if (actor) {
							Carriers::Unbuff(actor, x.spell);
							if (x.root) Carriers::Unroot(actor);
							if (x.sleep) Carriers::Wake(actor);
							if (x.calm) Carriers::Uncalm(actor);
							if (x.fear) Carriers::Unfear(actor);
						}
						Lua(std::format("__WBAuraRemove({}, {})", onSelf ? std::string("\"player\"") : std::to_string(target->GetFormID()), x.spell));
					}
				}
				return same;
			});
			if (onSelf) for (const auto& a : auras) if (a.value("type", "") == "absorb_shield") {
				std::scoped_lock lock(g_absorbMutex);
				g_absorbs[a_spell] = {a.value("amount", 0.f), a.value("school_mask", 0u), sorted.end};
			}
			// Unit::AddSpellAuraHolder: a single-target spell of theirs (any rank) leaves the unit it held before.
			if (sorted.single) {
				for (auto& x : g_auras) {
					if (x.single && x.target != a_on && x.single->Matches(*sorted.single)) x.end = sorted.start;
				}
			}
			RE::Actor* who = target ? target : static_cast<RE::Actor*>(Player());
			const auto whoArg = onSelf ? std::string("\"player\"") : std::to_string(target->GetFormID());
			const double shown = sorted.end > 1e11 ? 0.0 : sorted.end - sorted.start;   // 0: until cancelled (a form)
			Lua(std::format("__WBAuraAdd({}, {}, {}, {:.3f}, {})", whoArg, a_spell, sorted.harmful ? "true" : "false", shown, TooltipLua(sorted.tooltip)));
			if (onSelf) {
				Log("CHAT_MSG_SPELL_PERIODIC_SELF_BUFFS", "AURAADDEDSELFHELPFUL", sorted.name);
			} else if (sorted.harmful) {
				Log("CHAT_MSG_SPELL_PERIODIC_CREATURE_DAMAGE", "AURAADDEDOTHERHARMFUL", Name(target), sorted.name);
				Carriers::Provoke(target);   // Faerie Fire, Roots, Hibernate: harmful without a point of damage
			} else {
				Log("CHAT_MSG_SPELL_PERIODIC_FRIENDLYPLAYER_BUFFS", "AURAADDEDOTHERHELPFUL", Name(target), sorted.name);
			}
			const auto seconds = static_cast<float>(sorted.end - sorted.start);
			if (sorted.root && target) {
				Carriers::Root(target, static_cast<float>(sorted.controlEnd - sorted.start));
				sorted.rootAt = target->GetPosition();
			}
			if (sorted.fear && target) Carriers::Fear(target);
			if (sorted.sleep && target) {
				Carriers::Sleep(target, static_cast<float>(sorted.controlEnd - sorted.start));
			}
			sorted.calm = sorted.calm && target && !target->IsInCombat();   // the user's rule: before combat only
			if (sorted.calm) {
				Carriers::Calm(target);
			}
			// TauntUpdate: the taunter heads its threat list for as long as the aura lasts (TickAuras holds it there).
			sorted.taunt = sorted.taunt && target && !target->IsPlayerTeammate();
			if (sorted.taunt) {
				auto* before = Victim(target);
				sorted.tauntedFrom = before && before != Player() ? before->CreateRefHandle() : tauntedFrom;
				if (sorted.tauntHolds) for (auto& x : g_auras) if (x.taunt && x.target == sorted.target) x.tauntedFrom = {};   // their threat now tops its list
				if (Retarget(target, Player())) g_talentEvidence["taunts"] = g_talentEvidence.value("taunts", 0) + 1;
				sorted.tauntAt = sorted.start + .25;
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
			const auto freedom = sorted.freedom;
			const auto immunity = sorted.purges ? sorted.immunity : 0u;
			const auto pacify = sorted.pacify;
			g_auras.push_back(std::move(sorted));
			if (freedom) {
				Carriers::Unsnare(who, true);
				for (auto& a : g_auras) if (a.target == a_on && a.spell != a_spell && a.end > Frame::Now() &&
					(a.root || std::ranges::any_of(a.effects, [](const json& e) { return e.value("type", "") == "movement_speed" && e.value("amount", 0.f) < 0; }))) {
					if (a.root) Carriers::Unroot(who);
					a.root = false; a.end = Frame::Now();
					Carriers::Unbuff(who, a.spell);
				}
			}
			if (immunity) PurgeImmunity(who, immunity);
			if (pacify && onSelf) Melee::Stop();
			PublishImmunities();
			if (immunity) SuppressImmunityThreat();
		}

		void PollPartyAura()
		{
			if (Character::Get().classId != 2) return;
			const auto now = Frame::Now();
			const auto source = std::ranges::find_if(g_auras, [now](const Aura& a) {
				return !a.target && a.partySource && a.end > now;
			});
			const auto spell = source == g_auras.end() ? 0u : source->spell;
			const auto serial = source == g_auras.end() ? 0 : source->serial;
			std::map<RE::FormID, RE::ActorHandle> handles;
			for (std::size_t i = 0; i < 4; ++i) if (auto* actor = Bridge::PartyMember(i)) handles[actor->GetFormID()] = actor->CreateRefHandle();
			for (auto& a : g_auras) if (a.partyMember && a.end > now) {
				auto actor = a.target.get();
				if (!spell || a.areaOwner != serial || Player()->IsDead() || !actor || !handles.contains(actor->GetFormID())) a.end = now;
			}
			if (g_partyPollBusy && now >= g_partyPollDeadline) {
				g_partyPollBusy = false; ++g_partyPollSerial;
				g_talentEvidence["party_poll_timeouts"] = g_talentEvidence.value("party_poll_timeouts", 0) + 1;
				logger::warn("cast: party aura request timed out; polling again");
			}
			if (!spell || Player()->IsDead() || g_partyPollBusy || now < g_partyPollAt) return;
			g_partyPollAt = now + 1; g_partyPollBusy = true; g_partyPollDeadline = now + 5;
			const auto poll = ++g_partyPollSerial;
			json members = json::array();
			for (const auto& [id, handle] : handles) if (auto actor = handle.get()) members.push_back({
				{"id", id}, {"level", actor->GetLevel()}, {"alive", !actor->IsDead() && !actor->IsDisabled()},
				{"party", actor->IsPlayerTeammate()}, {"friendly", Bridge::Healable(actor.get())},
				{"distance_yd", actor->GetPosition().GetDistance(Player()->GetPosition()) / kUnitsPerYard}});
			Service::Request({{"op", "party_aura"}, {"spell", spell}, {"caster", CasterNow()}, {"members", members}},
				[epoch = g_epoch, spell, serial, handles, poll](const json& out) {
					if (epoch != g_epoch || poll != g_partyPollSerial) return;
					g_partyPollBusy = false;
					if (!out.value("ok", false) || Character::Get().classId != 2 || Player()->IsDead() ||
						!std::ranges::any_of(g_auras, [serial](const Aura& a) { return !a.target && a.serial == serial && a.end > Frame::Now(); })) return;
					g_talentEvidence["party_aura"] = out;
					for (const auto& row : out.value("members", json::array())) {
						const auto it = handles.find(row.value("id", 0u));
						auto actor = it == handles.end() ? RE::NiPointer<RE::Actor>{} : it->second.get();
						if (!actor) continue;
						bool current = false;
						for (std::size_t i = 0; i < 4; ++i) if (Bridge::PartyMember(i) == actor.get()) current = true;
						const bool covered = row.value("in_aura", false) && current && !actor->IsDead() && !actor->IsDisabled();
						const auto old = std::ranges::find_if(g_auras, [&](const Aura& a) {
							return a.partyMember && a.areaOwner == serial && a.target == it->second && a.end > Frame::Now(); });
						if (!covered) { if (old != g_auras.end()) old->end = Frame::Now(); }
						else if (old == g_auras.end()) AddAura(spell, {{"name", out.value("name", "")}, {"school", 1},
							{"stack", "Aura"}, {"stack_rule", "unique_per_caster"}, {"party_member", true}, {"area_owner", serial},
							{"auras", out.value("auras", json::array())}}, it->second);
					}
				});
		}

		// Rebirth's target is dead, as it must be.
		bool Raises(const json& a_outcome)
		{
			return std::ranges::any_of(a_outcome.value("events", json::array()), [](const json& e) { return e.value("type", "") == "resurrect"; });
		}

		void StartGroundArea(std::uint32_t spell, const json& out)
		{
			const auto timing = ChannelTiming::From(out);
			if (!timing) return;
			for (auto it = g_areas.begin(); it != g_areas.end();) {
				if (it->spell == spell) { SpellVisuals::Stop(it->area); it = g_areas.erase(it); } else ++it;
			}
			Channel area;
			area.spell = spell; area.serial = ++g_areaSerial; g_groundSerial[spell] = area.serial;
			area.name = out.value("name", "Consecration"); area.school = out.value("school", 1);
			area.at = Player()->GetPosition(); area.radius = timing->radius * kUnitsPerYard;
			area.start = Frame::Now(); area.end = area.start + timing->duration;
			area.amplitude = timing->period; area.nextTick = area.start + area.amplitude;
			area.area = SpellVisuals::Area(spell, area.at, timing->radius);
			g_areas.push_back(std::move(area));
		}

		void HolyWrathImpact(const InFlight& cast);

		// A cast's auras: on its target, or (a party aura: Gift of the Wild) on their party around the player.
		void AddAuras(const InFlight& a_cast)
		{
			const auto& out = a_cast.outcome;
			const auto  auras = out.value("auras", json::array());
			if (auras.empty()) {
				return;
			}
			if (Character::Get().classId == 2 && std::ranges::any_of(auras, [](const json& a) {
				return a.value("area", json::object()).value("anchor", "") == "caster_position"; })) {
				StartGroundArea(a_cast.spell, out);
				return;
			}
			if (a_cast.spell == 20549) {
				// Select the area at impact, after the cast completes; roll each of at most five enemies separately.
				auto targets = json::array();
				std::map<RE::FormID, RE::ActorHandle> handles;
				const auto area = auras[0].value("area", json::object());
				for (const auto& h : Hostiles(Player()->GetPosition(), area.value("radius_yd",8.f) * kUnitsPerYard, 64)) {
					if (auto actor = h.get()) {
						bool los = true;
						if (!Player()->HasLineOfSight(actor.get(),los)) continue;
						json victim = {{"id",actor->GetFormID()},{"level",actor->GetLevel()},{"is_player",false},
							{"distance_yd",Player()->GetPosition().GetDistance(actor->GetPosition()) / kUnitsPerYard}};
						AugmentTarget(victim,actor.get());
						targets.push_back(std::move(victim)); handles.emplace(actor->GetFormID(),h);
						if (targets.size() >= area.value("max_targets",5u)) break;
					}
				}
				if (!targets.empty()) RequestOutcome({{"op","resolve"},{"spell",20549},{"caster",CasterNow()},
					{"target",{{"kind","self"}}},{"targets",targets},{"seed",std::uniform_int_distribution<std::uint32_t>()(g_rng)}},
					[handles = std::move(handles), epoch = g_epoch](const json& result) {
						if (epoch != g_epoch || Character::Looks().race != 6 || !result.value("ok",false)) return;
						for (const auto& hit : result.value("results",json::array())) {
							const auto it = handles.find(hit.value("id",0u));
							auto actor = it == handles.end() ? RE::NiPointer<RE::Actor>{} : it->second.get();
							if (!actor || actor->IsDead() || actor->IsDisabled()) continue;
							Carriers::Provoke(actor.get());
							AddAura(20549, {{"name",result.value("name","War Stomp")},{"auras",hit.value("auras",json::array())}},it->second);
						}
					});
				return;
			}
			if (Character::Get().classId == 2 && out.contains("party_aura")) {
				AddAura(a_cast.spell, out, {}); g_partyPollAt = 0; return;
			}
			std::vector<RE::ActorHandle> on{ a_cast.target };
			if (Character::Get().classId == 2 && out.contains("greater_blessing") &&
				out.at("greater_blessing").value("applies_to", "") == "party") {
				// D3: the player and their followers count as one class. Keep the selected target even outside the class radius.
				for (const auto& h : Party(Player()->GetPosition(), out.at("greater_blessing").value("class_radius_yd", 0.f) * kUnitsPerYard)) {
					if (std::ranges::find(on, h) == on.end()) on.push_back(h);
				}
			}
			for (const auto& a : auras) {
				if (const auto area = a.value("area", json::object()); area.value("party", false) && !area.value("channel", false)) {
					on = Party(Player()->GetPosition(), area.value("radius_yd", 0.0f) * kUnitsPerYard);
					break;
				} else if (area.value("hostile", false)) {   // Demoralizing Roar: the enemies around the player
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
			if (Character::Get().classId == 2 && (a_cast.spell == 2812 || a_cast.spell == 10318) && !out.contains("results")) {
				HolyWrathImpact(a_cast); return;
			}
			auto*             target = a_cast.target.get().get();
			if (a_cast.target && (!target || target->IsDisabled() || target->IsDeleted() || target->IsDead() != Raises(out))) {
				return;   // removed or dead while the missile flew (or, for Rebirth, alive again)
			}
			RE::Actor*        who = target ? target : static_cast<RE::Actor*>(Player());
			if (target && Unattackable(target) && WowUi::Facts(a_cast.spell).harmful) return;
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
				const int  amount = e.contains("caster_max_health") && Character::Get().classId == 2 ?
					static_cast<int>(MaximumHealth(Player()) * e.value("caster_max_health", 1.f)) : e.value("amount", 0);
				const bool crit = e.value("crit", false);
				if (type == "damage") {
					ApplyHealth(who, "damage", amount, a_cast.spell, out.value("school", 0));
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
                    Forms::Apply(out, nullptr); // a missed attack still ends Prowl
					Carriers::Provoke(who);     // and is still an attack, as a resisted spell still draws a WoW mob
					return;
				} else if (type == "resist") {
					Log("CHAT_MSG_SPELL_SELF_DAMAGE", "SPELLRESISTSELFOTHER", spellName, Name(who));
					Carriers::Provoke(who);
					return;
				} else if (type == "absorb") {
                    who->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kMagicka,e.value("mana",0.f));
                    Lua(std::format("__WBFire('CHAT_MSG_SPELL_SELF_DAMAGE',{})",LuaQuote(std::format("{} absorbs {}.",Name(who),spellName))));
                    Carriers::Provoke(who);
                    return;
				} else if (type == "dodge" || type == "parry" || type == "block") {
					Log("CHAT_MSG_SPELL_SELF_DAMAGE", type == "dodge" ? "SPELLDODGEDSELFOTHER" : type == "parry" ? "SPELLPARRIEDSELFOTHER" : "SPELLBLOCKEDSELFOTHER",
						spellName, Name(who));
					Forms::Apply(out, nullptr);   // a refund, a broken prowl
					Carriers::Provoke(who);
					return;
				} else if (type == "dispel") {
					Dispel(who, e.value("dispel", ""), e.value("count", 1));
				} else if (type == "power" && e.value("power", "") == "mana" && (out.value("item_use", false) || Character::Get().classId == 2)) {
					who->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kMagicka, e.value("amount", 0.f));
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
				} else if (Character::Get().classId == 2 && type == "sanctuary") {
					who->StopCombat(); who->InterruptCast(false);
					for (const auto& h : RE::ProcessLists::GetSingleton()->highActorHandles) if (auto actor = h.get()) {
						bool aimed = Victim(actor.get()) == who;
						for (const auto source : {RE::MagicSystem::CastingSource::kLeftHand, RE::MagicSystem::CastingSource::kRightHand, RE::MagicSystem::CastingSource::kOther}) {
							auto* caster = actor->GetMagicCaster(source);
							if (caster && caster->desiredTarget.get().get() == who) aimed = true;
						}
						if (aimed) actor->InterruptCast(false);
					}
					g_talentEvidence["sanctuary_target"] = who->GetFormID();
				} else if (Character::Get().classId == 2 && type == "instakill") {
					auto* killed = e.value("target", "caster") == "caster" ? static_cast<RE::Actor*>(Player()) : who;
					killed->KillImpl(nullptr, MaximumHealth(killed), true, true);
					g_talentEvidence["instakill_target"] = killed->GetFormID();
				} else if (type == "interrupt" && target) {
					target->InterruptCast(false);
				} else if (type == "threat" && target && amount < 0) {
					Cower(target);
				}
			}
			for (const auto& hit : out.value("results", json::array())) {   // Swipe: each of the enemies it struck
				auto* struck = RE::TESForm::LookupByID<RE::Actor>(hit.value("id", RE::FormID{ 0 }));
				if (!struck || struck->IsDisabled() || struck->IsDeleted() || struck->IsDead() || hit.value("type", "") != "damage") {
					continue;
				}
				const int amount = hit.value("amount", 0);
				ApplyHealth(struck, "damage", amount, a_cast.spell, out.value("school", 0));
				if (amount > 0) Forms::AttackThreat(out, struck);
				Log("CHAT_MSG_SPELL_SELF_DAMAGE", hit.value("crit", false) ? "SPELLLOGCRITSELFOTHER" : "SPELLLOGSELFOTHER", spellName, Name(struck), amount);
			}
			Forms::Apply(out, target);
			if (Shapeshifts(out)) FreeMovement();
			if (!out.value("events", json::array()).empty() || !out.value("auras", json::array()).empty()) {
				const auto at = who->GetPosition();
				WowUi::PlaySound(WowUi::Sounds(a_cast.spell).impact, &at);
				SpellVisuals::Play(a_cast.spell, SpellVisuals::Kit::Impact, a_cast.target, false);
			}
			AddAuras(a_cast);
		}

		void HolyWrathImpact(const InFlight& cast)
		{
			json targets = json::array();
			const float radius = cast.outcome.value("area", json::object()).value("radius_yd", 20.f);
			for (const auto& h : Hostiles(Player()->GetPosition(), radius * kUnitsPerYard, 64)) {
				auto actor = h.get();
				bool los = true;
				if (!actor || !Player()->HasLineOfSight(actor.get(), los)) continue;
				json victim = {{"id", actor->GetFormID()}, {"kind", "unit"}, {"level", actor->GetLevel()},
					{"reaction", Reaction(actor.get())}, {"alive", true}, {"attackable", true}, {"los", true},
					{"distance_yd", Player()->GetPosition().GetDistance(actor->GetPosition()) / kUnitsPerYard}};
				AugmentTarget(victim, actor.get()); targets.push_back(std::move(victim));
			}
			g_talentEvidence["holy_wrath_targets"] = targets;
			RequestOutcome({{"op", "resolve"}, {"spell", cast.spell}, {"caster", CasterNow()}, {"target", {{"kind", "self"}}},
				{"targets", targets}, {"seed", std::uniform_int_distribution<std::uint32_t>()(g_rng)}},
				[spell = cast.spell, epoch = g_epoch](const json& out) {
					if (epoch != g_epoch || Character::Get().classId != 2 || Player()->IsDead() || !out.value("ok", false)) return;
					auto result = out;
					if (!result.contains("results")) result["results"] = json::array();
					g_talentEvidence["holy_wrath_results"] = result["results"];
					Land({spell, result, {}});
				});
		}

		// The player moves of their own accord - a movement key, auto-run, a jump or a fall - as the client's movement flags say;
		// turning on the spot does not count, nor does a blow nudging the player (Skyrim's hits shift the player a few units, and
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
			if (Character::Get().classId == 2) for (const auto& sid : a_outcome.value("category_spells", json::array())) {
				const int ms = a_outcome.value("category_cooldown_ms", 0);
				if (ms > 0) {
					const auto id = sid.get<std::uint32_t>();
					g_cooldowns[id] = std::max(g_cooldowns[id], Frame::Now() + ms / 1000.0);
					Lua(std::format("__WBCooldown({}, {:.3f})", id, (g_cooldowns[id] - Frame::Now())));
				}
			}
			if (const int cd = a_outcome.value("cooldown_ms", 0); cd > 0) {
				g_cooldowns[a_spell] = Frame::Now() + cd / 1000.0;
				Lua(std::format("__WBCooldown({}, {:.3f})", a_spell, cd / 1000.0));
			}
		}

		// An item use's timers (Player::AddCooldown with the item): the spell timer is kept under the item, as it blocks
		// only that item (WorldObject::IsSpellReady compares the item ID), the category timer under its category.
		// SPELL_ATTR_COOLDOWN_ON_EVENT holds both until their aura of the spell fades (Spell::SendSpellCooldown holds them,
		// SpellAuraHolder's removal starts them).
		constexpr double kHeld = 1e12;
		std::map<std::uint32_t, std::map<std::uint32_t, int>> g_held;   // spell -> timer key -> ms from the aura's end

		bool ItemUseReady(std::uint32_t a_entry, const json& a_cooldowns)
		{
			return ItemReady(kItemTimer | a_entry) && std::ranges::all_of(a_cooldowns, [](const json& cd) {
				const auto category = cd.value("ready_category", 0u);   // the item's own category; zero checks none
				return !category || ItemReady(kItemCategory | category);
			});
		}

		void StartItemCooldowns(std::uint32_t a_entry, const json& a_cooldowns, bool a_aura)
		{
			for (const auto& cd : a_cooldowns) {
				std::map<std::uint32_t, int> timers{ { kItemTimer | a_entry, cd.value("cooldown_ms", 0) } };
				if (const auto category = cd.value("category", 0u); category) timers[kItemCategory | category] = cd.value("category_cooldown_ms", 0);
				std::erase_if(timers, [](const auto& timer) { return timer.second <= 0; });
				if (cd.value("on_event", false) && a_aura) {
					for (const auto& [key, ms] : timers) {
						g_cooldowns[key] = kHeld;
						Lua(std::format("__WBCooldown({}, {:.3f}, 1)", key, ms / 1000.0));
					}
					g_held[cd.value("spell", 0u)] = std::move(timers);
					continue;
				}
				for (const auto& [key, ms] : timers) {   // an item with two use spells keeps the longer timer
					if (const auto it = g_cooldowns.find(key); it == g_cooldowns.end() || it->second < Frame::Now() + ms / 1000.0)
						StartCooldown(key, { { "cooldown_ms", ms } });
				}
			}
		}

		void ReleaseHeld(std::uint32_t a_spell)
		{
			const auto held = g_held.find(a_spell);
			if (held == g_held.end()) return;
			for (const auto& [key, ms] : held->second) StartCooldown(key, { { "cooldown_ms", ms } });
			g_held.erase(held);
		}

		// A channelled area spell (Hurricane where it was placed; Tranquility around the player): ticks every amplitude on
		// whatever is inside its circle while the player stands still - enemies hurt, or their party healed.
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
			c.selfAura = a_r.value("self_channel", false);
			if (c.selfAura) {
				for (const auto& aura : a_r.at("auras"))
					c.end = std::max(c.end, c.start + aura.value("duration_ms", 0) / 1000.0);
			} else {
				const auto timing=ChannelTiming::From(a_r);
				if (!timing) { Lua("__WBCastFailed(\"SPELL_FAILED_ERROR\")"); return; }
				c.radius=timing->radius*kUnitsPerYard;
				c.end=c.start+timing->duration;
				c.amplitude=timing->period;
				c.heal=timing->heal;
			}
			c.nextTick = c.start + c.amplitude;
			c.interruptFlags = a_r.value("channel_interrupt_flags", 0u);
			c.pushbackResist = a_r.value("pushback_resist_pct", 0.0f);
			const auto kit = WowUi::Sounds(a_spell);
			c.loop = WowUi::PlaySound(kit.channel);
			Avatar::Act(kit.channelAnim, true);
			c.visual = SpellVisuals::Play(a_spell, SpellVisuals::Kit::Channel, {}, true);
			g_talentEvidence.erase("channel");
			g_talentEvidence["area_damage"] = json::array();
			if (!c.selfAura) c.area = SpellVisuals::Area(a_spell, c.at, c.radius / Avatar::kUnitsPerYard);
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
			if (const auto entry = out.value("item_entry", 0u); entry) {
				auto* form = Gear::FormOf(entry);
				if (!form || Player()->GetItemCount(form) <= 0) { fail("SPELL_FAILED_ITEM_NOT_READY"); return; }
				const auto caster = CasterNow();
				if (out.value("item_requires_equipped", false) && std::ranges::find(caster.at("items"), json(entry)) == caster.at("items").end()) {
					fail("SPELL_FAILED_ITEM_NOT_READY"); return;
				}
				if (!ItemUseReady(entry, out.at("item_cooldowns"))) { fail("ERR_ITEM_COOLDOWN"); return; }
			}
            if (Bounces(out, cast.target)) { fail("SPELL_FAILED_AURA_BOUNCED"); return; }   // rechecked at its end
            if ((cast.spell == 20549 && Character::Looks().race != 6) || (cast.spell == 20580 && Character::Looks().race != 4)) {
                fail("SPELL_FAILED_NOT_KNOWN"); return;
            }
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
                const float distance=Player()->GetPosition().GetDistance(target->GetPosition());
                if (distance<WowUi::Facts(cast.spell).rangeMin*kUnitsPerYard || (range>0 && distance>range*kUnitsPerYard)) {
                    fail("SPELL_FAILED_OUT_OF_RANGE"); return;
                }
                bool los=true;
                if (target!=Player() && !Player()->HasLineOfSight(target,los)) { fail("SPELL_FAILED_LINE_OF_SIGHT"); return; }
			}
			if (Character::Get().classId == 2) {
				auto* who = cast.target ? cast.target.get().get() : static_cast<RE::Actor*>(Player());
				if ((cast.spell == 24275 || cast.spell == 24274 || cast.spell == 24239) && who &&
					who->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) >= MaximumHealth(who) * .2f) {
					fail("SPELL_FAILED_TARGET_AURASTATE"); return;
				}
				if (who && cast.target && out.contains("creature_types") &&
					std::ranges::find(out.at("creature_types"), json(Bridge::CreatureTypeName(who))) == out.at("creature_types").end()) {
					fail("SPELL_FAILED_BAD_TARGETS"); return;
				}
				const bool protectedSpell = std::ranges::any_of(out.value("auras", json::array()), [](const json& a) {
					return a.value("precast", false) && a.value("spell", 0u) == 25771; });
				if (protectedSpell && std::ranges::any_of(g_auras, [&](const Aura& a) {
					return a.spell == 25771 && a.target == cast.target && a.end > Frame::Now(); })) {
					fail("SPELL_FAILED_TARGET_AURASTATE"); return;
				}
			}
			if (Character::Get().classId == 11 && (cast.spell == 2782 || cast.spell == 8946 || cast.spell == 2893)) {
				auto* who = cast.target ? cast.target.get().get() : static_cast<RE::Actor*>(Player());
				if (std::ranges::none_of(out.value("events", json::array()), [&](const json& e) {
					return e.value("type", "") == "dispel" && !DispelsOn(who, e.value("dispel", "")).empty();
				})) { fail("SPELL_FAILED_NOTHING_TO_DISPEL"); return; }
			}
            const auto power=out.value("power","mana");
            if (power=="mana" && out.value("cost",0.f)>Mana()) { fail("ERR_OUT_OF_MANA"); return; }
            if (!Forms::CanPay(out)) { fail(power=="rage" ? "ERR_OUT_OF_RAGE" : "ERR_OUT_OF_ENERGY"); return; }
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
			const auto kit = WowUi::Sounds(cast.spell);   // the visual kits: the cast's sound, their animation, its effects
			WowUi::PlaySound(kit.cast);
			Avatar::Act(kit.castAnim, false);
			SpellVisuals::Stop(cast.precast);
			// The cast kit's effects on their hands last until their cast animation lets the spell go - its $CSL, $CSR or
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
			if (const auto entry = out.value("item_entry", 0u); entry)
				StartItemCooldowns(entry, out.at("item_cooldowns"), !out.value("auras", json::array()).empty());
			if (out.value("self_channel", false)) {
				AddAuras(cast);
				StartChannel(cast.spell, out, Player()->GetPosition());
				return;
			}
			if (Channels(out)) {   // Tranquility: channelled around the player from here
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
			NativeDamage(now);
			for (auto it = g_auras.begin(); it != g_auras.end();) {
				auto&      a = *it;
				if (!a.target) {
					std::scoped_lock lock(g_absorbMutex);
					if (const auto shield = g_absorbs.find(a.spell); shield != g_absorbs.end() && shield->second.remaining <= 0) a.end = now;
				}
				auto*      target = a.target.get().get();
				const bool onPlayer = !a.target;
				const bool gone = onPlayer ? ((a.trackingAura || a.form) && Player()->IsDead()) :
					(!target || target->IsDead() || target->IsDisabled());
				if (!gone && now < a.end) {
					if (a.freedom) Carriers::Unsnare(onPlayer ? static_cast<RE::Actor*>(Player()) : target, true);
					if (a.reveal) Reveal(onPlayer ? static_cast<RE::Actor*>(Player()) : target);
					// Skyrim has no WoW school lockout. Prevent all native recasts
					// during Charge's four seconds, owned by its root aura.
					if (a.lockout && target) target->InterruptCast(false);
					// Taunted: SelectHostileTarget keeps the player whatever Skyrim's own target selection would prefer.
					if (a.taunt && target && target->Is3DLoaded() && now >= a.tauntAt && !Player()->IsDead()) {   // left behind a door: let it be
						a.tauntAt = now + .25;
						if (Victim(target) != Player() && Retarget(target, Player())) g_talentEvidence["taunt_holds"] = g_talentEvidence.value("taunt_holds", 0) + 1;
					}
				}
				if (!gone && a.calm && target && target->IsInCombat()) {   // hurt or provoked natively: soothing is over
					a.end = now;
				}
				if (!gone && ControlTiming::Expired(now, a.controlEnd)) {
					if (a.root && target) Carriers::Unroot(target);
					if (a.sleep && target) Carriers::Wake(target);
					if (a.fear && target) Carriers::Unfear(target);
					a.root = a.sleep = a.fear = a.lockout = false;
				}
				while (!gone && a.next < a.ticks.size() && a.start + a.ticks[a.next].first <= a.end + 0.001 &&
					now >= a.start + a.ticks[a.next].first) {
					const int   amount = a.ticks[a.next].second;
					const auto& kind = a.kinds[a.next];
					++a.next;
					RE::Actor* who = target ? target : static_cast<RE::Actor*>(Player());
					if (kind == "power_mana") {
						who->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kMagicka, static_cast<float>(amount));
					} else if (kind == "power_rage" || kind == "power_energy") {
						Forms::Apply({{"events", json::array({{{"type", "power"}, {"power", kind.substr(6)}, {"amount", amount}}})}}, nullptr);
					} else if (kind == "heal") {
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
						ApplyHealth(who, "damage", amount, a.spell, a.school);
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
				if ((a.root || a.sleep || a.fear) && a.breakAt >= 0 && now >= a.breakAt) {
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
					if (a.fear && there) Carriers::Unfear(target);
					if (a.calm && there) {
						Carriers::Uncalm(target);
					}
					// TauntFadeOut: its threat list chooses again. Growl's matched threat keeps the player on top (the 110% rule);
					// Challenging Roar raised none, so it returns to whom it attacked, unless another taunt still holds it.
					if (a.taunt && there && target->Is3DLoaded() && !a.tauntHolds && Victim(target) == Player() &&
						std::ranges::none_of(g_auras, [&](const Aura& x) { return &x != &a && x.taunt && x.target == a.target && now < x.end; })) {
						auto from = a.tauntedFrom.get();
						if (from && !from->IsDisabled() && !from->AsActorState()->IsBleedingOut() && Retarget(target, from.get())) g_talentEvidence["taunt_returns"] = g_talentEvidence.value("taunt_returns", 0) + 1;
					}
					if (!a.skyrim.empty() && (onPlayer || there || (a.partyMember && target))) {
						Carriers::Unbuff(onPlayer ? static_cast<RE::Actor*>(Player()) : target, a.spell);
					}
					leftForm = leftForm || (onPlayer && a.form);   // their form's buff taken off: the player leaves it
					if (onPlayer && a.stealth) {
						Forms::EndStealth(a.spell);
					}
					SpellVisuals::Stop(a.state);
					ReleaseHeld(a.spell);
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
				if (!Player()->IsDead()) FreeMovement();   // their form's aura removed frees the player as applying one does
			}
			Watch();
			PublishImmunities();
		}

	}

	namespace
	{
		// A blow lands (Skyrim's hit event, from anyone): Thorns hurts a melee attacker of the unit it is on. What a hit
		// does to their crowd control is its damage's (DamageBreak, through NpcHealthDamage), as Unit::DealDamage's is.
		void Struck(RE::Actor* a_victim, RE::Actor* a_attacker, RE::FormID a_source, RE::FormID a_projectile)
		{
			const auto* source = a_source ? RE::TESForm::LookupByID(a_source) : nullptr;
			auto*       weapon = source ? source->As<RE::TESObjectWEAP>() : nullptr;
			const bool  blow = !a_projectile && (!source || (weapon && !weapon->IsBow() && !weapon->IsCrossbow()));
			if (!blow) {
				return;
			}
			auto* player = Player();
			for (auto& a : g_auras) {
				auto* on = a.target ? a.target.get().get() : static_cast<RE::Actor*>(player);
				if (on != a_victim || (Character::Get().classId == 2 && a.end <= Frame::Now())) {
					continue;
				}
				if (a.shield > 0 && a_attacker && a_attacker != player && !a_attacker->IsDead()) {
					ApplyHealth(a_attacker, "damage", a.shield, a.spell, a.school);
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
			if (g_channel->selfAura) for (auto& a : g_auras)
				if (!a.target && a.spell == g_channel->spell)
					a.end = std::min({a.end, g_channel->end, Frame::Now()});
			for (const auto id : g_channel->hit) {
				Lua(std::format("__WBAuraRemove({}, {})", id == Player()->GetFormID() ? std::string("\"player\"") : std::to_string(id), g_channel->spell));
			}
			for (const auto id : g_channel->slowed) {
				if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id); actor && !actor->IsDisabled()) Carriers::Unbuff(actor, g_channel->spell);
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
		void AreaTick(Channel& c, bool persistent = false)
		{
			if (c.selfAura) return;
			auto*      player = Player();
			json       targets = json::array();
			json skipped = json::array();
			if (c.heal) {   // Tranquility: their party in the circle, friendly and alive
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
				// All in it the player may strike but their followers and, unless the player chose otherwise (Interface Options), friendly
				// people: a WoW area spell never touches a friendly (green) unit, so Hurricane in a town starts no war; a
				// deliberate cast at one still provokes.
				if (c.heal || !actor || actor == player || actor->IsDead() || !actor->Is3DLoaded() ||
					!Bridge::Attackable(actor) || actor->IsPlayerTeammate()) {
					continue;
				}
				const auto p = actor->GetPosition();
				if (std::hypot(p.x - c.at.x, p.y - c.at.y) <= c.radius && std::abs(p.z - c.at.z) <= c.radius) {
					if (!g_areaFriendly && Bridge::Reaction(actor) == 5) {
						skipped.push_back(actor->GetFormID());
						continue;
					}
					json victim={{"id",actor->GetFormID()},{"level",actor->GetLevel()},{"is_player",false}};
                    AugmentTarget(victim,actor);
                    targets.push_back(std::move(victim));
				}
			}
			++c.tick;
			g_talentEvidence[persistent ? "ground_tick" : "channel"]={{"spell",c.spell},{"tick",c.tick},{"period",c.amplitude},
				{"area_friendly",g_areaFriendly.load()},{"targets",targets},{"skipped_friendly",skipped}};
			if (targets.empty()) {
				return;
			}
			Service::Request({ { "op", "tick_area" }, { "spell", c.spell }, { "tick", c.tick }, { "caster", CasterNow() }, { "targets", targets },
								 { "seed", std::uniform_int_distribution<std::uint32_t>()(g_rng) } },
				[spell = c.spell, snapshot = c, persistent, epoch = g_epoch](const json& r) mutable {
					if (epoch != g_epoch || !r.value("ok", false)) return;
					Channel* active = nullptr;
					if (persistent) {
						if (g_groundSerial[spell] != snapshot.serial) return;
						for (auto& area : g_areas) if (area.serial == snapshot.serial) active = &area;
					} else {
						if (!g_channel || g_channel->spell != spell) return;
						active = &*g_channel;
					}
					auto& c = active ? *active : snapshot;   // the final ground tick may arrive after its area expired
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
							if (c.hit.insert(actor->GetFormID()).second) {
								Lua(std::format("__WBAuraAdd({}, {}, false, {:.3f})", her ? std::string("\"player\"") : std::to_string(actor->GetFormID()),
									spell, std::max(0.0, c.end - Frame::Now())));
							}
							continue;
						}
						if (!actor || (persistent && (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded())) || hit.value("type", "") != "damage") {
							continue;
						}
						const int amount = hit.value("amount", 0);
						g_talentEvidence["area_damage"].push_back({{"spell",spell},{"target",actor->GetFormID()},{"amount",amount}});
						ApplyHealth(actor, "damage", amount, spell, persistent ? c.school : -1);
						if (persistent && amount > 0) Forms::AttackThreat(r, actor);
						// Hurricane's storm slows what it strikes, for a little over a tick: it lapses once they leave. Its aura
						// (SPELL_AURA_MOD_HASTE -25) makes the time between attacks 25% longer: speed x 1/1.25, as melee_haste.
						if (const float slow = hit.value("attack_slow_percent", 0.0f); slow > 0 && !actor->IsDead()) {
							Carriers::Buff(actor, spell, c.name, { { 0, 1.0f / (1.0f + slow / 100.0f) - 1.0f, RE::ActorValue::kWeaponSpeedMult } },
								static_cast<float>(c.amplitude * 1.5));
							c.slowed.insert(actor->GetFormID());
						}
						Log("CHAT_MSG_SPELL_PERIODIC_CREATURE_DAMAGE", "PERIODICAURADAMAGESELFOTHER", Name(actor), amount,
							std::string(kSchool[std::clamp(c.school, 0, 6)]), c.name);
						if (!persistent || c.end > Frame::Now())
							Lua(std::format("__WBAuraAdd({}, {}, true, {:.3f})", actor->GetFormID(), spell, std::max(0.0, c.end - Frame::Now())));
						const auto at = actor->GetPosition();
						WowUi::PlaySound(WowUi::Sounds(spell).impact, &at);
						SpellVisuals::Play(spell, SpellVisuals::Kit::Impact, actor->CreateRefHandle(), false);
						c.hit.insert(actor->GetFormID());
					}
				});
		}
		void RequestJudgedHit(RE::Actor* attacker, const json& victim, float damage, const char* kind, std::uint32_t seed)
		{
			json request = {{"op", "judged_hit"}, {"caster", CasterNow()}, {"attacker", {{"level", attacker->GetLevel()}}},
				{"target", victim}, {"attack", {{"kind", kind}, {"damage", damage}}}, {"seed", seed}};
			g_talentEvidence["judged_requests"] = g_talentEvidence.value("judged_requests", 0) + 1;
			Service::Request(request, [epoch = g_epoch, handle = attacker->CreateRefHandle(), request, serial = g_talentEvidence["judged_requests"].get<int>()](const json& out) {
				if (epoch != g_epoch || Character::Get().classId != 2) return;
				auto actor = handle.get();
				g_talentEvidence["judged_hit"] = {{"serial", serial}, {"request", request}, {"reply", out}};
				if (out.value("ok", false) && actor && actor->IsPlayerTeammate() && !actor->IsDead() && !actor->IsDisabled()) TalentEffects(out, actor.get());
			});
		}

		void NativeHitProcs()
		{
			std::vector<NativeProcHit> hits;
			{ std::scoped_lock lock(g_nativeProcMutex);
				std::erase_if(g_nativeProcHits, [&](const NativeProcHit& hit) {
					if (hit.ready > Frame::Now()) return false;
					hits.push_back(hit); return true;
				});
			}
			for (const auto& hit : hits) {
				auto source = hit.attacker.get(), victim = hit.victim.get();
				if (hit.epoch != g_defenseEpoch || Character::Get().classId != 2 || !source || !victim ||
					source->IsDead() || source->IsDisabled() || victim->IsDisabled()) continue;
				if (victim.get() != Player()) {
					if (source.get() != Player() && source->IsPlayerTeammate() && !hit.judgements.empty()) {
						// Keep the auras present at damage time even if the victim died or the aura expired while queued.
						RequestJudgedHit(source.get(), {{"kind", "unit"}, {"judgements", hit.judgements}}, hit.damage, hit.kind.c_str(), std::uniform_int_distribution<std::uint32_t>()(g_rng));
					} else if (source.get() == Player()) {
						float threat = 1;
						for (const auto& a : g_auras) if (!a.target && a.end > Frame::Now()) for (const auto& e : a.effects)
							if (e.value("type", "") == "threat" && (e.value("school_mask", 127u) & (1u << hit.school))) threat *= e.value("multiplier", 1.f);
						Forms::AttackThreat({{"threat_multiplier", threat}}, victim.get());
					}
					continue;
				}
				if (Player()->IsDead() || DefenseAlly(source.get())) continue;
				json target = {{"kind", "unit"}, {"level", source->GetLevel()}, {"reaction", "hostile"}, {"alive", true}};
				AugmentTarget(target, source.get());
				// Skyrim spells expose no critical-hit bit. For procs only, use Classic creatures' base 5%
				// (Unit::GetCritChance, Unit.cpp:3485). Undo x1.5 on the landed portion only; Sanctuary is flat before crit.
				const bool crit = std::uniform_real_distribution<float>(0, 100)(g_rng) < 5;
				json request = {{"op", "incoming_hit"}, {"weapon_hit", false}, {"caster", CasterNow()}, {"target", target},
					{"attack", {{"kind", "spell"}, {"school", hit.school}, {"damage", (hit.damage - hit.reduced) / (crit ? 1.5f : 1.f) + hit.reduced}, {"crit", crit}}},
					{"ctx", {{"sitting", Player()->AsActorState()->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSitting}}},
					{"seed", std::uniform_int_distribution<std::uint32_t>()(g_rng)}};
				g_talentEvidence["native_spell_requests"] = g_talentEvidence.value("native_spell_requests", 0) + 1;
				Service::Request(request, [hit, request, serial = g_talentEvidence["native_spell_requests"].get<int>()](const json& out) {
					if (hit.epoch != g_defenseEpoch || Character::Get().classId != 2 || Player()->IsDead()) return;
					auto source = hit.attacker.get();
					g_talentEvidence["native_spell"] = {{"serial", serial}, {"request", request}, {"reply", out}};
					if (out.value("ok", false) && source && !DefenseAlly(source.get()) && !source->IsDead() && !source->IsDisabled()) TalentEffects(out, source.get());
				});
			}
		}

		void SalvationThreat()
		{
			if (!g_paladin) return;
			// Skyrim has no threat table. Once a second, Salvation can move a follower's pursuer to an
			// unprotected party member; their own damage uses the same probability in Forms::AttackThreat.
			for (const auto& aura : g_auras) if (aura.target && aura.end > Frame::Now()) {
				float threat = 1;
				for (const auto& e : aura.effects) if (e.value("type", "") == "threat") threat *= e.value("multiplier", 1.f);
				if (threat >= 1) continue;
				auto who = aura.target.get(); if (!who || who->IsDead() || who->IsDisabled() || !who->Is3DLoaded()) continue;
				for (const auto& h : RE::ProcessLists::GetSingleton()->highActorHandles) if (auto enemy = h.get(); enemy && enemy.get() != Player() && !enemy->IsDead() && !enemy->IsPlayerTeammate() && Victim(enemy.get()) == who.get()) {
					if (std::uniform_real_distribution<float>(0, 1)(g_rng) < threat) continue;
					RE::Actor* next = Player()->IsDead() || Unattackable(Player()) ? nullptr : Player();
					for (const auto& member : Party(who->GetPosition(), 30 * kUnitsPerYard)) if (auto actor = member.get(); actor && actor.get() != who.get() &&
						!actor->IsDead() && !Unattackable(actor.get()) && (!next || actor->GetPosition().GetDistance(enemy->GetPosition()) < next->GetPosition().GetDistance(enemy->GetPosition()))) next = actor.get();
					if (next && Retarget(enemy.get(), next)) g_talentEvidence["salvation_redirects"] = g_talentEvidence.value("salvation_redirects", 0) + 1;
				}
			}
		}

		struct PendingDefense
		{
			RE::ActorHandle attacker;
			float damage;
			std::string kind;
			std::uint32_t school, seed, epoch;
			bool inFront, sitting, casting, stunned;
		};
		std::deque<PendingDefense> g_defenses;
		struct ActiveDefense
		{
			PendingDefense hit;
			json request;
			std::uint64_t serial;
			double deadline;
			bool applied = false, throwForTesting = false;
			std::optional<json> replyForTesting;
		};
		std::optional<ActiveDefense> g_activeDefense;
		std::optional<json> g_defenseReplyForTesting;
		std::uint64_t g_defenseSerial = 0;
		constexpr double kDefenseTimeout = 5;

		float ApplyDefenseDamage(RE::Actor* attacker, float damage, std::uint32_t school)
		{
			if (damage <= 0 || Player()->IsDead()) return 0;
			const float before = Player()->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
			DamageSchool scoped(1u << school);
			const bool previous = g_defenseApplied; g_defenseApplied = true;
			if (PhysicalDamage::func.address()) PhysicalDamage::func(Player(), damage, attacker, true);
			else Player()->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kHealth, damage);
			g_defenseApplied = previous;
			const float applied = std::max(0.f, before - Player()->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth));
			if (applied > 0) OnHit(false);
			return applied;
		}

		void OriginalDefense(const PendingDefense& hit)
		{
			if (hit.epoch != g_defenseEpoch || Player()->IsDead()) return;
			auto attacker = hit.attacker.get();
			const float applied = ApplyDefenseDamage(attacker.get(), hit.damage, hit.school);
			g_talentEvidence["defense_fallbacks"] = g_talentEvidence.value("defense_fallbacks", 0) + 1;
			g_talentEvidence["defense_fallback"] = {{"original", hit.damage}, {"applied", applied}};
			WorldLog::Defense(attacker.get(), {{"outcome", "hit"}, {"damage", hit.damage}, {"school", hit.school}}, applied);
			if (hit.kind == "melee" && applied > 0 && attacker && !DefenseAlly(attacker.get()) && !attacker->IsDead() && !attacker->IsDisabled())
				Struck(Player(), attacker.get(), 0, 0);
		}

		void NextDefense();
		void CompleteDefense(std::uint64_t serial, const json& out)
		{
			if (!g_activeDefense || serial != g_activeDefense->serial || g_activeDefense->hit.epoch != g_defenseEpoch) return;
			const auto hit = g_activeDefense->hit;
			const auto request = g_activeDefense->request;
			auto source = hit.attacker.get();
			if (Player()->IsDead()) { g_activeDefense.reset(); NextDefense(); return; }
			const bool ok = out.value("ok", false) && out.contains("defense") && source && !DefenseAlly(source.get());
			// A missing attacker or failed reply must not discard the health withheld by the native clamp.
			const float damage = ok ? out.at("defense").value("damage", hit.damage) : hit.damage;
			g_talentEvidence["incoming"] = {{"request", request}, {"reply", out}, {"applied", damage},
				{"health_before", Player()->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth)}};
			g_activeDefense->applied = true; // if later proc parsing throws, the watchdog must not replay this damage
			// If the parsing below throws, the slot is still released (the watchdog otherwise held every hit 5 s).
			// (Counted with the watchdog's releases: wow_paladin_damage_pipeline's after_apply case watches for one.)
			struct Release { bool done = false; ~Release() { if (!done) {
				g_talentEvidence["defense_timeouts"] = g_talentEvidence.value("defense_timeouts", 0) + 1;
				g_activeDefense.reset(); NextDefense(); } } } release;
			const float applied = ApplyDefenseDamage(source.get(), damage, hit.school);
			const json defense = ok ? out.at("defense") : json{{"outcome", "hit"}, {"damage", damage}, {"school", hit.school}};
			WorldLog::Defense(source.get(), defense, applied);
			if (!source) {
				g_talentEvidence["defense_fallbacks"] = g_talentEvidence.value("defense_fallbacks", 0) + 1;
				g_talentEvidence["defense_fallback"] = {{"original", hit.damage}, {"applied", applied}};
			}
			// Unit::DealMeleeDamage: damage shields need actual melee damage, including after absorbs.
			if (hit.kind == "melee" && applied > 0 && source && !DefenseAlly(source.get()) && !source->IsDead() && !source->IsDisabled()) Struck(Player(), source.get(), 0, 0);
			if (ok) {
				if (!source->IsDead() && !source->IsDisabled()) TalentEffects(out, source.get());
				for (const auto& [spell, used] : out.value("charges_used", json::object()).items()) {
					const auto id = static_cast<std::uint32_t>(std::stoul(spell));
					for (auto& aura : g_auras) if (!aura.target && aura.spell == id && aura.charges > 0) {
						aura.charges = std::max(0, aura.charges - used.get<int>());
						if (!aura.charges) aura.end = Frame::Now();
					}
				}
				g_talentEvidence["defense_history"].push_back(out.at("defense"));
				if (g_talentEvidence["defense_history"].size() > 256) g_talentEvidence["defense_history"].erase(0);
			}
			PublishImmunities();
			release.done = true;
			g_activeDefense.reset();
			NextDefense();
		}

		void NextDefense()
		{
			if (g_activeDefense) {
				if (Frame::Now() < g_activeDefense->deadline) return;
				const auto pending = *g_activeDefense;
				g_activeDefense.reset(); // retire the serial before a late callback can see it
				logger::warn("cast: defence request {} timed out; releasing the native hit", pending.serial);
				if (!pending.applied) OriginalDefense(pending.hit);
				g_talentEvidence["defense_timeouts"] = g_talentEvidence.value("defense_timeouts", 0) + 1;
				// A silent service (connected, not answering) would otherwise hold each hit already queued behind it
				// for 5 s in turn: those land as original damage now. Hits after this ask the service as usual.
				while (!g_defenses.empty()) {
					const auto queued = g_defenses.front(); g_defenses.pop_front();
					if (queued.epoch == g_defenseEpoch && !Player()->IsDead()) OriginalDefense(queued);
				}
			}
			while (!g_defenses.empty()) {
				const auto hit = g_defenses.front(); g_defenses.pop_front();
				if (hit.epoch != g_defenseEpoch) continue;
				auto attacker = hit.attacker.get();
				if (Player()->IsDead()) continue;
				if (!attacker || DefenseAlly(attacker.get())) { OriginalDefense(hit); continue; }
				json target = {{"kind", "unit"}, {"level", attacker->GetLevel()}, {"alive", !attacker->IsDead()},
					{"reaction", "hostile"}, {"in_front", hit.inFront}, {"distance_yd", Player()->GetPosition().GetDistance(attacker->GetPosition()) / kUnitsPerYard}};
				AugmentTarget(target, attacker.get());
				const json request = {{"op", "incoming_hit"}, {"weapon_hit", hit.kind != "spell"}, {"caster", CasterNow()},
					{"target", target}, {"attack", {{"damage", hit.damage}, {"kind", hit.kind}, {"school", hit.school}}},
					{"ctx", {{"casting", hit.casting}, {"sitting", hit.sitting}, {"stunned", hit.stunned}}}, {"seed", hit.seed}};
				const auto serial = ++g_defenseSerial;
				g_activeDefense = ActiveDefense{hit, request, serial, Frame::Now() + kDefenseTimeout};
				g_activeDefense->replyForTesting = std::exchange(g_defenseReplyForTesting, std::nullopt);
				Service::Request(request, [serial, epoch = hit.epoch](const json& out) {
					if (epoch != g_defenseEpoch) return;
					if (!g_activeDefense || g_activeDefense->serial != serial) {
						g_talentEvidence["defense_retired_replies"] = g_talentEvidence.value("defense_retired_replies", 0) + 1;
						return;
					}
					if (g_activeDefense->throwForTesting) {
						g_talentEvidence["defense_callback_threw"] = true;
						throw std::runtime_error("explicit Paladin defence callback probe");
					}
					// Copy before CompleteDefense retires the active request and its optional fixture reply.
					const auto reply = g_activeDefense->replyForTesting.value_or(out);
					CompleteDefense(serial, reply);
				});
				return;
			}
		}

		void QueueDefense(RE::Actor* attacker, float damage, const char* kind, std::uint32_t school,
			std::uint32_t seed, bool inFront, bool sitting, bool casting, bool stunned)
		{
			if (!attacker || Character::Get().classId != 2) return;
			const PendingDefense hit{attacker->CreateRefHandle(), damage, kind, school, seed, g_defenseEpoch.load(), inFront, sitting, casting, stunned};
			if (DefenseAlly(attacker)) { OriginalDefense(hit); return; }
			g_defenses.push_back(hit);
			NextDefense();
		}

		void DispatchDefense(RE::ActorHandle handle, float damage, std::string kind, std::uint32_t school, std::uint32_t epoch)
		{
			auto attacker = handle.get();
			if (epoch != g_defenseEpoch || !Integration::Active() || Player()->IsDead()) return;
			if (!attacker || Character::Get().classId != 2 || DefenseAlly(attacker.get())) {
				OriginalDefense({handle, damage, kind, school, 0, epoch, false, false, false, false}); return;
			}
			const auto to = attacker->GetPosition() - Player()->GetPosition();
			const float heading = Player()->GetHeading(false);
			const bool front = std::sin(heading) * to.x + std::cos(heading) * to.y >= 0;
			QueueDefense(attacker.get(), damage, kind.c_str(), school, std::uniform_int_distribution<std::uint32_t>()(g_rng), front,
				Player()->AsActorState()->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSitting, Casting(),
				Player()->AsActorValueOwner()->GetActorValue(RE::ActorValue::kParalysis) > 0);
		}

		bool DeferDefense(RE::Actor* actor, float damage)
		{
			// Spells keep synchronous native damage. Friendly fire gets neither a hostile defence roll nor procs.
			if (!g_paladin || g_defenseApplied || g_reported || actor != RE::PlayerCharacter::GetSingleton() ||
				!g_damageAttacker || std::string_view(g_attackKind) == "spell" || DefenseAlly(g_damageAttacker)) return false;   // weapons: main thread
			const auto handle = g_damageAttacker->CreateRefHandle();
			const std::string kind = g_attackKind;
			const auto mask = g_damageSchool;
			const std::uint32_t school = mask & 1 ? 0 : mask & 2 ? 1 : mask & 4 ? 2 : mask & 8 ? 3 : mask & 16 ? 4 : mask & 32 ? 5 : mask & 64 ? 6 : 0;
			SKSE::GetTaskInterface()->AddTask([handle, damage, kind, school, epoch = g_defenseEpoch.load()] {
				DispatchDefense(handle, damage, kind, school, epoch);
			});
			return true;
		}

		void TransferDamage(float amount, std::uint32_t school)
		{
			if (amount <= 0 || Player()->IsDead()) return;
			DamageSchool scoped(1u << school);
			const bool previous = g_transferDamage; g_transferDamage = true;
			// Sacrifice is unmitigated: no armor, difficulty or new defence roll; their absorbs still count.
			Player()->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kHealth, amount);
			g_transferDamage = previous;
		}

		void PartyHit(RE::ActorHandle member, RE::ActorHandle attacker, const PartyProtection& protection,
			float damage, std::string kind, std::uint32_t school, bool blocked, bool synchronous,
			float memberApplied = 0, float transferred = 0)
		{
			auto victim = member.get(); auto source = attacker.get();
			if (!victim || victim->IsDisabled()) return;
			json target = {{"kind", "unit"}, {"level", source ? source->GetLevel() : victim->GetLevel()}};
			if (source) AugmentTarget(target, source.get());
			auto caster = CasterNow(); caster["auras"] = protection.casterAuras;
			const json request = {{"op", "party_hit"}, {"caster", caster}, {"target", target},
				{"member", {{"id", victim->GetFormID()}, {"auras", protection.auras}}},
				{"attack", {{"damage", damage}, {"kind", kind}, {"school", school}, {"outcome", blocked ? "block" : "hit"}}},
				{"ctx", {{"caster_alive", protection.casterAlive}, {"self_damage", source.get() == victim.get()}}},
				{"seed", std::uniform_int_distribution<std::uint32_t>()(g_rng)}};
			g_talentEvidence["party_hit_requests"] = g_talentEvidence.value("party_hit_requests", 0) + 1;
			Service::Request(request, [epoch = g_epoch, member, attacker, damage, school, synchronous, memberApplied, transferred, request, serial = g_talentEvidence["party_hit_requests"].get<int>()](const json& out) {
				if (epoch != g_epoch) return;
				auto victim = member.get(); auto source = attacker.get();
				if (!victim || victim->IsDisabled()) return;
				const bool ok = out.value("ok", false) && out.contains("member_damage");
				const float resolved = ok ? out.value("member_damage", damage) : damage;
				g_talentEvidence["party_hit"] = {{"serial", serial}, {"request", request}, {"reply", out}, {"synchronous", synchronous}, {"applied", synchronous ? memberApplied : resolved}};
				if (!synchronous && !victim->IsDead()) {
					DamageSchool scoped(1u << school);
					const bool previous = g_defenseApplied; g_defenseApplied = true;
					if (PhysicalDamage::func.address()) PhysicalDamage::func(victim.get(), resolved, source.get(), true);
					else victim->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kHealth, resolved);
					g_defenseApplied = previous;
				} else if (synchronous && !victim->IsDead()) {
					// Most replies match the per-hit snapshot exactly. Consume member_damage even if their
					// talents changed meanwhile, or the service failed: correct only the difference, never replay a spell.
					const float correction = memberApplied - resolved;
					if (correction > .025f) victim->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kHealth, correction);
					else if (correction < -.025f) {
						DamageSchool scoped(1u << school);
						const bool previous = g_defenseApplied; g_defenseApplied = true;
						victim->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kHealth, -correction);
						g_defenseApplied = previous;
					}
				}
				if (ok) {
					float transfer = 0;
					for (const auto& event : out.value("events", json::array())) if (event.value("type", "") == "damage_transfer") transfer += event.value("amount", 0.f);
					TransferDamage(transfer, school);   // split damage neither pushes back nor interrupts (InterruptOrDelaySpell)
					if (synchronous) g_talentEvidence["party_hit"]["snapshot_matches"] = std::abs(resolved - memberApplied) < .03f && std::abs(transfer - transferred) < .03f;
					TalentEffects(out, source.get());
				}
			});
		}

		bool PartyDamage(RE::Actor* actor, float& damage)
		{
			// No attacker (a trap, a fall, a script): nothing to split or reduce (Unit::DealDamage needs an attacker).
			if (!g_paladin || actor == Player() || !g_damageAttacker || g_transferDamage || g_defenseApplied || g_reported) return false;
			PartyProtection protection;
			{ std::scoped_lock lock(g_immunityMutex);
				const auto it = g_partyProtection.find(actor);
				if (it == g_partyProtection.end()) return false;
				protection = it->second;
			}
			protection.casterAlive = !Player()->IsDead();
			const auto member = actor->CreateRefHandle();
			const auto attacker = g_damageAttacker ? g_damageAttacker->CreateRefHandle() : RE::ActorHandle{};
			const auto mask = g_damageSchool;
			const std::uint32_t school = mask & 1 ? 0 : mask & 2 ? 1 : mask & 4 ? 2 : mask & 8 ? 3 : mask & 16 ? 4 : mask & 32 ? 5 : mask & 64 ? 6 : 0;
			const std::string kind = g_attackKind;
			const bool synchronous = kind == "spell";
			const bool blocked = !synchronous && g_physicalBlocked;
			const float original = damage;
			float transfer = 0;
			if (synchronous) {
				// Sanctuary and Sacrifice have one budget per one-second native tick, not per rendered frame.
				const bool canTransfer = g_damageAttacker != actor && protection.casterAlive && !ImmuneTo(Player(), 1u << school);
				std::scoped_lock lock(g_nativeProcMutex);
				auto it = std::ranges::find_if(g_nativePartyTicks, [&](const NativePartyTick& tick) {
					return tick.member == member && tick.attacker == attacker && tick.school == school &&
						tick.epoch == g_defenseEpoch && tick.ready > Frame::Now();
				});
				if (it == g_nativePartyTicks.end()) {
					g_nativePartyTicks.push_back({member, attacker, protection, school, g_defenseEpoch.load(), Frame::Now() + 1});
					it = std::prev(g_nativePartyTicks.end());
					it->transferLimit = canTransfer ? protection.transfer[school] : 0;
				}
				const float reduced = std::min(damage, std::max(0.f, it->protection.reduction[school] - it->reduced));
				damage -= reduced;
				transfer = std::min(damage, std::max(0.f, it->transferLimit - it->transferred));
				damage -= transfer;
				it->damage += original; it->applied += damage; it->reduced += reduced; it->transferred += transfer;
			} else {
				damage = 0; // keep lethal weapon damage out until the service answers, as their own defence does
				SKSE::GetTaskInterface()->AddTask([epoch = g_defenseEpoch.load(), member, attacker, protection, original, kind, school, blocked] {
					if (epoch != g_defenseEpoch || !Integration::Active()) return;
					PartyHit(member, attacker, protection, original, kind, school, blocked, false);
				});
			}
			return true;
		}

		void NativePartyHits()
		{
			std::vector<NativePartyTick> ticks;
			{ std::scoped_lock lock(g_nativeProcMutex);
				std::erase_if(g_nativePartyTicks, [&](const NativePartyTick& tick) {
					if (tick.ready > Frame::Now()) return false;
					ticks.push_back(tick); return true;
				});
				std::erase_if(g_nativeReductions, [](const NativeReductionTick& tick) { return tick.ready <= Frame::Now(); });
			}
			for (const auto& tick : ticks) if (tick.epoch == g_defenseEpoch && Integration::Active())
				PartyHit(tick.member, tick.attacker, tick.protection, tick.damage, "spell", tick.school, false, true, tick.applied, tick.transferred);
		}

	}

	// You were hit (Unit::InterruptOrDelaySpell): a cast with pushback is set back (its remaining time never more than
	// the whole cast time), or cancelled by damage; a channel loses time, or ends.
	void OnHit(bool a_byYourself)
	{
		// Barkskin: blows do not delay their casting (aura 149, all schools at 100%).
		const bool steady = std::ranges::any_of(g_auras, [](const Aura& a) { return !a.target && a.noPushback; });
		float resist = g_cast ? g_cast->outcome.value("pushback_resist_pct", 0.0f) : g_channel ? g_channel->pushbackResist : 0.0f;
		// Spell::Delayed adds SPELL_AURA_RESIST_PUSHBACK to the spell's talent modifier; 35/50 is a roll, 100 is immunity.
		if (Character::Get().classId == 2) for (const auto& a : g_auras) if (!a.target && a.end > Frame::Now()) resist += a.pushbackResist;
		const bool avoided = std::uniform_real_distribution<float>(0, 100)(g_rng) < resist;
		if ((steady || avoided) && (g_cast || g_channel)) {
			g_talentEvidence["pushbacks_resisted"] = g_talentEvidence.value("pushbacks_resisted", 0) + 1;
		}
		if (g_cast && g_cast->castTime > 0) {
			const auto flags = g_cast->outcome.value("interrupt_flags", 0u);
			if (flags & 0x10) {   // damage cancels it: no mechanic, nothing resists it (Unit::InterruptOrDelaySpell)
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
		// Intercept their health before it is reduced, including lethal damage and
		// spell ticks. Hit-event healing cannot implement an absorb.
		REL::Relocation<std::uintptr_t> actor{RE::VTABLE_PlayerCharacter[0]};
		HealthDamage::func = actor.write_vfunc(0x127, HealthDamage::thunk);
		REL::Relocation<std::uintptr_t> npc{RE::VTABLE_Character[0]};
		NpcHealthDamage::func = npc.write_vfunc(0x127, NpcHealthDamage::thunk);
		if (const auto hit = REL::Relocation<std::uintptr_t>{REL::ID(38586)}.address() + 0x4A7;
			Hooks::VerifyCall(hit, REL::Relocation<std::uintptr_t>{REL::ID(37335)}.address(), "hit damage"))
			PhysicalDamage::func = SKSE::GetTrampoline().write_call<5>(hit, PhysicalDamage::thunk);
		if (const auto hit = REL::Relocation<std::uintptr_t>{REL::ID(38586)}.address() + 0xFA;
			Hooks::VerifyCall(hit, REL::Relocation<std::uintptr_t>{REL::ID(39987)}.address(), "hit record"))
			NativeHit::func = SKSE::GetTrampoline().write_call<5>(hit, NativeHit::thunk);
		if (const auto block = REL::Relocation<std::uintptr_t>{REL::ID(44014)}.address() + 0x21C;
			Hooks::VerifyCall(block, REL::Relocation<std::uintptr_t>{REL::ID(37952)}.address(), "native block check"))
			NativeBlock::func = SKSE::GetTrampoline().write_call<5>(block, NativeBlock::thunk);
		const auto magic = []<int N>(REL::VariantID id) {
			REL::Relocation<std::uintptr_t> table{id};
			MagicDamage<N>::func = table.write_vfunc(0x20, MagicDamage<N>::thunk);
			MovementStart<N>::func = table.write_vfunc(0x14, MovementStart<N>::thunk);
		};
		magic.operator()<0>(RE::VTABLE_ValueModifierEffect[0]);
		magic.operator()<1>(RE::VTABLE_DualValueModifierEffect[0]);
		magic.operator()<2>(RE::VTABLE_PeakValueModifierEffect[0]);
		magic.operator()<3>(RE::VTABLE_AccumulatingValueModifierEffect[0]);
		magic.operator()<4>(RE::VTABLE_TargetValueModifierEffect[0]);
		MechanicStart<0>::func = REL::Relocation<std::uintptr_t>{RE::VTABLE_DemoralizeEffect[0]}.write_vfunc(0x14, MechanicStart<0>::thunk);
		MechanicStart<1>::func = REL::Relocation<std::uintptr_t>{RE::VTABLE_ParalysisEffect[0]}.write_vfunc(0x14, MechanicStart<1>::thunk);
		MechanicStart<2>::func = REL::Relocation<std::uintptr_t>{RE::VTABLE_StaggerEffect[0]}.write_vfunc(0x14, MechanicStart<2>::thunk);
		// Hits on you from Skyrim (blows, arrows, spells - not damage over time) push your casting back.
		static struct HitSink : RE::BSTEventSink<RE::TESHitEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				// Hit events come on another thread: what they change (their cast, the auras) is changed on the main one.
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
						const auto* form = RE::TESForm::LookupByID(source);
						if (Character::Get().classId != 2 || (form && form->As<RE::MagicItem>() && !Carriers::IsDamage(source))) OnHit(attacker.get() == player);
						IncomingHit(attacker.get(), source, projectile);
					}
					const bool paladinVictim = Character::Get().classId == 2 && victim.get() == player;
					if (!paladinVictim) Struck(victim.get(), attacker.get(), source, projectile);
					const auto* form = source ? RE::TESForm::LookupByID(source) : nullptr;
					const bool deferredWeapon = paladinVictim && attacker && !DefenseAlly(attacker.get()) && (!form || form->IsWeapon());
					if (!deferredWeapon) WorldLog::Struck(victim.get(), attacker.get(), source, blocked);
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		} hits;
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESHitEvent>(&hits);
		// Native actions also interrupt their meld. Runtime carrier spells (the FF forms) apply their effects;
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

	void UseItem(std::uint32_t entry)
	{
		if (g_pending || g_cast || g_channel || SkyrimSpells::Casting()) { UiError("SPELL_FAILED_SPELL_IN_PROGRESS"); return; }
		if (Player()->IsDead()) { UiError("SPELL_FAILED_CASTER_DEAD"); return; }
		auto* form = Gear::FormOf(entry);
		if (!form || Player()->GetItemCount(form) <= 0) { UiError("SPELL_FAILED_ITEM_NOT_READY"); return; }
		const auto row = Armory::Row(entry);
		std::uint32_t sid = 0;
		if (const auto item = Character::Gear().find(entry); item != Character::Gear().end()) sid = item->second.useSpell;
		if (row.is_object() && row.contains("wow")) for (int n = 1; n <= 5 && !sid; ++n) {
			const auto& wow = row.at("wow");
			if (!wow.value("spelltrigger_" + std::to_string(n), 0)) sid = wow.value("spellid_" + std::to_string(n), 0u);
		}
		if (!sid) { UiError("SPELL_FAILED_SPELL_UNAVAILABLE"); return; }
		const auto facts = WowUi::Facts(sid);
		auto* target = facts.self ? nullptr : Bridge::Target();
		if (target == Player()) target = nullptr;
		if (facts.harmful && !facts.self && !target) { UiError("ERR_GENERIC_NO_TARGET"); return; }
		json t = {{"kind", "self"}};
		if (target) {
			bool los = true;
			t = {{"kind", "unit"}, {"level", target->GetLevel()}, {"alive", !target->IsDead()},
				{"reaction", Reaction(target)}, {"attackable", Bridge::Attackable(target)}, {"healable", Bridge::Healable(target)},
				{"creature_type", Bridge::CreatureTypeName(target)}, {"distance_yd", Player()->GetPosition().GetDistance(target->GetPosition()) / kUnitsPerYard},
				{"los", Player()->HasLineOfSight(target, los)}};
			AugmentTarget(t, target);
		}
		const auto handle = target ? target->CreateRefHandle() : RE::ActorHandle{};
		g_pending = true;
		auto caster = CasterNow();
		caster["gcd_ms"] = std::max(0.0, (g_gcdUntil - Frame::Now()) * 1000.0);
		Service::Request({{"op", "item_use"}, {"item", entry}, {"caster", caster}, {"target", t},
			{"seed", std::uniform_int_distribution<std::uint32_t>()(g_rng)}},
			[entry, handle, epoch = g_epoch, request = ++g_request](const json& r) {
				if (epoch != g_epoch || request != g_request) return;
				g_pending = false;
				if (!r.value("ok", false)) {
					const auto error = r.value("error", "SPELL_FAILED_ERROR");
					UiError(error == "SERVICE_OFFLINE" ? "SPELL_FAILED_ERROR" : error.c_str()); return;
				}
				if (!r.contains("uses") || r.at("uses").size() != 1) { UiError("SPELL_FAILED_SPELL_UNAVAILABLE"); return; }
				auto out = r.at("uses").at(0);
				out["item_entry"] = entry;
				out["item_requires_equipped"] = r.value("requires_equipped", false);
				out["item_cooldowns"] = r.at("item_cooldowns");
				if (out.value("item_gcd_category", 0) == 133 && Frame::Now() < g_gcdUntil) {
					UiError("ERR_ITEM_COOLDOWN"); return;
				}
				if (const int gcd = out.value("gcd_ms", 0); gcd > 0) {
					g_gcdUntil = Frame::Now() + gcd / 1000.0;
					Lua(std::format("__WBGcd({:.3f})", gcd / 1000.0));
				}
				const auto spell = out.value("spell", 0u);
				const auto on = out.value("target", "caster") == "caster" ? RE::ActorHandle{} : handle;
				g_cast = InFlight{spell, out, on};
				g_cast->castTime = out.value("cast_ms", 0) / 1000.0;
				g_cast->endAt = Frame::Now() + g_cast->castTime;
				if (g_cast->castTime > 0) {
					Lua(std::format("__WBCastStart({}, {})", spell, out.value("cast_ms", 0)));
					const auto kit = WowUi::Sounds(spell);
					g_cast->loop = WowUi::PlaySound(kit.precast);
					Avatar::Act(kit.precastAnim, true);
					g_cast->precast = SpellVisuals::Play(spell, SpellVisuals::Kit::Precast, {}, true);
				} else Complete();
			});
	}

	void Use(std::uint32_t a_spell)
	{
		if (Mounts::Use(a_spell)) return;
		Mounts::Dismount();
		// Repeated presses keep one follow-up; another command replaces it.
		if (g_formFollowup && g_formFollowup->spell == a_spell) return;
		g_formFollowup.reset();
		const auto race = Character::Looks().race;
		// Each race's General-tab actives (service/racials.py RACE_ACTIVES): Shadowmeld, War Stomp, Perception, Stoneform, Find Treasure.
		const bool racial = (a_spell == 20580 && race == 4) || (a_spell == 20549 && race == 6) || (a_spell == 20600 && race == 1) || ((a_spell == 20594 || a_spell == 2481) && race == 3);
		if (a_spell == 20582 || a_spell == 20583 || a_spell == 20585 || a_spell == 20550 || a_spell == 20551 || a_spell == 20552 ||
			((a_spell == 20580 || a_spell == 20549) && !racial) || (!racial && !Progression::Knows(a_spell))) {
			UiError("SPELL_FAILED_NOT_KNOWN");
			return;
		}
		const double now = Frame::Now();
		if (g_cast || g_pending || SkyrimSpells::Casting()) {   // a Skyrim spell of theirs is casting too
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
		if (facts.ground && !(Character::Get().classId == 2 && (a_spell == 26573 || a_spell == 20116 || a_spell == 20922 || a_spell == 20923 || a_spell == 20924))) {
			CancelPlacing();
			g_placing = a_spell;   // the next click on the ground places it
			Lua(std::format("__WBTargeting({})", a_spell));
			return;
		}
		CancelPlacing();
		auto*      player = Player();
		const bool shock = Character::Get().classId == 2 && (a_spell == 20473 || a_spell == 20929 || a_spell == 20930);
		RE::Actor* target = Bridge::Target();   // a dead one is the service's to refuse (SPELL_FAILED_TARGETS_DEAD)
		if ((!shock && facts.self) || (Character::Get().classId == 2 && (a_spell == 2812 || a_spell == 10318 || a_spell == 26573 || a_spell == 20116 || a_spell == 20922 || a_spell == 20923 || a_spell == 20924))) {                       // the client sends no target for these: whoever is selected is not asked
			target = nullptr;
		} else if (target == player) {          // yourself targeted (F1)
			if (facts.harmful && !shock) {
				UiError("SPELL_FAILED_BAD_TARGETS");
				return;
			}
			target = nullptr;
		}
		json t;
		if (facts.harmful && !shock && !(Character::Get().classId == 2 && (a_spell == 2812 || a_spell == 10318 || a_spell == 26573 || a_spell == 20116 || a_spell == 20922 || a_spell == 20923 || a_spell == 20924))) {
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
		} else if (!shock && target && !Bridge::Healable(target)) {
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
				  { "friendly", Bridge::Healable(target) && (shock || !facts.harmful) } };
			// Behind it (Shred, Ravage): the player stands in the back half of where it faces.
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
		if (Character::Get().classId == 11) t["dispellable"] = Dispellable(target ? target : player);
		const auto* cell = player->GetParentCell();
		AugmentTarget(t, target ? target : (Character::Get().classId == 2 ? player : nullptr));
		json req = { { "op", "resolve" }, { "spell", a_spell }, { "caster", CasterNow() }, { "target", t },
			{ "ctx", { { "indoors", cell && cell->IsInteriorCell() } } }, { "seed", std::uniform_int_distribution<std::uint32_t>()(g_rng) } };
		req["caster"]["combo_points"] = Forms::ComboPoints(target); // points on this captured target
		if (facts.name == "Swipe") {   // Swipe (any rank): up to three enemies in front of the player, within reach
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
			if ((a_spell == 20549 && Character::Looks().race != 6) || (a_spell == 20580 && Character::Looks().race != 4)) {
				UiError("SPELL_FAILED_NOT_KNOWN"); return;
			}
			// The state may have changed while the service answered this instant spell.
			if (a_spell == 20580 && (MovingHerself() || Player()->IsMoving() || Player()->IsDead())) {
				UiError(Player()->IsDead() ? "SPELL_FAILED_CASTER_DEAD" :
					"SPELL_FAILED_MOVING");
				return;
			}
			if (Bounces(r, handle)) { UiError("SPELL_FAILED_AURA_BOUNCED"); return; }   // before Shadowmeld breaks or the GCD
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

	void Interrupt()
	{
		// Hostile interruption (MECHANIC_INTERRUPT: Kick, Pummel, Shield Bash) is a mechanic; damage cancelling a cast,
		// moving and replacing one's own cast are not. Skyrim has no native counterpart yet (its staggers do not
		// interrupt their WoW casts): wb paladin interrupt_probe.
		if ((g_cast || g_channel) && Character::Get().classId == 2 && ResistMechanic(26)) return;
		if (g_channel && !g_cast) EndChannel(true);
		else Stop();
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
		PublishImmunities();
		Service::Pump();
		PollMechanics();
		NativeHitProcs();
		NativePartyHits();
		NextDefense();
		PollPartyAura();
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
					AreaTick(*g_channel);
				}
				if (g_channel && Frame::Now() >= g_channel->end) {
					EndChannel(false);
				}
			}
		}
		for (auto it = g_areas.begin(); it != g_areas.end();) {
			while (Frame::Now() >= it->nextTick && it->nextTick <= it->end + .001) {
				it->nextTick += it->amplitude;
				AreaTick(*it, true);
			}
			if (Frame::Now() >= it->end || Player()->IsDead()) {
				SpellVisuals::Stop(it->area);
				for (const auto id : it->hit) Lua(std::format("__WBAuraRemove({}, {})", id, it->spell));
				it = g_areas.erase(it);
			} else ++it;
		}
		TickAuras();
		if (static double nextImmunity = 0; Frame::Now() >= nextImmunity) {
			nextImmunity = Frame::Now() + .25;
			SuppressImmunityThreat();
		}
		if (static double nextSalvation = 0; Frame::Now() >= nextSalvation) {
			nextSalvation = Frame::Now() + 1; SalvationThreat();
		}
		// Their spells' magicka (service op skyrim_costs), again as skills, talents or active modifiers change.
		if (static double next = 0; Frame::Now() >= next) {
			next = Frame::Now() + 1.0;
			static json told;
			auto caster = CasterNow();
			auto weapons = Gear::WeaponDamages();
			json signature = { { "skills", caster["skyrim"] }, { "level", caster["level"] }, { "talents", caster["talents"] },
				{ "items", caster["items"] }, { "weapons", weapons }, { "indoors", caster["indoors"] },
				{ "perks", caster["perks"] }, { "class", caster["class"] },
				{ "auras", caster["auras"] }, { "equipment_modifiers", caster["equipment_modifiers"] }, { "form", caster["form"] }, { "epoch", g_epoch } };
			if (signature != told) {
				told = signature;
				Service::Request({ { "op", "skyrim_costs" }, { "caster", caster }, { "weapons", weapons } },
					[epoch = g_epoch, caster, weapons](const json& r) {
					auto* ui = RE::UI::GetSingleton();
					if (!WowUi::Ready() || !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) { told = json(); return; }
					const auto current = CasterNow();
					if (epoch != g_epoch || !r.value("ok", false) || current["talents"] != caster["talents"] ||
						current["level"] != caster["level"] || current["perks"] != caster["perks"] ||
						current["auras"] != caster["auras"] || current["form"] != caster["form"] ||
						current["items"] != caster["items"] || current["skyrim"] != caster["skyrim"] ||
						current["equipment_modifiers"] != caster["equipment_modifiers"] || current["indoors"] != caster["indoors"] ||
						Gear::WeaponDamages() != weapons) {
						told = json();
						return;
					}
					std::string lua = "__WB.skyrimCosts = {";
					for (const auto& [spell, cost] : r.value("costs", json::object()).items()) {
						lua += std::format(" [{}] = {},", spell, cost.get<int>());
					}
					// Each WoW weapon's swing in their hand now, for its tooltip's damage lines (wowapi_bags.lua wowTooltip).
					lua += " } __WB.skyrimWeapons = {";
					for (const auto& [entry, w] : r.value("weapon_tooltips", json::object()).items()) {
						lua += std::format(" [{}] = {{ min = {}, max = {} }},", entry, w.value("min", 0.0), w.value("max", 0.0));
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
		++g_mechanicEpoch;
		g_mechanicAsked = json(); g_mechanicProfile = json::object(); g_mechanicPending = false; g_mechanicNext = 0;
		for (int n = 0; n < 31; ++n) { g_mechanicResist[n] = 0; g_mechanicRolls[n] = 0; g_mechanicResisted[n] = 0; }
		++g_epoch;
		++g_defenseEpoch;
		g_activeDefense.reset(); g_defenseReplyForTesting.reset();
		g_defenses.clear();
		{ std::scoped_lock lock(g_nativeProcMutex); g_nativeProcHits.clear(); g_nativePartyTicks.clear(); g_nativeReductions.clear(); }
		g_paladinProcCooldowns.clear();
		g_paladinManaSources.clear();
		g_partyPollAt = 0; g_partyPollBusy = false; ++g_partyPollSerial;
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
		for (const auto& area : g_areas) SpellVisuals::Stop(area.area);
		g_areas.clear();
		g_groundSerial.clear();
		PublishImmunities();
		{ std::scoped_lock lock(g_absorbMutex); g_absorbs.clear(); }
		Watch();   // a loaded game's actors are not those the player held
		g_gcdUntil = 0;
		g_cooldowns.clear();
		g_held.clear();
		Lua("__WBResetCombat()");
		if (g_loaded) {
			Restore(*g_loaded);
			g_loaded.reset();
		}
	}

    void RefreshCooldownsForTesting()
    {
        g_gcdUntil=0; g_cooldowns.clear(); g_held.clear();
        Lua("__WBRefreshCooldowns()");
    }
	void ResetForTesting()
	{
		Stop();
		if (Character::Get().classId == 2) { Melee::Stop(); Melee::Reset(); }
		for (const auto& a : g_auras) {
			auto target = a.target.get();
			auto* actor = a.target ? target.get() : static_cast<RE::Actor*>(Player());
			if (actor) {
				Carriers::Unbuff(actor, a.spell);
				if (a.root) { Carriers::Unroot(actor); }
				if (a.sleep) { Carriers::Wake(actor); }
				if (a.calm) { Carriers::Uncalm(actor); }
				if (a.fear) { Carriers::Unfear(actor); }
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
		std::map<std::uint32_t, Absorb> absorbs;
		{ std::scoped_lock lock(g_absorbMutex); absorbs = g_absorbs; }
		return [auras = g_auras, cooldowns = g_cooldowns, procs = g_paladinProcCooldowns, areas = g_areas, extra = Melee::ExtraAttacks(), held = g_held, absorbs, at = Frame::Now(), gcd = g_gcdUntil, spent = g_manaSpentAt]() {
			const double now = Frame::Now(), shift = now - at;
			bool restored = true;
			for (auto a : auras) {
				auto target = a.target.get();
				auto* who = a.target ? target.get() : static_cast<RE::Actor*>(Player());
				if (a.end <= at) continue;
				if (!who || who->IsDead() || who->IsDisabled()) { restored = false; continue; }
				a.start += shift; a.end += shift;
				if (a.controlEnd > 0) a.controlEnd += shift;
				if (a.breakAt >= 0) a.breakAt += shift;
				a.nextDispel += shift;
				const float remaining = static_cast<float>(a.end - now);
				if (!a.skyrim.empty()) Carriers::Buff(who, a.spell, a.name, a.skyrim, remaining);
				if (a.root) Carriers::Root(who, remaining);
				if (a.sleep) Carriers::Sleep(who, remaining);
				if (a.calm) Carriers::Calm(who);
				if (a.fear) Carriers::Fear(who);
				if (Character::Get().classId == 2 && (a.stack == "Seal" || a.stack == "Judgement")) g_paladinManaSources.insert(a.spell);
				const auto whoArg = a.target ? std::to_string(who->GetFormID()) : std::string("\"player\"");
				Lua(std::format("__WBAuraAdd({}, {}, {}, {:.3f}, {})", whoArg, a.spell, a.harmful ? "true" : "false",
					a.stealth || a.end > 1e11 ? 0.0 : remaining, TooltipLua(a.tooltip)));
				a.state = SpellVisuals::Play(a.spell, SpellVisuals::Kit::State, a.target, true);
				g_auras.push_back(std::move(a));
			}
			for (const auto& [spell, until] : cooldowns) if (until > at && until < kHeld) {
				g_cooldowns[spell] = until + shift;
				Lua(std::format("__WBCooldown({}, {:.3f})", spell, until - at));
			}
			for (const auto& [spell, timers] : held) for (const auto& [key, ms] : timers) {
				g_cooldowns[key] = kHeld;
				Lua(std::format("__WBCooldown({}, {:.3f}, 1)", key, ms / 1000.0));
			}
			g_held = held;
			{ std::scoped_lock lock(g_absorbMutex);
				for (auto [spell, shield] : absorbs) if (shield.end > at) { shield.end += shift; g_absorbs[spell] = shield; }
			}
			g_gcdUntil = std::max(now, gcd + shift);
			g_manaSpentAt = spent + shift;
			if (g_gcdUntil > now) Lua(std::format("__WBGcd({:.3f})", g_gcdUntil - now));
			for (const auto& [spell, until] : procs) if (until > at) g_paladinProcCooldowns[spell] = until + shift;
			Melee::QueueExtra(extra - Melee::ExtraAttacks());
			for (auto area : areas) if (area.end > at) {
				area.start += shift; area.end += shift; area.nextTick += shift;
				area.serial = ++g_areaSerial; g_groundSerial[area.spell] = area.serial;
				area.area = SpellVisuals::Area(area.spell, area.at, area.radius / kUnitsPerYard);
				g_areas.push_back(std::move(area));
			}
			PublishImmunities();
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
		if (Shapeshifts(a_outcome)) FreeMovement();
		AddAura(a_spell, a_outcome, {});
		FormTracking(a_outcome);
	}

	json Snapshot()
	{
		const double now = Frame::Now();
		json         j = { { "saved_at", WallNow() }, { "cooldowns", json::object() }, { "auras", json::array() } };
		if (Character::Get().classId == 11) j["form_resources"] = Forms::SaveResources();
		for (const auto& [spell, until] : g_cooldowns) {
			if (until > now && until < kHeld) {
				j["cooldowns"][std::to_string(spell)] = until - now;
			}
		}
		for (const auto& [spell, timers] : g_held) {   // a held timer would run from the aura's natural end
			double left = 0;
			for (const auto& a : g_auras) if (a.spell == spell && a.end < 1e11) left = std::max(left, a.end - now);
			for (const auto& [key, ms] : timers) j["cooldowns"][std::to_string(key)] = left + ms / 1000.0;
		}
		if (Character::Get().classId == 2) {
			j["paladin_procs"] = json::object();
			for (const auto& [spell, until] : g_paladinProcCooldowns) if (until > now) j["paladin_procs"][std::to_string(spell)] = until - now;
			j["extra_attacks"] = Melee::ExtraAttacks();
		}
		for (const auto& a : g_auras) {
			if (a.target || a.end <= now || (g_channel && g_channel->selfAura && g_channel->spell == a.spell)) {
				continue;
			}
			json ticks = json::array();   // seconds from now, amount, kind
			for (std::size_t i = a.next; i < a.ticks.size(); ++i) {
				ticks.push_back({ a.start + a.ticks[i].first - now, a.ticks[i].second, a.kinds[i] });
			}
			const auto skyrim = AuraPersistence::SaveChanges(a.skyrim);
			j["auras"].push_back({ { "spell", a.spell }, { "name", a.name }, { "stack", a.stack }, { "stack_rule", a.stackRule }, { "stack_amounts", a.stackAmounts }, { "harmful", a.harmful }, { "school", a.school },
				{ "tooltip", a.tooltip },
				{ "respite", a.respite },
				{ "threat_multiplier", a.threat },
				{ "remaining", a.end - now }, { "ticks", ticks }, { "skyrim", skyrim }, { "shield", a.shield }, { "steady", a.noPushback }, { "wow_per_tick", a.wowPerTick },
				{ "tracking_aura", a.trackingAura }, { "tracking_mask", a.trackingMask },
				{ "ends_with_form", a.endsWithForm }, { "form_outcome", a.formOutcome },
				{ "periodic_dispel", AuraPersistence::SaveDispel(a, now) } });
			if (Character::Get().classId == 2) {
				auto& saved = j["auras"].back(); saved["pushback_resist"] = a.pushbackResist; saved["duration"] = a.duration;
				saved["unattackable"] = a.unattackable; saved["immunity"] = a.immunity; saved["pacify"] = a.pacify;
				saved["charges"] = a.charges; saved["holy_threat"] = a.holyThreat;
				saved["freedom"] = a.freedom; saved["party_source"] = a.partySource; saved["effects"] = a.effects;
			}
			{ std::scoped_lock lock(g_absorbMutex);
				if (const auto shield = g_absorbs.find(a.spell); shield != g_absorbs.end()) {
					j["auras"].back()["absorb_remaining"] = shield->second.remaining;
					j["auras"].back()["absorb_schools"] = shield->second.schools;
				}
			}
		}
		return j;
	}

	void Restore(const json& a_saved)
	{
		const auto validAbsorbs = [&] {
			for (const auto& aura : a_saved.value("auras", json::array()))
				if (!CastPersistence::Fields(aura, {"absorb_remaining"}, [](const json& x) { return CastPersistence::Number(x, 0, 1e8); }) ||
					!CastPersistence::Fields(aura, {"absorb_schools"}, [](const json& x) { return CastPersistence::UInt(x) && x.get<std::uint32_t>() <= 127; })) return false;
			return true;
		};
        const auto validPaladin = [&] {
			if (!CastPersistence::Fields(a_saved, {"extra_attacks"}, [](const json& x) { return CastPersistence::UInt(x) && x.get<std::uint32_t>() <= 5; })) return false;
			if (a_saved.contains("paladin_procs") && !CastPersistence::Valid({{"cooldowns", a_saved["paladin_procs"]}})) return false;
			for (const auto& aura : a_saved.value("auras", json::array()))
				if (!CastPersistence::Fields(aura, {"immunity"}, [](const json& x) { return CastPersistence::UInt(x) && x.get<std::uint32_t>() <= 127; }) ||
					!CastPersistence::Fields(aura, {"charges"}, [](const json& x) { return CastPersistence::UInt(x) && x.get<std::uint32_t>() <= 1000000; }) ||
					!CastPersistence::Fields(aura, {"pacify", "freedom", "party_source", "unattackable"}, CastPersistence::Bool) ||
					!CastPersistence::Fields(aura, {"duration", "pushback_resist"}, [](const json& x) { return CastPersistence::Number(x, 0, 1e12); }) ||
					!CastPersistence::Fields(aura, {"holy_threat"}, [](const json& x) { return CastPersistence::Number(x, 0, 100); })) return false;
			for (const auto& aura : a_saved.value("auras", json::array())) if (aura.contains("effects")) {
				if (!aura["effects"].is_array() || aura["effects"].size() > 64) return false;
				for (const auto& e : aura["effects"]) if (!e.is_object() ||
					!CastPersistence::Fields(e, {"type"}, [](const json& x) { return x.is_string(); }) ||
					!CastPersistence::Fields(e, {"school_mask"}, [](const json& x) { return CastPersistence::UInt(x) && x.get<std::uint32_t>() <= 127; }) ||
					!CastPersistence::Fields(e, {"amount", "native_per_hit"}, [](const json& x) { return CastPersistence::Number(x, -1e9, 1e9); })) return false;
			}
			return true;
		};
        if (!CastPersistence::Valid(a_saved) || !validAbsorbs() || !validPaladin()) {
            logger::warn("cast: invalid saved state; auras and cooldowns left unchanged");
            return;
        }
		const double now = Frame::Now();
		const double away = std::max(0.0, WallNow() - a_saved.value("saved_at", WallNow()));
		if (a_saved.contains("mana")) {   // saved while their magicka mirrored WoW mana, its base set to WoW's 5284
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
			a.stack = s.value("stack", "");
			a.stackRule = s.value("stack_rule", "");
			if (Character::Get().classId == 2 && (a.stack == "Seal" || a.stack == "Judgement")) g_paladinManaSources.insert(a.spell);
			a.stackAmounts = s.value("stack_amounts", std::map<std::string, float>{});
			a.tooltip = s.value("tooltip", json());
			a.harmful = s.value("harmful", false);
			a.school = s.value("school", 0);
			const double remaining = s.value("remaining", 0.0) - (a.harmful ? away : 0.0);   // a helpful one waits for you
			if (remaining <= 0) {
				continue;
			}
			a.start = now;
			a.end = now + remaining;
			AuraPersistence::LoadDispel(a, s.value("periodic_dispel", json::object()), now);
			if (s.contains("absorb_remaining")) {
				std::scoped_lock lock(g_absorbMutex);
				g_absorbs[a.spell] = {s.value("absorb_remaining", 0.f), s.value("absorb_schools", 0u), a.end};
			}
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
			a.duration = s.value("duration", remaining);
			a.pushbackResist = s.value("pushback_resist", 0.f);
			if (Character::Get().classId == 2) {
				a.freedom = s.value("freedom", a.spell == 1044); a.partySource = s.value("party_source", a.stack == "Aura"); a.effects = s.value("effects", json::array()); a.serial = ++g_auraSerial;
				// Older Concentration saves marked every amount as immunity; recover the service amount.
				for (const auto& e : a.effects) if (e.value("type", "") == "pushback_immunity") {
					a.noPushback = false; a.pushbackResist = e.value("amount", 100.f);
				}
				a.unattackable = s.value("unattackable", false);
				a.immunity = s.value("immunity", 0u); a.purges = s.value("purges", true); a.pacify = s.value("pacify", false); a.charges = s.value("charges", 0); a.holyThreat = s.value("holy_threat", 1.f);
			}
			// A save from before stack groups may hold Mark and Gift together: the first restored keeps its place.
			if (std::ranges::any_of(g_auras, [&](const Aura& x) { return !x.target && x.spell != a.spell && SameStack(x, a.stack, a.stackAmounts); })) {
				continue;
			}
			if (!a.skyrim.empty()) {
				Carriers::Buff(Player(), a.spell, a.name, a.skyrim, static_cast<float>(remaining));
			}
			Lua(std::format("__WBAuraAdd(\"player\", {}, {}, {:.3f}, {})", a.spell, a.harmful ? "true" : "false", a.stealth || a.end > 1e11 ? 0.0 : remaining, TooltipLua(a.tooltip)));
			a.state = SpellVisuals::Play(a.spell, SpellVisuals::Kit::State, {}, true);
			g_auras.push_back(std::move(a));
		}
		if (Character::Get().classId == 2) {
			for (const auto& [spell, left] : a_saved.value("paladin_procs", json::object()).items()) {
				const double rest = left.get<double>() - away;
				if (rest > 0) g_paladinProcCooldowns[static_cast<std::uint32_t>(std::stoul(spell))] = now + rest;
			}
			Melee::QueueExtra(a_saved.value("extra_attacks", 0) - Melee::ExtraAttacks());
		}
		if (a_saved.contains("form_resources")) Forms::LoadResources(a_saved["form_resources"]);
		PublishImmunities();
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		const auto text = Snapshot().dump();
		const auto size = static_cast<std::uint32_t>(text.size());
		if (!a_intfc->OpenRecord(kRecord, kRecordVersion) || !a_intfc->WriteRecordData(size) || !a_intfc->WriteRecordData(text.data(), size)) {
			logger::error("cast: could not save their auras, mana and cooldowns");
		}
	}

	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length)
	{
		std::uint32_t size = 0;
		if (a_length < sizeof(size) || a_length > 4*1024*1024 || a_intfc->ReadRecordData(size) != sizeof(size) || size != a_length - sizeof(size)) {
			logger::error("cast: a damaged saved record; the player starts fresh");
			return;
		}
		std::string text(size, '\0');
		if (a_intfc->ReadRecordData(text.data(), size) != size) {
            logger::error("cast: incomplete saved record; the player starts fresh"); return;
        }
		auto j = json::parse(text, nullptr, false);
		if (!CastPersistence::Valid(j)) {
			logger::error("cast: an unreadable saved record; the player starts fresh");
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
	void SetAreaFriendly(bool a_hit) { g_areaFriendly = a_hit; }
	bool InFiveSecondRule() { return Casting() || Frame::Now() - g_manaSpentAt < 5; }

	void DefenseForTesting(RE::Actor* attacker, float damage, std::uint32_t seed, bool inFront, bool sitting)
	{
		QueueDefense(attacker, damage, "melee", 0, seed, inFront, sitting, Casting(), false);
	}

	void DefenseReplyForTesting(const json& reply)
	{
		g_defenseReplyForTesting = reply;
	}

	void DefenseFailureForTesting(RE::Actor* attacker, float damage, const std::string& fault)
	{
		if (Character::Get().classId != 2) return;
		if (fault == "task") {
			SKSE::GetTaskInterface()->AddTask([damage, epoch = g_defenseEpoch.load()] { DispatchDefense({}, damage, "melee", 0, epoch); });
		} else if (fault == "queued") {
			g_defenses.push_back({{}, damage, "melee", 0, 0, g_defenseEpoch.load(), true, false, false, false});
			NextDefense();
		} else {
			QueueDefense(attacker, damage, "melee", 0, 0, true, false, false, false);
			if (!g_activeDefense) return;
			if (fault == "reply") g_activeDefense->hit.attacker = {};
			if (fault == "after_apply") g_activeDefense->replyForTesting = json{{"ok", true},
				{"defense", {{"kind", "melee"}, {"school", 0}, {"outcome", "hit"}, {"damage", damage}}},
				{"charges_used", {{"not-a-spell", 1}}}};
			if (fault == "throw") {
				g_activeDefense->throwForTesting = true;
				// Keep the production five-second deadline: a quick service reply must throw before it expires.
			} else if (fault == "late") {
				g_activeDefense->deadline = Frame::Now();
				NextDefense(); // retire before the real service reply arrives
			}
		}
	}

	void PartyPollFailureForTesting()
	{
		g_partyPollBusy = true;
		g_partyPollDeadline = Frame::Now();
		PollPartyAura();
	}

	bool PartyHitForTesting(RE::Actor* member, RE::Actor* attacker, float damage, bool blocked)
	{
		if (!member || !attacker || Character::Get().classId != 2) return false;
		PublishImmunities();
		PartyProtection protection;
		{ std::scoped_lock lock(g_immunityMutex);
			const auto it = g_partyProtection.find(member);
			if (it == g_partyProtection.end()) return false;
			protection = it->second;
		}
		protection.casterAlive = !Player()->IsDead();
		PartyHit(member->CreateRefHandle(), attacker->CreateRefHandle(), protection, damage, "melee", 0, blocked, false);
		return true;
	}

	void WeaponHitForTesting(RE::Actor* attacker, RE::Actor* victim, float damage, bool blocked)
	{
		if (!attacker || !victim || !PhysicalDamage::func.address()) return;
		RE::HitData hit{};
		hit.Populate(attacker, victim, attacker->GetAttackingWeapon());
		g_talentEvidence["native_hit"] = {{"percent_blocked", hit.percentBlocked},
			{"blocked", hit.flags.any(RE::HitData::Flag::kBlocked)}, {"target", victim->GetFormID()}};
		hit.totalDamage = hit.physicalDamage = damage;
		if (blocked) { hit.flags.set(RE::HitData::Flag::kBlocked); hit.percentBlocked = 1; hit.totalDamage = hit.physicalDamage = 0; }
		hit.stagger = hit.targetedLimbDamage = hit.reflectedDamage = 0;
		REL::Relocation<void (*)(RE::Actor*, RE::HitData*)> process{REL::ID(38586)};
		process(victim, &hit);
	}

	void IncomingHit(RE::Actor* a_attacker, RE::FormID a_source, RE::FormID a_projectile)
	{
		if (Character::Get().classId == 2) return;   // Paladin procs belong to the pre-damage defence reply
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
        json target={{"kind","unit"},{"level",a_attacker->GetLevel()},{"reaction","hostile"},{"alive",true},
            {"distance_yd",Player()->GetPosition().GetDistance(a_attacker->GetPosition()) / kUnitsPerYard}};
        AugmentTarget(target,a_attacker);
		Service::Request({ { "op", "incoming_hit" }, { "weapon_hit", true }, { "caster", caster }, {"target",target},
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
		out["defense_busy"] = g_activeDefense.has_value();
		out["defense_queued"] = g_defenses.size();
		out["party_poll_busy"] = g_partyPollBusy;
		out["mechanic_profile"] = g_mechanicProfile;
		out["mechanic_profile_ready"] = Character::Get().classId == 2 && !g_mechanicPending && g_mechanicAsked == MechanicCaster();
		out["mechanics"] = json::object();
		for (const int n : {2,5,9,12,26}) out["mechanics"][std::to_string(n)] = {
			{"percent",g_mechanicResist[n].load()},{"rolls",g_mechanicRolls[n].load()},{"resisted",g_mechanicResisted[n].load()}};
		out["auras"] = json::array();
		for (const auto& a : g_auras) {
			const auto actor = a.target.get();
			if (a.end > Frame::Now() && (!a.target || actor)) out["auras"].push_back({ {"spell", a.spell}, {"target", a.target ? actor->GetFormID() : Player()->GetFormID()},
				{"duration", a.duration}, {"pushback_resist", a.pushbackResist}, {"unattackable", a.unattackable}, {"effects", a.effects}, {"party_member", a.partyMember}, {"freedom", a.freedom}, {"root", a.root}, {"stun", a.sleep}, {"fear", a.fear}, {"charges", a.charges}, {"immunity", a.immunity}, {"purges", a.purges}, {"pacify", a.pacify}, {"stack", a.stack}, {"calm", a.calm}, {"taunt", a.taunt}, {"control_remaining",a.controlEnd - Frame::Now()},
				{"reveal",a.reveal}, {"lockout",a.lockout}, {"armor_reduction",a.armorReduction}, {"remaining", a.end - Frame::Now()} });
		}
		out["native_block_hook"] = NativeBlock::func.address() != 0;
		out["physical_hit_hook"] = PhysicalDamage::func.address() != 0;
		out["extra_attacks"] = Melee::ExtraAttacks();
		out["ground_areas"] = json::array();
		for (const auto& area : g_areas) out["ground_areas"].push_back({{"spell", area.spell}, {"tick", area.tick},
			{"at", {area.at.x, area.at.y, area.at.z}}, {"remaining", area.end - Frame::Now()}});
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

	std::uint32_t ResourceTrackingMask()
	{
		if (Character::Get().classId != 2 || Player()->IsDead()) return 0;
		for (const auto& a : g_auras) if (!a.target && a.end > Frame::Now() && a.trackingAura == 45)
			return a.trackingMask & (1u << (6-1)); // HandleAuraTrackResources: lock type, not creature type
		return 0;
	}

	double AuraRemaining(std::uint32_t a_spell)
	{
		const double now = Frame::Now();
		for (const auto& a : g_auras) {
			if (!a.target && a.spell == a_spell && a.end > now) return a.end - now;
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
				{ std::scoped_lock lock(g_absorbMutex); g_absorbs.erase(a_spell); }
			}
		}
		PublishImmunities();
	}

	void EndAura(std::uint32_t a_spell, RE::Actor* a_target)
	{
		for (auto& a : g_auras) {
			if (a.spell == a_spell && (a_target ? a.target.get().get() == a_target : !a.target)) {
				a.end = Frame::Now();
				{ std::scoped_lock lock(g_absorbMutex); g_absorbs.erase(a_spell); }
			}
		}
		PublishImmunities();
	}

	void Augment(json& a_caster)
	{
        a_caster["actor_values"]=Bridge::NativeActorValues(Player());
		const double now = Frame::Now();
		a_caster["auras"] = json::array();
		for (const auto& aura : g_auras) {
			if (!aura.target && aura.end > now) {
				a_caster["auras"].push_back(aura.spell);
			}
		}
		a_caster["proc_cooldowns"] = json::object();
		for (const auto& [spell, until] : (Character::Get().classId == 2 ? g_paladinProcCooldowns : g_cooldowns)) {
			if (until > now) {
				a_caster["proc_cooldowns"][std::to_string(spell)] = std::lround((until - now) * 1000.0);
			}
		}
	}

	void AugmentTarget(json& a_target, RE::Actor* a_actor)
	{
		if (!a_actor) return;
        a_target["actor_values"]=Bridge::NativeActorValues(a_actor);
        a_target["is_player"]=a_actor->IsPlayerRef();
		if (Character::Get().classId == 11) a_target["dispellable"] = Dispellable(a_actor);
		if (Character::Get().classId == 2) {
			a_target["health"] = a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
			a_target["max_health"] = MaximumHealth(a_actor);
			a_target["party"] = a_actor == Player() || a_actor->IsPlayerTeammate();
			a_target["creature_type"] = Bridge::CreatureTypeName(a_actor);
			a_target["stunned"] = a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kParalysis) > 0;
			a_target["auras"] = json::array();
			a_target["judgements"] = json::array();
			for (const auto& aura : g_auras) {
				const bool on = a_actor == Player() ? !aura.target : aura.target && aura.target.get().get() == a_actor;
				if (!on || aura.end <= Frame::Now()) continue;
				a_target["auras"].push_back(aura.spell);
				if (aura.stack == "Judgement") a_target["judgements"].push_back(aura.spell);
				if (aura.sleep && aura.controlEnd > Frame::Now()) a_target["stunned"] = true;
			}
		}
		float reduction = 0;
		for (const auto& aura : g_auras) if (aura.end > Frame::Now() && aura.target.get().get() == a_actor) {
			// Faerie Fire ranks and caster/feral versions do not stack.
			reduction = std::max(reduction, aura.armorReduction);
		}
		a_target["armor_reduction"] = reduction;
	}

	bool MovementImmune(RE::Actor* actor)
	{
		if (!actor || !g_paladin) return false;
		std::scoped_lock lock(g_immunityMutex);
		const auto it = g_immunities.find(actor);
		return it != g_immunities.end() && std::ranges::any_of(it->second, [](const Immunity& i) { return i.freedom && i.end > Frame::Now(); });
	}

	bool Pacified(RE::Actor* actor)
	{
		if (!actor) actor = Player();
		std::scoped_lock lock(g_immunityMutex);
		const auto it = g_immunities.find(actor);
		return it != g_immunities.end() && std::ranges::any_of(it->second, [](const Immunity& i) {
			return i.pacify && i.end > Frame::Now(); });
	}

	bool Unattackable(RE::Actor* actor)
	{
		if (!actor || !g_paladin) return false;
		std::scoped_lock lock(g_immunityMutex);
		const auto it = g_immunities.find(actor);
		return it != g_immunities.end() && std::ranges::any_of(it->second, [](const Immunity& i) { return i.unattackable && i.end > Frame::Now(); });
	}

	void JudgedHit(RE::Actor* attacker, RE::Actor* target, float damage, const char* kind, std::uint32_t seed)
	{
		if (Character::Get().classId != 2 || !attacker || !target || attacker == Player() || !attacker->IsPlayerTeammate() ||
			attacker->IsDead() || attacker->IsDisabled() || target->IsDead() || target->IsDisabled() || damage <= 0) return;
		json victim = {{"kind", "unit"}, {"level", target->GetLevel()}};
		AugmentTarget(victim, target);
		if (victim.value("judgements", json::array()).empty()) return;
		RequestJudgedHit(attacker, victim, damage, kind, seed);
	}

	void MeleeEffects(const json& outcome, RE::Actor* target)
	{
		TalentEffects(outcome, target);
		if (Character::Get().classId == 2) g_talentEvidence["paladin_swings"] = g_talentEvidence.value("paladin_swings", 0) + 1;
		if (Character::Get().classId != 2 || !outcome.value("refresh_judgements", false)) return;
		for (auto& aura : g_auras) if (aura.stack == "Judgement" && aura.target.get().get() == target && aura.end > Frame::Now()) {
			aura.end = Frame::Now() + aura.duration;
			Lua(std::format("__WBAuraAdd({}, {}, true, {:.3f})", target->GetFormID(), aura.spell, aura.duration));
			g_talentEvidence["judgement_refreshes"] = g_talentEvidence.value("judgement_refreshes", 0) + 1;
		}
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
			if (Character::Get().classId == 2 && !proc.contains("threat_multiplier")) {
				// Seal/Judgement payloads omit the server's top-level threat hook. Use Fury's returned aura multiplier.
				const bool holy = std::ranges::any_of(proc.value("events", json::array()), [&](const json& e) {
					return (e.value("type", "") == "damage" || e.value("type", "") == "heal") && e.value("school", proc.value("school", 1)) == 1; });
				if (holy && proc.value("target", "caster") != "attacker") for (const auto& aura : g_auras) if (!aura.target && aura.end > Frame::Now() && aura.holyThreat != 1)
					proc["threat_multiplier"] = proc.value("threat_multiplier", 1.f) * aura.holyThreat;
			}
			if (Character::Get().classId == 2) {
				if (const int ms = proc.value("cooldown_ms", 0); ms > 0)
					g_paladinProcCooldowns[proc.value("source_spell", 0u)] = Frame::Now() + ms / 1000.0;
			} else StartCooldown(proc.value("source_spell", 0u), proc);
			Forms::Apply(proc, nullptr);   // rage/energy power events; combo gain is already in the parent outcome
			const bool paladinMana = Character::Get().classId == 2 && (spell == 20272 || proc.value("judgement", false) ||
				g_paladinManaSources.contains(proc.value("source_spell", 0u)));
			for (const auto& e : proc.value("events", json::array())) {
				const auto type = e.value("type", "");
				const auto destination = e.value("target", proc.value("target", "caster"));
				auto* recipient = destination == "target" || (Character::Get().classId == 2 && destination == "attacker") ? a_target : Player();
				if (recipient && type == "damage") {
					const auto amount = Character::Get().classId == 2 && proc.contains("max_health_fraction") ?
						std::min(e.value("amount", 0.f), MaximumHealth(Player()) * proc.value("max_health_fraction", 1.f)) : e.value("amount", 0.f);
					if (Character::Get().classId == 2) {
						ApplyHealth(recipient, "damage", static_cast<int>(amount), spell, e.value("school", proc.value("school", 0)));
						Forms::AttackThreat(proc, recipient);
					} else {
						Carriers::Damage(recipient, amount);
						Damaged(recipient, static_cast<int>(amount));
					}
				} else if (recipient && type == "heal") {
					const bool effective = !FullHealth(recipient) && e.value("amount", 0.f) > 0;
					recipient->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kHealth, e.value("amount", 0.f));
					if (Character::Get().classId == 2 && recipient == Player() && effective) HealingThreat(proc.value("threat_multiplier", 1.f));
                } else if (recipient && type == "absorb") {
                    recipient->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kMagicka,e.value("mana",0.f));
				}
				if (type == "extra_attacks" && Character::Get().classId == 2) Melee::QueueExtra(e.value("count", 0));
				if (type == "power" && e.value("power", "") == "mana") {
					auto* manaRecipient = Character::Get().classId == 2 ? recipient : Player();
					if (manaRecipient) manaRecipient->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kMagicka, e.value("amount", 0.0f) * (paladinMana || (Character::Get().classId == 2 && destination == "attacker") ? 1.f : .2f));
				}
			}
			if (Character::Get().classId == 2) {
				g_talentEvidence["paladin_procs"].push_back(proc);
				if (g_talentEvidence["paladin_procs"].size() > 128) g_talentEvidence["paladin_procs"].erase(0);
			}
			if (!proc.value("auras", json::array()).empty()) {
				if (!proc.contains("name")) proc["name"] = WowUi::Facts(spell).name;
				const auto on = proc.value("target", "caster") == "target" && a_target ? a_target->CreateRefHandle() : RE::ActorHandle{};
				if (Character::Get().classId != 2 || !Bounces(proc, on)) AddAura(spell, proc, on);
			}
		}
	}
}
