#include "Mounts.h"
#include "Melee.h"

#include "Forms.h"

#include "Avatar.h"
#include "Bridge.h"
#include "Carriers.h"
#include "Character.h"
#include "Cast.h"
#include "Frame.h"
#include "Gear.h"
#include "LuaText.h"
#include "Service.h"
#include "ui/WowUi.h"

#include <nlohmann/json.hpp>

namespace Melee
{
	namespace
	{
		using json = nlohmann::json;

		constexpr float kUnitsPerYard = 64.0f;

		// What a blow lands on (WeaponImpactSounds' index): plate on heavy armour, mail on light, else flesh.
		std::uint32_t Material(RE::Actor* a_target)
		{
			const auto* body = a_target->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody);
			return !body ? 0u : body->IsHeavyArmor() ? 2u : body->IsLightArmor() ? 1u : 0u;
		}

		// What she strikes with, as the client sounds it (docs/combat-world.md): her form's creature, her weapon (its
		// subclass, wood or metal), else bare hands - taken as the swing starts, so a shift meanwhile does not change it.
		struct Striker
		{
			WowUi::Striker by = WowUi::Striker::Unarmed;
			std::uint32_t  id = 0, parryType = 0;
		};
		Striker StrikingWith()
		{
			const auto& form = Forms::Current();
			if (form != "none") {   // her forms' creatures (Forms::kDisplays)
				return { WowUi::Striker::Creature, form == "cat" ? 892u : form == "travel" ? 632u : form == "aquatic" ? 2428u : 2281u };
			}
			if (const auto hand = Character::InSlot(15); hand && hand->itemClass == 2) {
				return { WowUi::Striker::Weapon, hand->subclass, hand->material == 1 ? 1u : 0u };   // item Material 1: metal
			}
			return {};
		}

		// The blow's sound on what it lands on.
		void Impact(const Striker& a_with, const std::string& a_outcome, RE::Actor* a_target)
		{
			const bool   crit = a_outcome == "crit";
			std::int32_t index = -1;
			if (a_outcome == "hit" || a_outcome == "crit" || a_outcome == "glance") {
				index = static_cast<std::int32_t>(Material(a_target));
			} else if (a_outcome == "block") {
				index = 4;   // a shield (wooden: Skyrim's do not say)
			} else if (a_outcome == "parry") {
				index = 5;   // parried by a metal weapon
			}
			if (index >= 0) {
				const auto at = a_target->GetPosition();
				WowUi::PlaySound(WowUi::Melee(a_with.by, a_with.id, a_with.parryType, static_cast<std::uint32_t>(index), crit).impact, &at);
			}
		}

		RE::ActorHandle  g_target;
		bool             g_attacking = false;
		bool             g_pending = false;      // a swing's roll in flight
		double           g_readyAt = 0;          // the weapon's timer
		std::string      g_lastError;            // shown once until it changes (Player::LastSwingErrorMsg)
		std::uint32_t    g_epoch = 0;
		std::mt19937     g_rng{ std::random_device{}() };

		void Lua(std::string a_lua) { WowUi::Run(std::move(a_lua)); }

		// The client's combat log line (CHAT_MSG_COMBAT_SELF_HITS / _MISSES) in its GlobalStrings.
		void Log(const json& a_r, const std::string& a_name)
		{
			const auto outcome = a_r.value("outcome", "");
			const int  damage = a_r.value("damage", 0), blocked = a_r.value("blocked", 0);
			if (outcome == "miss") {
				Lua(std::format("__WBLog('CHAT_MSG_COMBAT_SELF_MISSES', 'MISSEDSELFOTHER', {})", LuaQuote(a_name)));
			} else if (outcome == "dodge" || outcome == "parry") {
				Lua(std::format("__WBLog('CHAT_MSG_COMBAT_SELF_MISSES', '{}', {})", outcome == "dodge" ? "VSDODGESELFOTHER" : "VSPARRYSELFOTHER", LuaQuote(a_name)));
			} else if (outcome == "block" && damage == 0) {
				Lua(std::format("__WBLog('CHAT_MSG_COMBAT_SELF_MISSES', 'VSBLOCKSELFOTHER', {})", LuaQuote(a_name)));
			} else {
				std::string line = std::format("string.format({}, {}, {})", outcome == "crit" ? "COMBATHITCRITSELFOTHER" : "COMBATHITSELFOTHER", LuaQuote(a_name), damage);
				if (outcome == "glance") {
					line += " .. GLANCING_TRAILER";
				}
				if (blocked > 0) {
					line += std::format(" .. string.format(BLOCK_TRAILER, {})", blocked);
				}
				// All nine chat arguments: ChatFrame_OnEvent reads them (arg1 alone never reached a window).
				Lua(std::format("__WBFire('CHAT_MSG_COMBAT_SELF_HITS', {}, '', '', '', '', '', 0, 0, '')", line));
			}
		}

		void Error(const std::string& a_key)
		{
			if (a_key != g_lastError) {
				g_lastError = a_key;
				if (!a_key.empty()) {
					Lua(std::format("__WBUiError({})", LuaQuote(a_key)));
				}
			}
		}

		void Swing(RE::PlayerCharacter* a_player, RE::Actor* a_target)
		{
			// Unit::UpdateMeleeAttackingState: in reach (both combat reaches and 1.33, at least 5 yards; 3D from a
			// player), in the front 120 degrees, alive.
			const auto  to = a_target->GetPosition() - a_player->GetPosition();
			const float distance = to.Length() / kUnitsPerYard;
			const float h = a_player->GetHeading(false);
			const float along = (std::sin(h) * to.x + std::cos(h) * to.y) / std::max(std::hypot(to.x, to.y), 1e-3f);
			const bool  facing = along >= std::cos(RE::NI_PI / 3.0f);
			const float reach = std::max(1.5f, a_target->GetBoundRadius() / kUnitsPerYard);
			// The target in front of it (from its own heading): it can parry and block.
			const float th = a_target->GetHeading(false);
			const bool  inFront = std::sin(th) * -to.x + std::cos(th) * -to.y >= 0.0f;
			json  target = { { "level", a_target->GetLevel() }, { "distance_yd", distance }, { "facing", facing }, { "in_front", inFront },
				 { "alive", !a_target->IsDead() }, { "reaction", Bridge::ReactionName(a_target) }, { "attackable", Bridge::Attackable(a_target) }, { "combat_reach_yd", reach } };
			Cast::AugmentTarget(target, a_target);
			g_pending = true;
			const RE::ActorHandle handle = a_target->CreateRefHandle();
			auto caster = Character::Caster();
			Forms::Augment(caster);   // in a form she swings its claws; a queued Maul rides on the swing
			Service::Request({ { "op", "melee" }, { "caster", caster }, { "target", target }, { "seed", std::uniform_int_distribution<std::uint32_t>()(g_rng) } },
				[handle, epoch = g_epoch](const json& r) {
					if (epoch != g_epoch) {
						return;
					}
					g_pending = false;
					if (!r.value("ok", false)) {
						const auto err = r.value("error", "");
						if (err == "ERR_BADATTACKPOS" || err == "ERR_BADATTACKFACING") {
							g_readyAt = Frame::Now() + 0.1;   // try again in a tenth of a second
							Error(err);
						} else {
							Error(err == "SERVICE_OFFLINE" ? "" : err);
							Stop();
						}
						return;
					}
					Error("");
					Forms::OnMelee(r);   // rage gained, Maul paid
					Cast::TalentEffects(r, handle.get().get());
					g_readyAt = Frame::Now() + r.value("delay_ms", 2000) / 1000.0;
					// A service reply can arrive after a cast/channel took animation ownership.
					if (!Cast::Casting()) Avatar::Act(static_cast<std::int32_t>(Character::Melee().attack), false, true);
					auto target = handle.get();
					if (!target) {
						return;
					}
					const auto outcome = r.value("outcome", "");
					if (Forms::Current() != "cat" && Forms::Current() != "bear" && Forms::Current() != "dire_bear" &&
						(outcome == "hit" || outcome == "crit" || outcome == "glance" || (outcome == "block" && r.value("damage", 0) > 0))) {
						Gear::EnchantHit(target.get());
					}
					if (const int damage = r.value("damage", 0); damage > 0) {   // in Skyrim's health (service/skyrim.py)
						Carriers::Damage(target.get(), static_cast<float>(damage));
						Cast::Damaged(target.get(), damage);
						Forms::AttackThreat(r, target.get());
						Character::Train(r.value("skill", ""), static_cast<float>(damage));   // a hit trains her weapon's skill
					}
					Log(r, target->GetDisplayFullName());
					// The server deals the blow as it rolls it (above); the client sounds it as her swing animation marks
					// it: the whoosh at its $CSS, the impact at its $HIT (a creature's combat hit, $CAH, in a form), a
					// creature's critical growl as it strikes.
					const auto swing = Character::Melee().attack;
					const auto with = StrikingWith();
					const bool crit = outcome == "crit";
					const auto sounds = WowUi::Melee(with.by, with.id, with.parryType, 0, crit);
					if (with.by == WowUi::Striker::Creature && crit) {
						WowUi::PlaySound(sounds.exertion);
					}
					const auto whoosh = with.by == WowUi::Striker::Creature ? sounds.swing : WowUi::SwingSound(Character::Melee().swingType, crit);
					Frame::After(Avatar::EventAt(swing, "$CSS").value_or(0.0), [whoosh] { WowUi::PlaySound(whoosh); });
					const char* hit = with.by == WowUi::Striker::Creature ? "$CAH" : "$HIT";
					Frame::After(Avatar::EventAt(swing, hit).value_or(0.0), [handle, outcome, with] {
						if (auto victim = handle.get()) {
							Impact(with, outcome, victim.get());
						}
					});
				});
		}
	}

	bool Attacking() { return g_attacking; }

	void Start(RE::Actor* a_target)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!a_target || a_target == player || a_target->IsDead()) {
			Lua("__WBUiError('ERR_INVALID_ATTACK_TARGET')");
			return;
		}
		Mounts::Dismount();
		const bool was = g_attacking;
		g_target = a_target->CreateRefHandle();
		g_attacking = true;
		g_lastError.clear();
		Avatar::SetWeaponsDrawn(true);   // the client draws her weapon as the attack starts
		if (!was) {
			Lua("__WBAttacking(1)");   // PLAYER_ENTER_COMBAT: the Attack action lights
		}
	}

	void Stop()
	{
		if (g_attacking) {
			++g_epoch; // stopping (including Shadowmeld) also cancels a service roll in flight
			g_pending = false;
			g_attacking = false;
			g_target = {};
			Lua("__WBAttacking(nil)");   // PLAYER_LEAVE_COMBAT
		}
	}

	void Toggle(RE::Actor* a_target)
	{
		if (g_attacking) {
			Stop();
		} else {
			Start(a_target);
		}
	}

	void Tick()
	{
		if (!g_attacking) {
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto  target = g_target.get();
		if (!target || target->IsDead() || !target->Is3DLoaded()) {
			Stop();
			return;
		}
		// No swing while a spell is cast (IsNonMeleeSpellCasted); the timer waits ready.
		if (g_pending || Cast::Casting() || Frame::Now() < g_readyAt) {
			return;
		}
		Swing(player, target.get());
	}

	void Reset()
	{
		++g_epoch;
		g_pending = false;
		g_readyAt = 0;
		g_attacking = false;
		g_target = {};
	}
}
