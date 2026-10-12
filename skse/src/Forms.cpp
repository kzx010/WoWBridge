#include "Forms.h"

#include "Avatar.h"
#include "AuraPersistence.h"
#include "Bridge.h"
#include "Carriers.h"
#include "Cast.h"
#include "Character.h"
#include "Frame.h"
#include "Melee.h"
#include "Progression.h"
#include "Service.h"
#include "ui/WowUi.h"

namespace Forms
{
	namespace
	{
		using json = nlohmann::json;

		// The service's stats.resources (Player::RegenerateAll / Regenerate at the realm's rates).
		constexpr float  kEnergyPerTick = 20, kRageDecayPerTick = 2.5f, kMax = 100;
		constexpr double kTick = 2.0;
		// Her form's Skyrim side (Carriers::Buff) is kept under one key, whichever form it is.
		constexpr std::uint32_t kFormBuff = 0xF0000001;
		constexpr std::uint32_t kTalentBuff = 0xF0000002;
		constexpr std::uint32_t kMoonkinPartyBuff = 0xF0000003;
		constexpr std::uint32_t kFeralStealthBuff = 0xF0000004;
		constexpr std::uint32_t kLeaderPartyBuff = 0xF0000005;
		constexpr std::uint32_t kProwl = 9913;
		constexpr std::uint32_t kShadowmeld = 20580;
		constexpr float         kStealthAlpha = 0.2f;   // her own model while stealthed (the client's, evidence/formref/prowl_1440.png)

		// Rage over time from an aura: Enrage's gain, Frenzied Regeneration's rage turned into health.
		struct RageAura
		{
			std::uint32_t spell = 0;
			double        next = 0, end = 0, every = 1;
			float         perTick = 0;              // rage gained (Enrage)
			float         maxPerTick = 0, healthPerRage = 0;   // rage spent for health (Frenzied Regeneration)
			std::vector<bool> crits;
			float         critMultiplier = 1.5f;
			std::size_t   tick = 0;
			bool          respite = false;
		};

		std::string          g_form = "none";
		std::string          g_power = "mana";
		float                g_rage = 0, g_energy = 0;
		struct Combo { RE::ActorHandle target; int points = 0; };
		std::map<RE::FormID, Combo> g_combos;
		bool                 g_prowling = false;
		bool                 g_shadowmeld = false;
		RE::NiPoint3         g_shadowmeldAt;
		std::uint32_t        g_prowlSpell = kProwl;
		std::uint32_t        g_nextSwing = 0;
		std::vector<RageAura> g_rageAuras;
		double               g_noDecayUntil = 0;   // Enrage's interrupt_regen
		double               g_nextEnergy = 0, g_nextDecay = 0;
		float                g_health = -1;        // last frame's, for rage from damage taken
		json                 g_formAura;           // her form's aura, for its Skyrim side again (in and out of water)
		bool                 g_aquaticInWater = false; // console land previews remain possible until the first swim
		bool                 g_swimBoost = false;  // Aquatic Form's swim speed applied
		bool                 g_sneakProwl = false; // Skyrim's sneak taken for her prowl
		double               g_prowlSince = 0;
		json                 g_profileAsked;
		std::uint32_t        g_profileEpoch = 0;
		double               g_nextProfile = 0;
		bool                 g_profilePending = false;
		float                g_castingRegen = 0;
		double               g_lastRegen = 0, g_nextParty = 0;
		float                g_reflectionRestored = 0;
		std::map<RE::FormID, RE::ActorHandle> g_moonkinParty;
		std::map<RE::FormID, RE::ActorHandle> g_leaderParty;
		json                 g_talentProfile = json::object();
		float                g_stealthApplied = 0;
		bool                 g_leaderShown = false;
		int                  g_threatRedirects = 0;
		std::mt19937         g_threatRng{std::random_device{}()};

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }
		float Cap(float a_value) { return std::clamp(a_value, 0.0f, kMax); }

		// The interface (docs/druid-client.md): the shapeshift bar's form, her power bar's kind and her prowl, the combo
		// points on her target - each told as it changes. Her rage or energy itself goes with her unit snapshot (Bridge).
		void Show()
		{
			const float stealthBonus = g_prowling && g_form == "cat" ? g_talentProfile.value("stealth_level_bonus", 0.f) : 0;
			if (stealthBonus != g_stealthApplied) {
				Carriers::Unbuff(Player(), kFeralStealthBuff);
				if (stealthBonus > 0) Carriers::Buff(Player(), kFeralStealthBuff, "Feral Instinct", {
					{0, stealthBonus, RE::ActorValue::kSneakingModifier}}, 1e7f);
				g_stealthApplied = stealthBonus;
			}
			static int shownForm = -1;
			static bool shownProwl = false, shownStealth = false;
			const bool stealth = g_prowling || g_shadowmeld;
			if (stealth != shownStealth) {   // Prowl and Shadowmeld share Skyrim's invisibility
				Carriers::Stealth(Player(), stealth);
				shownStealth = stealth;
			}
			const int   index = g_form == "bear" || g_form == "dire_bear" ? 1 : g_form == "aquatic" ? 2 : g_form == "cat" ? 3 : g_form == "travel" ? 4 : g_form == "moonkin" ? 5 : 0;
			std::string lua;
			if (index != shownForm || g_prowling != shownProwl) {
				const auto spell = g_form == "bear" ? 5487 : g_form == "dire_bear" ? 9634 : g_form == "aquatic" ? 1066 :
					g_form == "cat" ? 768 : g_form == "travel" ? 783 : g_form == "moonkin" ? 24858 : 0;
				lua += std::format("__WBFormSpell({}, {}, {}) ", spell, PowerType(), g_prowling ? "true" : "false");
				shownForm = index, shownProwl = g_prowling;
			}
			if (!lua.empty()) {
				WowUi::Run(lua);
			}
		}

		bool Swimming() { return Player()->AsActorState()->IsSwimming(); }

		// Skyrim's sneak, toggled as its key does (SneakHandler), when it is not already as wanted.
		void SetSneaking(bool a_on)
		{
			auto* controls = RE::PlayerControls::GetSingleton();
			if (!controls || !controls->sneakHandler || Player()->IsSneaking() == a_on) {
				return;
			}
			if (auto* press = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, RE::UserEvents::GetSingleton()->sneak, 0x1D, 1.0f, 0.0f)) {
				controls->sneakHandler->ProcessButton(press, &controls->data);
				RE::free(press);
			}
		}

		// A form's Skyrim side: its fixed health (converted), armor (a share of what her gear gives), running speed (and
		// Aquatic Form's swimming speed while she swims, its water breathing).
		void FormBuff(const json& a_aura)
		{
			auto*                         player = Player();
			g_swimBoost = Swimming() && a_aura.value("swim_speed_percent", 0.0f) != 0.0f;
			std::vector<Carriers::Change> changes;
			if (const int health = a_aura.value("skyrim_health_bonus", 0); health > 0) {
				changes.push_back({ 0, static_cast<float>(health), RE::ActorValue::kHealth });
			}
			if (const float pct = a_aura.value("armor_percent", 0.0f); pct != 0.0f) {   // of her worn armor's rating
				changes.push_back({ 0, WornArmor() * pct / 100.0f, RE::ActorValue::kDamageResist });
			}
			// Same native avoidance equivalent as Quickness: .12% mitigation per
			// armour point. Feline dodge survives indoor speed suppression.
			if (const float dodge = a_aura.value("dodge_bonus_percent", 0.f); dodge > 0) {
				changes.push_back({0, dodge / .12f, RE::ActorValue::kDamageResist});
			}
			const float speed = a_aura.value("movement_speed_percent", 0.0f) + (g_swimBoost ? a_aura.value("swim_speed_percent", 0.0f) : 0.0f);
			if (speed != 0.0f) {
				changes.push_back({ 0, speed, RE::ActorValue::kSpeedMult });
			}
			if (a_aura.value("water_breathing", false)) {
				changes.push_back({ 0, 1.0f, RE::ActorValue::kWaterBreathing });
			}
			Carriers::Unbuff(player, kFormBuff);
			if (!changes.empty()) {
				Carriers::Buff(player, kFormBuff, a_aura.value("form", std::string("form")), changes, 1e7f);
			}
		}

		void SetForm(const json& a_aura)
		{
			const bool leave = a_aura.value("action", "") == "leave" || a_aura.value("form", "none") == "none";
			g_form = leave ? "none" : a_aura.value("form", "none");
			g_power = leave ? "mana" : a_aura.value("power", "mana");
			for (const auto& [power, value] : a_aura.value("power_set", json::object()).items()) {   // a fresh form: none
				(power == "rage" ? g_rage : g_energy) = value.get<float>();
			}
			if (a_aura.value("clears_prowl", false)) {
				g_prowling = false;
			}
			g_nextSwing = 0;
			g_formAura = leave ? json() : a_aura;
			g_talentProfile = leave ? json::object() : a_aura;
			g_aquaticInWater = g_form == "aquatic" && Swimming();
			if (leave) {
				Carriers::Unbuff(Player(), kFormBuff);
			} else {
				FormBuff(a_aura);
			}
			Avatar::SetForm(leave ? 0 : a_aura.value("display_id", 0u));
			Character::SetShapeshifted(!leave);
			logger::info("forms: {} ({})", g_form, g_power);
			Show();
		}

		// Refresh the current profile without re-entering the form (and without another Furor resource roll).
		// Compare stats with the same caster without ranks: apply only talent gains, using skyrim_effects.STAT's
		// conversions. Spell/melee modifiers and crit remain service-owned; armor and speed use the form carrier.
		void RefreshProfile()
		{
			if (g_profilePending || Frame::Now() < g_nextProfile) {
				return;
			}
			g_nextProfile = Frame::Now() + 1.0;
			auto caster = Character::Caster();
			caster["form"] = g_form;
			caster["race"] = Character::Get().looks.race;
			caster["auras"] = json::array();
			const auto saved = Cast::Snapshot();
			for (const auto& aura : saved["auras"]) {
				caster["auras"].push_back(aura["spell"]);
			}
			if (caster == g_profileAsked) {
				return;
			}
			g_profileAsked = caster;
			g_profilePending = true;
			Service::Request({ { "op", "stats" }, { "caster", caster } }, [caster, epoch = g_profileEpoch](const json& r) {
				if (epoch != g_profileEpoch) {
					return;
				}
				if (!r.value("ok", false)) {
					g_profilePending = false;
					g_profileAsked = json();
					return;
				}
				auto baseline = caster;
				baseline["talents"] = json::object();
				// Moonkin itself requires a legal 31-point allocation. Its own
				// armor carrier is handled above, and it has no base stat bonus;
				// compare passive stats/regen with an unshifted untalented caster.
				if (g_form == "moonkin") baseline["form"] = "none";
				// Improved Mark (Talent.dbc 821) changes the cast's own native carrier.
				// Keep it in both profiles: its 16-versus-12 stats must not also become
				// a passive +6 health/magicka here. Other stat talents still compare
				// against their unlearned baseline, including Heart of the Wild.
				if (caster.value("class", 11) == 11 && caster["talents"].contains("821")) {
					baseline["talents"]["821"] = caster["talents"]["821"];
				}
				Service::Request({ { "op", "stats" }, { "caster", baseline } }, [caster, r, epoch](const json& base) {
					if (epoch != g_profileEpoch) {
						return;
					}
					g_profilePending = false;
					auto now = Character::Caster();
					if (!base.value("ok", false) || caster["form"] != g_form || caster["talents"] != now["talents"] ||
						caster["level"] != now["level"] || caster["indoors"] != now["indoors"] || caster["items"] != now["items"] || caster["equipment_modifiers"] != now["equipment_modifiers"]) {
						g_profileAsked = json();
						return;
					}
					if (g_form != "none") {
						const auto health = g_formAura.value("skyrim_health_bonus", 0);
						g_formAura = r.at("form");
						g_formAura["skyrim_health_bonus"] = health;   // resolve already converted this fixed form bonus
						FormBuff(g_formAura);
					}
					g_talentProfile = r.at("form");
					// The character pane uses the same profile as combat, including
					// crafting. Native armour/resources arrive through Bridge.
					const auto& damage = r["form"]["damage"];
					std::string sheet = std::format("__WBApplyLive({{player={{damage={{{},{},0,0,0,0,1}},stats={{",
						damage.value("min", 0.f), damage.value("max", 0.f));
					int index = 1;
					for (const char* stat : {"str", "agi", "sta", "int", "spi"}) {
						const auto value = r["stats"].value(stat, 0.f);
						sheet += std::format("[{}]={{{},{},0,0}},", index++, value, value);
					}
					const auto power = r["form"].value("attack_power", 0.f);
					const auto rating = r.value("attack_rating", 0);
					sheet += std::format("}},critChance={},attackPower={{{},0,0}},attackSpeed={},attackBothHands={{{},0,0,0}},defense={{{},0}}",
						r.value("melee_crit",0.f), power, r["form"].value("attack_speed_ms", 2000.f) / 1000.f, rating, rating);
					// Skyrim bows use the service's same native weapon profile in the ranged rows.
					const bool ranged = caster["equipment_modifiers"].value("weapon_skill", "") == "Marksman" && g_form == "none";
					sheet += std::format(",rangedAttack={{{},0}},rangedAttackPower={{{},0,0}},rangedDamage={{{},{},{},0,0,1}},resistances={{",
						ranged ? rating : 0, ranged ? power : 0.f,
						ranged ? r["form"].value("attack_speed_ms", 2000.f) / 1000.f : 0.f,
						ranged ? damage.value("min", 0.f) : 0.f, ranged ? damage.value("max", 0.f) : 0.f);
					for (const auto& [school, value] : r["resist"].items()) {
						sheet += std::format("[{}]={{{},{},0,0}},", school, value.get<float>(), value.get<float>());
					}
					WowUi::Run(sheet + "}}})");
					std::vector<Carriers::Change> changes;
					if (g_form == "none") {
						const float armor = r["form"].value("armor_percent", 0.0f) - base["form"].value("armor_percent", 0.0f);
						if (armor != 0) {
							changes.push_back({ 0, WornArmor() * armor / 100.0f, RE::ActorValue::kDamageResist });
						}
					}
					const auto delta = [&](const char* a_stat) { return r["stats"].value(a_stat, 0.0f) - base["stats"].value(a_stat, 0.0f); };
					// Reflection adds its spirit-derived mana inside WoW's five-second
					// rule. Native magicka regeneration continues under Skyrim's rules.
					g_castingRegen = std::max(0.0f, r["regen"].value("per_sec_casting", 0.0f) -
						base["regen"].value("per_sec_casting", 0.0f)) * 0.2f;
					// Strength -> carry weight / weapon damage modifiers; agility -> movement; stamina -> health;
					// intellect -> magicka; spirit -> magicka regeneration, as gear and stat auras do.
					for (const auto& change : std::vector<Carriers::Change>{ { 0x7A0F4, delta("str") },
						{ 0x7A0FF, delta("str") * 0.5f }, { 0x7A106, delta("str") * 0.5f }, { 0x49507, delta("agi") * 1.5f },
						{ 0x493AA, delta("sta") * 1.5f }, { 0x49504, delta("int") * 1.5f }, { 0x7A0FD, delta("spi") } }) {
						if (change.magnitude != 0) {
							changes.push_back(change);
						}
					}
					Carriers::Unbuff(Player(), kTalentBuff);
					if (!changes.empty()) {
						Carriers::Buff(Player(), kTalentBuff, "Talents", changes, 1e7f);
					}
				});
			});
		}

		void MoonkinParty(double a_now)
		{
			if (a_now < g_nextParty) return;
			g_nextParty = a_now + 0.5;
			std::map<RE::FormID, RE::ActorHandle> nearby;
			if (g_form == "moonkin") {
				for (auto& handle : RE::ProcessLists::GetSingleton()->highActorHandles) {
					auto actor = handle.get();
					if (!actor || actor.get() == Player() || actor->IsDead() || !actor->IsPlayerTeammate() ||
						actor->GetPosition().GetDistance(Player()->GetPosition()) > 30 * Avatar::kUnitsPerYard) continue;
					nearby[actor->GetFormID()] = handle;
					if (!g_moonkinParty.contains(actor->GetFormID())) {
						// Followers cast native spells: +3% crit with 150% critical
						// effect becomes its average +1.5% damage/healing potency.
						Carriers::Buff(actor.get(), kMoonkinPartyBuff, "Moonkin Aura", {
							{0, 1.5f, RE::ActorValue::kDestructionPowerModifier},
							{0, 1.5f, RE::ActorValue::kRestorationPowerModifier}}, 1e7f);
					}
				}
			}
			for (const auto& [id, handle] : g_moonkinParty) if (!nearby.contains(id)) {
				if (auto actor = handle.get()) Carriers::Unbuff(actor.get(), kMoonkinPartyBuff);
			}
			g_moonkinParty = std::move(nearby);
			nearby.clear();
			const auto leader = g_talentProfile.value("leader_of_the_pack", json());
			const bool active = !Player()->IsDead() && (g_form == "cat" || g_form == "bear" || g_form == "dire_bear") && leader.is_object();
			if (active) {
				for (auto& handle : RE::ProcessLists::GetSingleton()->highActorHandles) {
					auto actor = handle.get();
					if (!actor || actor.get() == Player() || actor->IsDead() || !actor->IsPlayerTeammate() ||
						actor->GetPosition().GetDistance(Player()->GetPosition()) > leader.value("radius_yd",45.f) * Avatar::kUnitsPerYard) continue;
					nearby[actor->GetFormID()] = handle;
					if (!g_leaderParty.contains(actor->GetFormID())) {
						// Followers' native weapon critical rules differ from WoW's:
						// 3% chance of double damage becomes +3% average weapon damage.
						Carriers::Buff(actor.get(), kLeaderPartyBuff, "Leader of the Pack", {
							{0, leader.value("crit_percent",3.f) / 100.f, RE::ActorValue::kAttackDamageMult}}, 1e7f);
					}
				}
			}
			for (const auto& [id, handle] : g_leaderParty) if (!nearby.contains(id)) {
				if (auto actor = handle.get()) Carriers::Unbuff(actor.get(), kLeaderPartyBuff);
			}
			g_leaderParty = std::move(nearby);
			if (active != g_leaderShown) {
				WowUi::Run(active ? "__WBAuraAdd('player',24932,false,0,{effects={{3,3}}})" : "__WBAuraRemove('player',24932)");
				g_leaderShown = active;
			}
		}
	}

	void Augment(json& a_caster)
	{
		Progression::Augment(a_caster);
		Cast::Augment(a_caster);
		a_caster["form"] = g_form;
		a_caster["power"] = { { "rage", g_rage }, { "energy", g_energy } };
		a_caster["combo_points"] = ComboPoints();
		a_caster["prowling"] = g_prowling;
		a_caster["race"] = Character::Get().looks.race;
		a_caster["swimming"] = Swimming();
		if (g_nextSwing) {
			a_caster["next_swing"] = g_nextSwing;
		}
	}

	void Apply(const json& a_outcome, RE::Actor* a_target)
	{
		for (const auto& a : a_outcome.value("auras", json::array())) {
			const auto type = a.value("type", "");
			const double now = Frame::Now();
			const double end = a.value("duration_ms", 0) < 0 ? 1e12 : now + a.value("duration_ms", 0) / 1000.0;
			if (type == "shapeshift") {
				SetForm(a);
			} else if (type == "stealth") {
				if (a.value("shadowmeld", false)) {
					g_shadowmeld = true;
					g_shadowmeldAt = Player()->GetPosition();
					Melee::Stop(); // an already armed auto-attack must not break the new concealment
				} else {
					g_prowling = a.value("prowling", true);
					g_prowlSpell = a_outcome.value("spell", kProwl);   // its rank
				}
			} else if (type == "periodic_power" && a.value("power", "") == "rage") {
				g_rageAuras.push_back({ a_outcome.value("spell", 0u), now + a.value("amplitude_ms", 1000) / 1000.0, end,
					a.value("amplitude_ms", 1000) / 1000.0, a.value("per_tick", 0.0f) });
			} else if (type == "rage_heal") {
				RageAura r{ a.value("spell", 0u), now + a.value("amplitude_ms", 1000) / 1000.0, end, a.value("amplitude_ms", 1000) / 1000.0 };
				r.maxPerTick = a.value("max_rage_per_tick", 10.0f);
				r.healthPerRage = a.value("health_per_rage", 0.0f);
				r.critMultiplier = a.value("crit_multiplier", 1.5f);
				r.respite = a.value("respite", false);
				for (const auto& t : a.value("ticks", json::array())) {
					r.crits.push_back(t.value("crit", false));
				}
				g_rageAuras.push_back(std::move(r));
			} else if (type == "interrupt_regen") {
				g_noDecayUntil = end;
			}
		}
		for (const auto& e : a_outcome.value("events", json::array())) {
			const auto type = e.value("type", "");
			if (type == "next_swing") {
				g_nextSwing = e.value("spell", 0u);
			} else if (type == "taunt" && a_target) {   // Growl: it turns on her
				a_target->StartCombat(Player());
			} else if (type == "power") {
				if (e.value("power", "") == "rage") {
					g_rage = Cap(g_rage + e.value("amount", 0.0f));
				} else if (e.value("power", "") == "energy") {
					g_energy = Cap(g_energy + e.value("amount", 0.0f));
				}
			}
		}
		if (a_target && !a_target->IsDead()) {
			if (const int gained = a_outcome.value("combo_points", 0); gained > 0) {
				auto& combo = g_combos[a_target->GetFormID()];
				combo.target = a_target->CreateRefHandle();
				combo.points = std::min(5, combo.points + gained);
			}
			if (a_outcome.value("combo_points_spent", 0) > 0) {
				g_combos.erase(a_target->GetFormID());
			}
		}
		if (a_outcome.value("breaks_prowl", false) && g_prowling) {   // an attack ends it, its buff with it
			g_prowling = false;
			Cast::EndAura(g_prowlSpell);
		}
		Show();
	}

	bool Pay(const json& a_outcome)
	{
		const auto power = a_outcome.value("power", "mana");
		if (power != "rage" && power != "energy") {
			return false;
		}
		if (!a_outcome.value("cost_deferred", false)) {   // Maul is paid by the swing it rides on
			auto& pool = power == "rage" ? g_rage : g_energy;
			pool = Cap(pool - a_outcome.value("cost", 0.0f) - a_outcome.value("extra_power_cost", 0.0f) + a_outcome.value("power_refund", 0.0f));
		}
		Show();
		return true;
	}

	void OnMelee(const json& a_reply)
	{
		BreakShadowmeld();
		g_rage = Cap(g_rage + a_reply.value("rage_gained", 0.0f));
		if (a_reply.value("next_swing_consumed", false) || a_reply.contains("next_swing_error")) {
			g_nextSwing = 0;
		}
		if (a_reply.contains("spell")) {   // Maul's swing: its cost now
			Pay(a_reply);
		}
		if (a_reply.value("breaks_prowl", false) && g_prowling) {
			g_prowling = false;
			Cast::EndAura(g_prowlSpell);
		}
		Show();
	}

	void AttackThreat(const json& a_reply, RE::Actor* a_target)
	{
		if (!a_target || a_target->IsDead() || (g_form != "bear" && g_form != "dire_bear")) return;
		const float bonus = a_reply.value("feral_threat_bonus_percent",0.f);
		auto* combat = a_target->GetActorRuntimeData().combatController;
		auto current = combat ? combat->targetHandle.get() : RE::NiPointer<RE::Actor>{};
		if (bonus <= 0 || !current || !current->IsPlayerTeammate() || current.get() == Player()) return;
		if (std::uniform_real_distribution<float>(0,100)(g_threatRng) < bonus) {
			a_target->StopCombat();
			if (a_target->StartCombat(Player())) ++g_threatRedirects;
		}
	}

	void EndProwl()
	{
		g_prowling = false;
		Show();
	}

	void EndStealth(std::uint32_t a_spell)
	{
		if (a_spell == kShadowmeld) {
			g_shadowmeld = false;
		} else if (a_spell == g_prowlSpell) {
			g_prowling = false;
		}
		Show();
	}

	void BreakShadowmeld()
	{
		if (g_shadowmeld) {
			g_shadowmeld = false;
			Cast::EndAura(kShadowmeld);
			Show();
		}
	}

	void Leave()
	{
		if (g_form != "none") {
			SetForm({ { "action", "leave" }, { "form", "none" } });
		}
	}

	void Tick()
	{
		const double now = Frame::Now();
		auto*        player = Player();
		if (!player || !player->Is3DLoaded()) {
			return;
		}
		RefreshProfile();
		MoonkinParty(now);
		const float elapsed = static_cast<float>(std::clamp(now - g_lastRegen, 0.0, 0.25));
		g_lastRegen = now;
		if (g_castingRegen > 0 && Cast::InFiveSecondRule() && !player->IsDead()) {
			player->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kMagicka, g_castingRegen * elapsed);
			g_reflectionRestored += g_castingRegen * elapsed;
		}
		Carriers::Racials();   // native constant abilities, added only when absent (also after a load)
		const float health = player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
		if (g_shadowmeld) {
			const auto* controls = RE::PlayerControls::GetSingleton();
			const auto at = player->GetPosition();
			const bool moving = player->IsMoving() || (controls && (controls->data.moveInputVec.Length() > 0.01f || controls->data.autoMove));
			if (moving || player->IsInMidair() || player->IsDead() ||
				std::hypot(at.x - g_shadowmeldAt.x, at.y - g_shadowmeldAt.y) > 1.0f) {
				BreakShadowmeld();
			}
		}
		if (g_shadowmeld) {
			// Skyrim keeps a remembered combat target despite invisibility. Drop only
			// actors targeting her; repeat while concealed so AI cannot reacquire her.
			if (auto* processes = RE::ProcessLists::GetSingleton()) {
				processes->ForAllActors([player](RE::Actor* actor) {
					auto* combat = actor ? actor->GetActorRuntimeData().combatController : nullptr;
					if (actor != player && combat) {
						const bool attackingHer = combat->targetHandle.get().get() == player;
						if (auto* group = combat->combatGroup) {
							RE::BSWriteLockGuard guard(group->lock);
							for (auto it = group->targets.begin(); it != group->targets.end();) {
								it = it->targetHandle.get().get() == player ? group->targets.erase(it) : std::next(it);
							}
						}
						if (combat->previousTargetHandle.get().get() == player) combat->previousTargetHandle = {};
						if (attackingHer) actor->StopCombat();
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
				processes->StopCombatAndAlarmOnActor(player, true);
			}
		}
		std::erase_if(g_combos, [](const auto& entry) {
			const auto actor = entry.second.target.get();
			return !actor || actor->IsDead() || actor->IsDeleted();
		});
		if (g_form == "aquatic") {
			g_aquaticInWater = g_aquaticInWater || Swimming();
			if (g_aquaticInWater && !Swimming()) {
				Cast::EndAura(1066);
				Leave();
			}
		}
		if (now >= g_nextEnergy) {   // energy comes back in and out of combat
			g_nextEnergy = now + kTick;
			if (g_energy < kMax) {
				g_energy = Cap(g_energy + kEnergyPerTick);
			}
		}
		if (now >= g_nextDecay) {   // rage fades outside combat, unless Enrage holds it
			g_nextDecay = now + kTick;
			if (g_rage > 0 && !player->IsInCombat() && now >= g_noDecayUntil) {
				g_rage = Cap(g_rage - kRageDecayPerTick);
			}
		}
		for (auto it = g_rageAuras.begin(); it != g_rageAuras.end();) {
			auto& r = *it;
			while (now >= r.next && r.next <= r.end + 0.001) {
				if (r.healthPerRage > 0) {   // Frenzied Regeneration: up to its share of rage into health
					const float spent = std::min(r.maxPerTick, std::floor(g_rage * 10.0f) / 10.0f);
					const bool  crit = r.tick < r.crits.size() && r.crits[r.tick];
					if (spent > 0) {
						g_rage = Cap(g_rage - spent);
						const float amount = std::round(spent * r.healthPerRage * (crit ? r.critMultiplier : 1.0f));
						Carriers::Heal(player, amount);
						if (r.respite) player->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kStamina, amount);
					}
				} else {
					g_rage = Cap(g_rage + r.perTick);
				}
				++r.tick;
				r.next += r.every;
			}
			it = now > r.end ? g_rageAuras.erase(it) : std::next(it);
		}
		if (g_formAura.is_object() && g_formAura.value("swim_speed_percent", 0.0f) != 0.0f && Swimming() != g_swimBoost) {
			FormBuff(g_formAura);   // Aquatic Form's speed is a swimming speed
		}
		// Prowl and Shadowmeld use Skyrim's sneak (its detection) and her faded model; leaving sneak ends stealth.
		// A bear, a stag or a seal does not sneak: Skyrim's sneak is left in those forms.
		const bool stealthed = g_prowling || g_shadowmeld;
		if (stealthed != g_sneakProwl) {
			g_sneakProwl = stealthed;
			g_prowlSince = now;
			SetSneaking(stealthed);
			Avatar::SetAlpha(stealthed ? kStealthAlpha : 1.0f);
		} else if (stealthed && now - g_prowlSince > 0.5 &&
			(!player->IsSneaking() || player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kInvisibility) <= 0.0f)) {
			// Outgoing actions explicitly break Shadowmeld through their hooks. A
			// native hit must not reveal the stationary player during concealment.
			if (g_shadowmeld && player->IsSneaking()) {
				Carriers::Stealth(player, true);
			} else {
				// Skyrim also breaks invisibility on native attacks, spells, item use and activation.
				BreakShadowmeld();
				g_prowling = g_sneakProwl = false;
				SetSneaking(false);
				Avatar::SetAlpha(1.0f);
				Cast::EndAura(g_prowlSpell);
			}
		}
		if (!SneakAllowed() && player->IsSneaking()) {
			SetSneaking(false);
		}
		// Rage from damage taken in a bear form: her health's fall, in WoW's units (Skyrim's x4), as the core rewards it.
		if ((g_form == "bear" || g_form == "dire_bear") && g_health >= 0 && health < g_health - 0.5f) {
			Service::Request({ { "op", "rage_from_damage_taken" }, { "damage", std::lround((g_health - health) * 4.0f) }, { "victim_level", player->GetLevel() } },
				[epoch = g_profileEpoch](const json& r) {
					if (epoch == g_profileEpoch && r.value("ok", false)) {
						g_rage = Cap(g_rage + r.value("rage", 0.0f));
						Show();
					}
				});
		}
		g_health = health;
		Show();   // tells the interface only what changed (combo points follow her target)
	}

	void Reset()
	{
		++g_profileEpoch;
		g_profilePending = false;
		g_profileAsked = json();
		g_nextProfile = 0;
		g_castingRegen = g_reflectionRestored = 0;
		g_lastRegen = Frame::Now();
		g_nextParty = 0;
		for (const auto& [id, handle] : g_moonkinParty) if (auto actor = handle.get()) Carriers::Unbuff(actor.get(), kMoonkinPartyBuff);
		g_moonkinParty.clear();
		for (const auto& [id, handle] : g_leaderParty) if (auto actor = handle.get()) Carriers::Unbuff(actor.get(), kLeaderPartyBuff);
		g_leaderParty.clear();
		Carriers::Unbuff(Player(), kFeralStealthBuff);
		g_talentProfile = json::object();
		g_stealthApplied = 0;
		g_threatRedirects = 0;
		if (g_leaderShown) WowUi::Run("__WBAuraRemove('player',24932)");
		g_leaderShown = false;
		Carriers::Unbuff(Player(), kTalentBuff);
		Carriers::Unbuff(Player(), kFormBuff);
		g_form = "none";
		g_power = "mana";
		g_rage = g_energy = 0;
		g_combos.clear();
		g_prowling = false;
		g_shadowmeld = false;
		g_nextSwing = 0;
		g_rageAuras.clear();
		g_health = -1;
		g_formAura = json();
		g_swimBoost = g_sneakProwl = false;
		Avatar::SetAlpha(1.0f);
		Show();
		// Cast::Reset has restored the saved auras. Restore the form first, then its stealth state.
		const auto saved = Cast::Snapshot();
		for (const auto& a : saved["auras"]) {
			if (const auto form = a.value("form_outcome", json()); form.is_object()) {
				Apply(form, nullptr);
				Cast::FormTracking(form);
			}
		}
		for (const auto& a : saved["auras"]) {
			if (const auto outcome = AuraPersistence::StealthOutcome(a.value("spell", 0u), g_form, Character::Get().looks.race); outcome.is_object()) {
				Apply(outcome, nullptr);
			}
		}
	}

	bool SneakAllowed() { return g_form == "none" || g_form == "cat"; }

	json SnapshotForTesting()
	{
		const double now = Frame::Now();
		json saved = {{"form", g_form}, {"power", g_power}, {"form_aura", g_formAura}, {"rage", g_rage}, {"energy", g_energy},
			{"prowling", g_prowling}, {"prowl_spell", g_prowlSpell}, {"shadowmeld", g_shadowmeld},
			{"sneaking", Player()->IsSneaking()},
			{"next_swing", g_nextSwing}, {"energy_in", g_nextEnergy - now}, {"decay_in", g_nextDecay - now},
			{"no_decay_in", g_noDecayUntil - now}, {"aquatic_in_water", g_aquaticInWater},
			{"combos", json::array()}, {"rage_auras", json::array()}};
		for (const auto& [id, combo] : g_combos) saved["combos"].push_back({{"target", id}, {"points", combo.points}});
		for (const auto& a : g_rageAuras) saved["rage_auras"].push_back({{"spell", a.spell}, {"next", a.next - now},
			{"end", a.end - now}, {"every", a.every}, {"per_tick", a.perTick}, {"max_per_tick", a.maxPerTick},
			{"health_per_rage", a.healthPerRage}, {"crits", a.crits}, {"tick", a.tick},
			{"crit_multiplier", a.critMultiplier}, {"respite", a.respite}});
		return saved;
	}

	void RestoreForTesting(const json& a_saved, bool a_resetProfile)
	{
		if (a_resetProfile) {
			Reset();   // also drops stale profile replies and temporary follower carriers
			if (a_saved["form_aura"].is_object()) SetForm(a_saved["form_aura"]);
		}
		const double now = Frame::Now();
		g_power = a_saved["power"].get<std::string>();   // refreshed stats.form omits the stance's resource kind
		g_rage = a_saved["rage"]; g_energy = a_saved["energy"];
		g_health = -1;   // restoring native health is not incoming combat damage
		g_prowling = a_saved["prowling"]; g_shadowmeld = a_saved["shadowmeld"];
		g_sneakProwl = g_prowling || g_shadowmeld;
		g_prowlSince = now;
		SetSneaking(a_saved["sneaking"]);
		Avatar::SetAlpha(g_sneakProwl ? kStealthAlpha : 1.0f);
		if (g_sneakProwl) Carriers::Stealth(Player(), true);
		g_prowlSpell = a_saved["prowl_spell"]; g_nextSwing = a_saved["next_swing"];
		g_shadowmeldAt = Player()->GetPosition();
		g_nextEnergy = now + a_saved["energy_in"].get<double>();
		g_nextDecay = now + a_saved["decay_in"].get<double>();
		g_noDecayUntil = now + a_saved["no_decay_in"].get<double>();
		g_aquaticInWater = a_saved["aquatic_in_water"];
		g_combos.clear();
		for (const auto& c : a_saved["combos"]) if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(c["target"].get<RE::FormID>()); actor && !actor->IsDead())
			g_combos[actor->GetFormID()] = {actor->CreateRefHandle(), c["points"].get<int>()};
		g_rageAuras.clear();
		for (const auto& a : a_saved["rage_auras"]) {
			RageAura restored{a["spell"], now + a["next"].get<double>(), now + a["end"].get<double>(),
				a["every"], a["per_tick"], a["max_per_tick"], a["health_per_rage"], a["crits"].get<std::vector<bool>>(),
				a["tick"], a["crit_multiplier"], a["respite"]};
			g_rageAuras.push_back(std::move(restored));
		}
		Show();
	}

	const std::string& Current() { return g_form; }
	nlohmann::json TalentDiagnostics()
	{
		return {{"reflection_per_second", g_castingRegen}, {"reflection_restored", g_reflectionRestored},
			{"moonkin_followers", g_moonkinParty.size()}, {"leader_followers", g_leaderParty.size()},
			{"stealth_bonus", g_stealthApplied}, {"feral_threat_redirects", g_threatRedirects}, {"profile", g_talentProfile},
			{"profile_ready", !g_profilePending && g_profileAsked.is_object() &&
				g_profileAsked.value("talents", json()) == Character::Caster()["talents"] &&
				g_profileAsked.value("form", "") == g_form}};
	}
	float WornArmor()
	{
		float worn = 0;
		for (const auto& [form, data] : RE::PlayerCharacter::GetSingleton()->GetInventory([](RE::TESBoundObject& a_object) { return a_object.IsArmor(); })) {
			if (data.second && data.second->IsWorn()) {
				worn += RE::PlayerCharacter::GetSingleton()->GetArmorValue(data.second.get());
			}
		}
		return worn;
	}

	int PowerType() { return g_power == "rage" ? 1 : g_power == "energy" ? 3 : 0; }
	int Power() { return static_cast<int>(g_power == "rage" ? g_rage : g_energy); }
	int ComboPoints(RE::Actor* a_target)
	{
		if (!a_target || a_target->IsDead()) return 0;
		const auto found = g_combos.find(a_target->GetFormID());
		return found != g_combos.end() && found->second.target.get().get() == a_target ? found->second.points : 0;
	}
	int ComboPoints() { return ComboPoints(Bridge::Target()); }
}
