#pragma once

// Skyrim's own magic on their - a potion, a shrine's blessing, a disease, a poison, a Skyrim spell cast on their - shown
// as the 1.12 client shows an aura: an icon in their buff or debuff row with its time, a tooltip of Skyrim's name and
// words, a right click taking a helpful one off. Their gear's constant effects, their race's abilities and the plugin's
// own carriers (the WoW auras show as themselves) are left out.
namespace SkyrimAuras
{
	void Tick();                  // every frame (scans twice a second)
	void Reset();                 // a load or a new game: the interface is told afresh
	void Cancel(RE::FormID a_spell);   // a right click on a helpful one

	// A WoW icon for a Skyrim spell or effect, by what it is (potion, poison, disease, its school's value).
	std::string Icon(const RE::MagicItem* a_spell, const RE::EffectSetting* a_base);
	// Skyrim's words for an effect of a spell: its description with the magnitude and duration in it.
	std::string Words(float a_magnitude, float a_duration, const RE::EffectSetting* a_base);
	// A follower's visible active effects as a Lua list, using the player's visibility/icon rules.
	std::string Snapshot(RE::Actor* a_actor);
	// Whether an effect shows as an aura (the rules above), and its 1.12 dispel type: a disease's Disease, a poison's
	// (or Poison Resist's) Poison, a spell's, scroll's, staff's, power's or shout's Magic; "" for the rest (potions,
	// abilities). The debuff border's colour, and what a WoW dispel of that type removes (Cast).
	bool        Visible(const RE::ActiveEffect* a_effect);
	std::string DispelType(const RE::ActiveEffect* a_effect);
}
