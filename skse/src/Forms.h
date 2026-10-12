#pragma once

#include <nlohmann/json.hpp>

// Her druid forms and their resources (docs/service-contract.md, "Druid forms and feral abilities"). The service is
// stateless: the form she is in, her rage and energy, the combo points on her target, her prowl and a queued Maul
// live here. Every request carries them (Augment); answers change them (Apply, Pay, OnMelee); Tick runs energy and
// rage as the client does.
namespace Forms
{
	// Her forms' creatures (the service's feral.DISPLAY): cat, travel, aquatic, bear and dire bear.
	inline constexpr std::uint32_t kDisplays[] = { 892, 632, 2428, 2281, 15374 };

	void Augment(nlohmann::json& a_caster);   // caster.form, power, combo_points, prowling, next_swing

	// A resolve answer's form part: the shapeshift aura (her form, its power, its Skyrim side), combo points gained or
	// spent on the target, Maul queued, a taunt, Enrage's and Frenzied Regeneration's rage over time.
	void Apply(const nlohmann::json& a_outcome, RE::Actor* a_target);
	// A rage or energy cost paid (true); false leaves the cost to her magicka (a mana spell, a shift).
	bool Pay(const nlohmann::json& a_outcome);
	void OnMelee(const nlohmann::json& a_reply);   // rage gained, Maul's swing consumed and paid, a prowl broken
	void AttackThreat(const nlohmann::json& a_reply, RE::Actor* a_target); // Bear threat attracts a follower's pursuer
	void Leave();                                  // back to her own form (her form's buff taken off)
	void EndProwl();                               // her Prowl's aura ended (cancelled, or with the form)
	void EndStealth(std::uint32_t a_spell);        // the particular stealth aura ended (Prowl or Shadowmeld)
	void BreakShadowmeld();                       // moving or acting removes Shadowmeld (combat entry and incoming damage are allowed)
	void Tick();
	void Reset();   // a load or a new game: caster form
	nlohmann::json TalentDiagnostics();
	nlohmann::json SnapshotForTesting();   // form, resources, combos, stealth and relative periodic timers
	void RestoreForTesting(const nlohmann::json& a_saved, bool a_resetProfile = true);   // after Cast restore; false: resources after profile settles

	const std::string& Current();   // "none", "bear", "dire_bear", "cat", "travel", "aquatic"
	int                PowerType();  // the client's: 0 mana, 1 rage, 3 energy
	float              WornArmor();  // her worn armor's rating (a form's or Enrage's armor share is of it)
	int                Power();      // rage or energy in the client's units (0-100)
	int                ComboPoints(RE::Actor* a_target); // on the request target, independent of current selection
	int                ComboPoints();   // on her current target
	bool               SneakAllowed();  // Skyrim's sneak: herself and the cat only
}
