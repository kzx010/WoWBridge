#pragma once
#include <nlohmann/json_fwd.hpp>
#include <functional>

// Their auto-attack (the Attack ability, 6603), as the 1.12 client and CMaNGOS run it: started on a hostile target
// (right click, the Attack action, AttackTarget), it swings whenever the weapon's timer is ready and the player is not
// casting (Unit::UpdateMeleeAttackingState) - out of reach or facing away the timer waits a tenth of a second and the
// client's error shows once; the service rolls each swing (op melee). Main thread.
namespace Melee
{
	void QueueExtra(int a_count);   // Reckoning, at most five pending swings
	int ExtraAttacks();
	std::function<void(const nlohmann::json&)> CompletionForTesting(RE::Actor* a_target); // capture the production callback before delaying a real reply
	void Start(RE::Actor* a_target);   // a hostile target; another one switches to it
	void Stop();
	void Toggle(RE::Actor* a_target);  // the Attack action
	void Tick();                       // every frame
	void Reset();                      // a load or a new game
	bool Attacking();
}
