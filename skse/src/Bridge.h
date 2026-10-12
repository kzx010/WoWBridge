#pragma once

// Skyrim -> the WoW interface, on the main thread each frame: the player's and target's state, where the player is
// and the time of day, published to WowUi as a snapshot; clicks the interface did not take become WoW targeting
// (left: target the actor under the cursor, or clear the target on empty ground).
namespace Bridge
{
	void Tick();
	void IntegrationChanged(bool active);

	void        ForgetLevel();   // a load or a new game: her level is shown again without a level up
	void        ForgetNameplates(); // before loading: invalidate queued plate clicks and remove copied world plates
	RE::Actor* Target();
	void       SetTarget(RE::Actor* a_actor);  // nullptr clears
	RE::Actor* PartyMember(std::size_t a_index);   // main thread, 0-3; only a current teammate
	const char* WorldOffer(RE::TESObjectREFR* a_ref, bool a_alt = false);
	std::string WorldActivationHint(RE::TESObjectREFR* a_ref);
	RE::Actor* Highlighted();                  // the unit under the pointer, brightened (nullptr: none)

	// A Skyrim actor's 1.12 reaction to her: 2 hostile (red: hostile to her now), 5 friendly (green: a person - Skyrim's
	// ActorTypeNPC - or her follower; healable, the speak cursor), 4 neutral (yellow: a creature that is not hostile,
	// attackable). ReactionName: the service's words for the same ("hostile", "friendly", "neutral").
	int         Reaction(RE::Actor* a_actor);
	const char* ReactionName(RE::Actor* a_actor);
	const char* CreatureTypeName(RE::Actor* a_actor);   // the 1.12 client's word for it (Beast, Humanoid, ...)
	// Whether she may strike it: in Skyrim anyone but herself - a townsperson is green and still attackable (the
	// user's direction), which WoW's reactions alone cannot say; harmful spells and swings ask this, heals the reaction.
	bool        Attackable(RE::Actor* a_actor);
	// One of Skyrim's menus, as its own control opens it: "Quests" (the journal), "Tween" (the Tab menu), "System" (the
	// pause menu), or a menu by name.
	void        OpenSkyrimMenu(const std::string& a_name);
	RE::NiPoint3 HoldTarget();   // where what she holds is pulled, under the pointer
	bool         Holding();      // she holds a loose thing (Alt+left click)
	// Whether her helpful spells may land on it: anything not hostile to her (a townsperson, a follower, a chicken).
	bool        Healable(RE::Actor* a_actor);
}
