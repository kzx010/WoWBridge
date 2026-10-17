#pragma once

#include <nlohmann/json.hpp>

// Their druid forms and their resources (docs/service-contract.md, "Druid forms and feral abilities"). The service is
// stateless: the form the player is in, their rage and energy, the combo points on their target, their prowl and a queued Maul
// live here. Every request carries them (Augment); answers change them (Apply, Pay, OnMelee); Tick runs energy and
// rage as the client does.
namespace Forms
{
	// Their forms' creatures (the service's feral.DISPLAY): cat, travel, aquatic, bear and dire bear.
	inline constexpr std::uint32_t kDisplays[] = { 892, 632, 2428, 2281, 15374, 8571, 2289, 15375 };

	void Augment(nlohmann::json& a_caster);   // caster.form, power, combo_points, prowling, next_swing

	// A resolve answer's form part: the shapeshift aura (their form, its power, its Skyrim side), combo points gained or
	// spent on the target, Maul queued, Enrage's and Frenzied Regeneration's rage over time.
	void Apply(const nlohmann::json& a_outcome, RE::Actor* a_target);
	// A rage or energy cost paid (true); false leaves the cost to their magicka (a mana spell, a shift).
	bool CanPay(const nlohmann::json& a_outcome); // live form and full quoted rage/energy cost, before refunds
	bool Pay(const nlohmann::json& a_outcome);
	void OnMelee(const nlohmann::json& a_reply);   // rage gained, Maul's swing consumed and paid, a prowl broken
	void AttackThreat(const nlohmann::json& a_reply, RE::Actor* a_target); // Bear bonus / Paladin Holy multiplier attracts a follower's pursuer
	void Leave();                                  // back to their own form (their form's buff taken off)
	void EndProwl();                               // their Prowl's aura ended (cancelled, or with the form)
	void EndStealth(std::uint32_t a_spell);        // the particular stealth aura ended (Prowl or Shadowmeld)
	void BreakShadowmeld();                       // moving or acting removes Shadowmeld (combat entry and incoming damage are allowed)
	void Tick();
	nlohmann::json SaveResources();   // pools and remaining energy/rage tick schedules
	void LoadResources(const nlohmann::json& a_saved);   // applied after Reset restores the form
	void Reset();   // a load or a new game: caster form
	nlohmann::json TalentDiagnostics();
	void HoldCastingRegenForTesting(bool a_hold);   // no in-rule spirit regen (probes that pay exact magicka)
	float RunSpeedPercent();       // Pursuit of Justice on foot (their talent buff carries it)
	float MountedSpeedPercent();   // ... and its mounted share, multiplying a mount's speed
	nlohmann::json SnapshotForTesting();   // form, resources, combos, stealth and relative periodic timers
	void RestoreForTesting(const nlohmann::json& a_saved, bool a_resetProfile = true);   // after Cast restore; false: resources after profile settles

	const std::string& Current();   // "none", "bear", "dire_bear", "cat", "travel", "aquatic"
	int                PowerType();  // the client's: 0 mana, 1 rage, 3 energy
	float              WornArmor();  // their worn armor's rating (a form's or Enrage's armor share is of it)
	int                Power();      // rage or energy in the client's units (0-100)
	int                ComboPoints(RE::Actor* a_target); // on the request target, independent of current selection
	int                ComboPoints();   // on their current target
	bool               SneakAllowed();  // Skyrim's sneak: the player and the cat only
}
