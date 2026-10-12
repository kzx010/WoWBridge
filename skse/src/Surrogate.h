#pragma once

// The Skyrim player actor stays in the world as an invisible surrogate (collision, AI, interaction).
namespace Surrogate
{
	// Culls only geometry leaves (both 1st- and 3rd-person skeletons), never nodes: the camera,
	// animation graph and collision keep working (SkyCraft's approach, Game.cpp).
	void SetBodyHidden(bool a_hidden);
	bool BodyHidden();
	bool SetSex(bool a_female);   // player base flag and the race's matching default voice
	// Re-applies the hidden state (equipment changes create new geometry). Cheap; call per frame.
	void Refresh();
}
