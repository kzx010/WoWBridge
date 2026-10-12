#pragma once

#include "wowcore/effect.hpp"
#include "wowcore/image.hpp"

// WoW spell visuals in Skyrim (M8): what a spell's visual kits put on - the precast, cast, impact, state and channel
// effect models, on her (the avatar's attachment points) or on its Skyrim target (the skeleton's nodes) - and its
// missile in flight. The slice's spells' models are loaded ahead; the casting starts and stops them (main thread); the
// world models pass runs and draws them (render thread, WowModels).
namespace SpellVisuals
{
	enum class Kit
	{
		Precast,
		Cast,
		Impact,
		State,
		Channel
	};
	using Handle = std::uint64_t;

	void Init();   // kDataLoaded: the slice spells' effect models, in the background
	void Want(const std::vector<std::uint32_t>& a_spells);   // more spells' (her class's ranks): loaded again in the background
	inline constexpr std::uint32_t kLevelUp = 0xFFFFFF01;   // Play(kLevelUp, Kit::Impact): her level up's light

	// On her (a_on empty) or on a Skyrim actor; lasting: until Stop (precast, state, channel), else once, its own length
	// including its particles (the model instance goes at the sequence's end).
	Handle Play(std::uint32_t a_spell, Kit a_kit, RE::ActorHandle a_on, bool a_lasting);
	void   Stop(Handle a_handle);   // its emitters stop; what they gave off finishes
	// The missile, from her right hand to the target's chest in a_seconds (the spell's flight).
	void   Missile(std::uint32_t a_spell, RE::ActorHandle a_target, float a_seconds);
	// A ground spell's area effect (Hurricane's storm), stood at a_at until Stop; sized to its radius where the model
	// says how big it is made.
	Handle Area(std::uint32_t a_spell, const RE::NiPoint3& a_at, float a_radiusYards);
	void   Clear();   // a load or a new game

	// Render thread: what to draw this frame.
	struct Asset
	{
		wowcore::Model                      model;
		std::vector<const wowcore::Image*>  batchImages;     // per batch
		std::vector<const wowcore::Image*>  textureImages;   // per texture (particles, ribbons)
		float                               scale = 1.0f;
	};
	struct DrawModel
	{
		const Asset*            asset = nullptr;
		std::vector<glm::mat4>  bones;
		glm::mat4               world;     // absolute Skyrim world
		float                   timeMs = 0;
		int                     sequence = 0;
	};
	struct DrawQuads
	{
		const Asset*                      asset = nullptr;
		std::vector<wowcore::EffectQuad>  quads;   // absolute Skyrim world
	};
	struct Her
	{
		const wowcore::Model*          body = nullptr;
		const std::vector<glm::mat4>*  bones = nullptr;
		glm::mat4                      world{ 1.0f };   // absolute
		bool                           visible = false;
	};
	void Step(float a_dt, const Her& a_her, const glm::vec3& a_eye, const glm::vec3& a_right, const glm::vec3& a_up,
		std::vector<DrawModel>& a_models, std::vector<DrawQuads>& a_quads);
	std::uint32_t Running();   // tests
}
