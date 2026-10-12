#pragma once

// World picking for cursor control: screen pixel -> camera ray -> Havok ray cast -> reference.
namespace Picker
{
	struct Hit
	{
		bool               hit = false;
		RE::NiPoint3       pos;
		RE::NiPoint3       normal;
		float              distance = 0.f;
		RE::TESObjectREFR* ref = nullptr;
		int                layer = -1;
		RE::bhkRigidBody*  body = nullptr;   // what was hit, when it is a rigid body (what Skyrim's grab holds)
	};

	// Pixel coordinates of the game's render target (origin top-left).
	bool ScreenRay(float a_x, float a_y, RE::NiPoint3& a_origin, RE::NiPoint3& a_dir);
	// a_ground: the ground only - units do not stop it (where a ground spell goes under the pointer, as in WoW).
	// a_ignore: a reference passed through (the thing she holds, under the pointer that moves it).
	Hit  Cast(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, bool a_ground = false, const RE::TESObjectREFR* a_ignore = nullptr);
	Hit  PickScreen(float a_x, float a_y, float a_maxDistance = 8000.f, bool a_ground = false, const RE::TESObjectREFR* a_ignore = nullptr);
	// World -> render-target pixel (origin top-left); false if behind the camera.
	bool WorldToScreen(const RE::NiPoint3& a_world, float& a_x, float& a_y);
}
