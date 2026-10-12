#pragma once

#include <glm/glm.hpp>

// The target's face for the TargetFrame portrait (M8): a Skyrim actor's head - the head parts under its face-gen node,
// skinned as they stand when it becomes the target (the 1.12 client's portraits are likewise a still) - for WowModels to
// draw as the client draws a unit's portrait.
namespace Portrait
{
	struct Part
	{
		std::vector<glm::vec3>             positions, normals;   // head space: origin at the head, +y its facing, +z up
		std::vector<glm::vec2>             uvs;
		std::vector<std::uint16_t>         indices;
		RE::NiPointer<RE::NiSourceTexture> diffuse, tint;        // tint: the face-gen tint (the head)
		glm::vec3                          color{ 1.0f };        // the hair's tint
		bool                               alphaKey = false, blended = false;
	};
	struct Head
	{
		std::vector<Part> parts;
		glm::vec3         centre{ 0.0f };   // of the head mesh, head space
		float             radius = 0;
		std::uint32_t     serial = 0;
		RE::FormID        actor = 0;
	};

	// Slot 0 is the target; slots 1-4 are party members. Each uses the same head extraction.
	void Capture(RE::Actor* a_actor, std::size_t a_slot = 0);
	void Update();                      // main thread: retry heads whose 3D was not loaded yet
	std::optional<RE::NiPoint3> HeadPosition(RE::Actor* a_actor); // main thread: the same humanoid/creature head node
	std::shared_ptr<const Head> Current(std::size_t a_slot = 0);   // any thread
}
