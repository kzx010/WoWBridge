#pragma once

// Skyrim -> the WoW interface, on the main thread each frame: the player's and target's state, where the player is
// and the time of day, published to WowUi as a snapshot; clicks the interface did not take become WoW targeting
// (left: target the actor under the cursor, or clear the target on empty ground).
namespace Bridge
{
	void Tick();
	void IntegrationChanged(bool active);

	void        ForgetLevel();   // a load or a new game: their level is shown again without a level up
	void        ForgetNameplates(); // before loading: invalidate queued plate clicks and remove copied world plates
	// What a person or creature says, as the client takes a creature's speech: "<name> says: ..." in chat and, near the player,
	// its chat bubble (__WBSay: the actor's form ID, as its nameplate row's guid, and yards from the player).
	void        Say(RE::TESObjectREFR* a_speaker, std::string_view a_text);
	RE::Actor* Target();
	void       SetTarget(RE::Actor* a_actor);  // nullptr clears
	RE::Actor* PartyMember(std::size_t a_index);   // main thread, 0-3; only a current teammate
	const char* WorldOffer(RE::TESObjectREFR* a_ref, bool a_alt = false);
	std::string WorldActivationHint(RE::TESObjectREFR* a_ref);
	RE::Actor* Highlighted();                  // the unit under the pointer, brightened (nullptr: none)

	// A Skyrim actor's 1.12 reaction to the player: 2 hostile (red: hostile to the player now), 5 friendly (green: a person - Skyrim's
	// ActorTypeNPC - or their follower; healable, the speak cursor), 4 neutral (yellow: a creature that is not hostile,
	// attackable). ReactionName: the service's words for the same ("hostile", "friendly", "neutral").
	int         Reaction(RE::Actor* a_actor);
	const char* ReactionName(RE::Actor* a_actor);
	const char* CreatureTypeName(RE::Actor* a_actor);   // the 1.12 client's word for it (Beast, Humanoid, ...)
	// Whether the player may strike it: in Skyrim anyone but the player - a townsperson is green and still attackable (the
	// user's direction), which WoW's reactions alone cannot say; harmful spells and swings ask this, heals the reaction.
	bool        Attackable(RE::Actor* a_actor);
	// One of Skyrim's menus, as its own control opens it: "Quests" (the journal), "Tween" (the Tab menu), "System" (the
	// pause menu), or a menu by name.
	void        OpenSkyrimMenu(const std::string& a_name);
	RE::NiPoint3 HoldTarget();   // where what the player holds is pulled, under the pointer
	bool         Holding();      // the player holds a loose thing (Alt+left click)
	// Whether their helpful spells may land on it: anything not hostile to the player (a townsperson, a follower, a chicken).
	bool        Healable(RE::Actor* a_actor);
}
