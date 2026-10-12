#pragma once

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
	// WoW's stealth (Prowl): Skyrim's invisibility on her until it is taken off (or Skyrim's own rules break it: an attack).
	void Stealth(RE::Actor* a_target, bool a_on);
	void Racials();   // her Night Elf passives: constant native abilities, added once when absent
	bool HasRacial(std::uint32_t a_wowSpell);   // the native passive is installed on her

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
}
