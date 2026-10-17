#pragma once

// The Skyrim player actor stays in the world as an invisible surrogate (collision, AI, interaction).
namespace Surrogate
{
	// Culls only geometry leaves (both 1st- and 3rd-person skeletons), never nodes: the camera,
	// animation graph and collision keep working (SkyCraft's approach, Game.cpp). PlayerWeapons exempts only
	// the selected third-person biped weapon clone's geometry; first-person geometry remains hidden.
	void SetBodyHidden(bool a_hidden);
	bool BodyHidden();
	bool SetSex(bool a_female);   // player base flag and the race's matching default voice
	struct Size
	{
		float modelScale = 1, height = 0;   // yards: first Stand sequence's authored bounds, including display/model scale
		float helm = 0;                      // yards: its Helm attachment (11) in that pose, including the scale
		std::uint32_t display = 0, model = 0;
	};
	std::optional<Size> ClientSize(std::uint32_t a_race, std::uint32_t a_sex);
	// Their 1.12 body in Skyrim: drawn so a Human of their sex stands as tall as Skyrim's average human of that sex (Nord,
	// Imperial, Breton, Redguard), every race in its client proportion to the Human; the surrogate's head where theirs is.
	std::optional<float> DrawScale(std::uint32_t a_race, std::uint32_t a_sex);    // the model's scale (client scale x this)
	std::optional<float> HeadHeight(std::uint32_t a_race, std::uint32_t a_sex);   // Skyrim units: their head node's place
	// Converts their drawn Helm attachment's height to the Skyrim head node's (the camera follows what the surrogate
	// cannot: its size is capped for doorways).
	std::optional<float> HeadFactor(std::uint32_t a_race, std::uint32_t a_sex);
	std::optional<float> ReferenceScale(std::uint32_t a_race, std::uint32_t a_sex);   // what SetSize gives their surrogate
	bool SetSize(std::uint32_t a_race, std::uint32_t a_sex);
	void RestoreSize();
	void PrepareLoad();
	constexpr std::uint32_t kSizeRecord = 'SIZE';   // the unmodified Skyrim reference scale, independent of LOOK
	void SaveSize(SKSE::SerializationInterface* a_intfc);
	void LoadSize(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length, std::uint32_t a_version);
	// Re-applies the hidden state (equipment changes create new geometry). Cheap; call per frame.
	void Refresh();
}
