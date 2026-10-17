#pragma once

#include <nlohmann/json.hpp>

// Carrier spells: how WoW damage and healing reach Skyrim actors. Skyrim's own spell system delivers them (blame on the
// player, so kill credit works natively) with the amount the gameplay service decided as the magnitude override:
// nothing in Skyrim rolls, resists or absorbs it again. A harmful one is a hit, as any Skyrim attack is (the victim
// fights back, a person struck is assaulted); none flinches or shows a hit effect - what the player sees is WoW's (M8).
// Runtime carrier effects are rebuilt from saved auras. Their native AV modifiers
// may survive a cold load; a scoped co-save ledger prevents applying those twice.
namespace Carriers
{
	bool Create();  // kDataLoaded
    constexpr std::uint32_t kRecord='CARR', kRecordVersion=1;
    void Save(SKSE::SerializationInterface* intfc);
    void Load(SKSE::SerializationInterface* intfc,std::uint32_t length,std::uint32_t version);
    void PrepareLoad();
    void OnGameLoaded(); // before any saved aura/form is recreated
	void Damage(RE::Actor* a_target, float a_skyrimHealth);
	// A harmful WoW outcome on someone not yet against the player raises Skyrim's assault, as a sword or Firebolt does.
	void Provoke(RE::Actor* a_target);
	void Heal(RE::Actor* a_target, float a_skyrimHealth);
	// WoW's root: the actor cannot move but turns and fights. Its speed multiplier is taken away (the carrier), which
	// slows a walk but not Skyrim's combat movement, so the aura also holds the actor where it was rooted (Cast). The
	// WoW aura owns the duration (Unroot when it ends or breaks); the carrier's own duration is only a backstop.
	void Root(RE::Actor* a_target, float a_seconds);
	void Unroot(RE::Actor* a_target);
	// WoW's sleep (Hibernate): Skyrim's paralysis - the beast drops where it stands until it wakes. The WoW aura owns
	// the duration (Wake when it ends or damage breaks it). Its immutable native
	// backstop must not expire before the bridge timer, which clamps frame hitches.
	void Sleep(RE::Actor* a_target, float a_seconds);
	void Wake(RE::Actor* a_target);
	bool IsSleep(RE::FormID a_spell);   // the sleep carrier (its own hit wakes nothing)
	bool IsDamage(RE::FormID a_spell);   // the damage carrier: their WoW damage, which Cast already judged
	// WoW's Soothe Animal, as the user adapted it: an unengaged beast does not attack while it lasts. Skyrim's calm (the
	// vanilla Calm effects' archetype and Aggression), with no native level cap: the service checks the spell's own.
	// The WoW aura owns the duration (Uncalm when it ends or the beast is hurt).
	void Calm(RE::Actor* a_target);
	void Uncalm(RE::Actor* a_target);
	void Fear(RE::Actor* a_target);   // Turn Undead; Cast owns its duration and breaks
	void Unfear(RE::Actor* a_target);
	// WoW's stealth (Prowl): Skyrim's invisibility on the player until it is taken off (or Skyrim's own rules break it: an attack).
	void Stealth(RE::Actor* a_target, bool a_on);
	// Their race's passives (constant native abilities added once when absent; The Human Spirit's Buff, its service share)
	// and their racial auras' native sides while Cast has them on the player (Perception's Detect Life, Stoneform's purge). Every frame.
	void Racials();
	bool Perceiving();   // Perception's Detect Life cast for their current aura (testing)
	bool HasRacial(std::uint32_t a_wowSpell);   // the native passive is installed on the player

	// A WoW buff or debuff's Skyrim side (the service's 'skyrim' list: a vanilla magic effect's actor value and how
	// much): one spell named for the WoW spell, lasting as its aura does, shown in Skyrim's Active Effects. A negative
	// magnitude lowers the value and is harmful (Faerie Fire's armor). The WoW aura owns the duration: Unbuff when it ends.
	struct Change
	{
		RE::FormID     vanilla = 0;   // Skyrim.esm magic effect whose actor value is changed, or
		float          magnitude = 0;
		RE::ActorValue value = RE::ActorValue::kNone;   // the actor value itself (no vanilla effect: a form's speed, armor)
	};
	void Buff(RE::Actor* a_target, std::uint32_t a_wowSpell, const std::string& a_name, const std::vector<Change>& a_changes, float a_seconds);
	void Unbuff(RE::Actor* a_target, std::uint32_t a_wowSpell);
	bool IsBuff(RE::FormID a_nativeSpell, std::uint32_t a_wowSpell);
	bool IsControlCarrier(const RE::MagicItem* a_spell);   // WoWBridge's own root, sleep or fear spell (service-judged)

	// WoW's shapeshift freeing the player (HandleAuraModShapeshift: roots and snares end). Skyrim's side: timed magic lowering
	// their speed multiplier (a frost or poison slow's slowing effect alone: the rest of its spell stays) and the root
	// carrier. Not a constant effect (an ability, a disease, an enchantment) and not a WoW aura's own carrier (Prowl's
	// slow ends with its aura). Returns how many effects ended.
	// Freedom also purges constant and WoW-owned slows; shapeshifting keeps its existing timed-effect rule.
	int Unsnare(RE::Actor* a_target, bool a_all = false);
	// Testing: a native timed value modifier (not a WoW aura's carrier), the live value modifiers by spell name, and
	// the test effects taken off again.
	void NativeForTesting(RE::Actor* a_target, const std::string& a_name, RE::ActorValue a_value, float a_magnitude, float a_seconds);
	void MechanicForTesting(RE::Actor* a_target, RE::EffectSetting::Archetype a_archetype);
	void ClearMechanicsForTesting(RE::Actor* a_target);
	nlohmann::json EffectsForTesting(RE::Actor* a_target);
	void ClearNativeForTesting(RE::Actor* a_target);
	// Testing: Skyrim spell damage (a fire value modifier, not a carrier) that a_caster deals a_target.
	void NativeHitForTesting(RE::Actor* a_caster, RE::Actor* a_target, float a_amount);
}
