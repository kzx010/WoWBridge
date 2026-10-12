#pragma once

#include <nlohmann/json.hpp>

// The player's WoW casting (docs/authority: the plugin owns controls, the cast/channel lifecycle, the WoW mana pool,
// the global cooldown and cooldowns, and the aura table; the service decides outcomes). Main thread.
//
// A cast: the interface asks (UseAction / CastSpell) -> target chosen as the client does (harmful: the target; helpful:
// a friendly target, else yourself) -> the service resolves it (range, line of sight, target rules, the rolls) ->
// mana and the global cooldown are checked and taken -> the cast bar runs for the cast time (moving interrupts it) ->
// the outcome lands through the carrier spells, auras tick, and the interface hears each step in 1.12's own events
// and words (UI errors, SPELLCAST_*, combat log lines).
namespace Cast
{
	void Init();               // kDataLoaded: carrier spells, the service, the character's mana
	void Tick();               // every frame (Frame::Tick)
	void Reset();              // a save loaded or a new game: nothing cast, placed or applied carries over, but
	                           // what the save kept (Load) comes back
    void RefreshCooldownsForTesting(); // WoW spell/item timers and GCD; preserve casts, auras and native powers
	void ResetForTesting();   // remove native WoW buffs too; do not restore a pending save record
	std::function<bool()> PreserveForTesting();   // restore all runtime auras (including other actors), cooldowns/GCD/FSR
	void FormForTesting(std::uint32_t a_spell, const nlohmann::json& a_outcome);   // resolved form aura; 0 leaves
	// A save keeps what a WoW logout keeps (Player::SaveToDB): her mana, her cooldowns and the auras on her - not those
	// she put on others. A load brings them back as a login does (Player::_LoadAuras, _LoadSpellCooldowns): helpful
	// auras as they were, harmful ones and cooldowns less the real time since the save, ticks on their old timers.
	nlohmann::json Snapshot();
	void           Restore(const nlohmann::json& a_saved);
	constexpr std::uint32_t kRecord = 'CAST', kRecordVersion = 1;
	void           Save(SKSE::SerializationInterface* a_intfc);
	void           Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length);   // its record; Reset restores it
	void Use(std::uint32_t a_spell);
	bool ItemReady(std::uint32_t a_spell);
	void ItemCooldown(std::uint32_t a_spell, std::uint32_t a_ms);
	void Stop();               // SpellStopCasting
	void OnHit(bool a_byYourself);   // you were hit: pushback
	void IncomingHit(RE::Actor* a_attacker, RE::FormID a_source = 0, RE::FormID a_projectile = 0);
	bool InFiveSecondRule();
	void ApplyForTesting(std::uint32_t a_spell, const nlohmann::json& a_outcome, RE::Actor* a_target = nullptr);
	// Supply a real service response to the production timer/completion path.
	void BeginForTesting(std::uint32_t a_spell, const nlohmann::json& a_outcome, RE::Actor* a_target = nullptr);
	nlohmann::json TalentDiagnostics();
	void Damaged(RE::Actor* a_victim, int a_amount);   // she hurt it some other way (a swing): roots on it may break
	// Ground spells (Hurricane): Use enters placement (SpellIsTargeting); a left click on the ground places it, a
	// right click or SpellStopTargeting cancels.
	bool Placing();
	void Place(const RE::NiPoint3& a_point);
	void CancelPlacing();

	int  Mana();
	int  MaxMana();
	bool Casting();
	std::uint32_t TrackingMask();   // PLAYER_TRACK_CREATURES, from active self auras
	void FormTracking(const nlohmann::json& a_outcome);   // Cat entry: learned Track Humanoids, unless Herbs/Minerals
	void EndAura(std::uint32_t a_spell);   // her aura of that spell fades now (as it would at its end)
	void CancelAura(std::uint32_t a_spell);   // the same, for a helpful one only (her right click: CancelPlayerBuff)
	void Augment(nlohmann::json& a_caster);   // active caster auras and remaining talent-proc cooldowns
	void AugmentTarget(nlohmann::json& a_target, RE::Actor* a_actor); // live Faerie Fire armor reduction
	void TalentEffects(const nlohmann::json& a_outcome, RE::Actor* a_target, bool a_consume = true);
}
