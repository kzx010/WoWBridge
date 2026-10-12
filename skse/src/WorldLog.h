#pragma once

// The combat around her in the chat (docs/combat-world.md; the user: "the combat log should record all entity combat and
// deaths in a specified radius around the player"): other units' blows and spells on each other and on her, deaths, and
// her killing blows, within kRadius yards - as the 1.12 client words them (__WBCombatEvent). Her own blows and spells
// are logged where they are dealt (Melee, Cast). Main thread.
namespace WorldLog
{
	inline constexpr float kRadius = 50.0f;   // yards
	void Init();   // the death events
	void Tick();   // the health of the units around her, to measure what a blow took
	void Struck(RE::Actor* a_victim, RE::Actor* a_attacker, RE::FormID a_source, bool a_blocked);   // a Skyrim hit
}
