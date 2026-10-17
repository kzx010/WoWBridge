#pragma once

#include "wowcore/character.hpp"

// The player's WoW body (M8): Elunara dressed as the 1.12 client dresses the player (wowcore::dressCharacter), drawn by the
// world pass where the Skyrim player stands - the Skyrim body hidden - and animated from what the player does.
namespace Avatar
{
	// A model with the image each of its batches draws with (nullptr: none).
	struct Part
	{
		wowcore::Model                      model;
		std::vector<const wowcore::Image*>  batchImages;
		const wowcore::Attached*            attached = nullptr;   // nullptr: the body
	};
	struct Loaded
	{
		std::uint32_t                        display = 0;
		wowcore::Look                        look;
		wowcore::Appearance                  appearance; // captured with this dressing, including sex
		std::vector<wowcore::Worn>           items;  // the worn snapshot used to dress this loaded body
		Part                                 body;
		std::vector<Part>                    attached;
		std::map<std::string, wowcore::Image> images;
		wowcore::Image                       selection;   // Textures\UnitSelectTexture.blp: the target's circle
		wowcore::Image                       areaOk, areaBad;   // Interface\SpellShadow: a ground spell's place, in range or not
	};

	// Where and how the player is this frame (the main thread writes, the renderer reads).
	struct Pose
	{
		bool         visible = false;
		RE::NiPoint3 position;          // the player's feet, Skyrim world
		float        facing = 0;        // radians, Skyrim heading (0 north, clockwise)
		std::uint32_t animation = 0;    // AnimationData id: 0 Stand, 4 Walk, 5 Run, 13 WalkBackwards, 37-40 jumping
		std::uint32_t mountDisplay = 0, mountAnimation = 0;
		float        mountTimeMs = 0;
		float        timeMs = 0;        // into the animation (wrapped by the renderer for loops)
		bool         loop = true;
		std::int32_t upperAnimation = -1; // melee overlay, independent of locomotion
		float        actionTimeMs = 0;
		bool         weaponsDrawn = false;
		std::uint32_t nativeWeaponSlots = 0; // actual native items: no legacy mapped-M2 weapon proxy in these slots
		float        speed = 0;         // yards a second, as measured
		float        rate = 1;          // the animation's playback rate
		float        alpha = 1;         // how the player sees the player: less than 1 while stealthed (Prowl)
		// The target's selection circle: radius 0 when there is none.
		RE::NiPoint3          circleAt;          // its feet
		float                 circleRadius = 0;  // Skyrim units
		std::array<float, 3>  circleColor{};
		// A ground spell's area under the pointer while it is placed: radius 0 when none.
		RE::NiPoint3          areaAt;
		float                 areaRadius = 0;
		bool                  areaOk = true;     // in range: Spell-Shadow-Acceptable, else Unacceptable
	};

	void          Init(std::span<const std::uint32_t> a_forms);   // kDataLoaded: dress the player, then their forms, in the background
	void          Redress();   // what the player wears changed (Character::SetWorn): dress the player again in the background
	void          SetForm(std::uint32_t a_display);   // their form: a creature display (0: the player), dressed as Redress does
	void          PrepareMount(std::uint32_t a_display);
	void          SetMount(std::uint32_t a_display);
	void          SetMountFlying(bool a_flying);   // a flying mount in the air: its Fly animation (135), as TBC's
	const Loaded* MountData();
	bool          MountReady(std::uint32_t a_display);
	void          IntegrationChanged(bool active); // restore native camera settings on a native/new game
	void          Suspend(bool a_suspended);   // tests of the bare Skyrim body: neither drawn nor hiding it
	void          Tick();   // main thread, every frame
	// What their spells make the player do (the kits' animations): a loop until stopped (ReadySpell*, ChannelCast*), or once
	// (SpellCast*). Moving and jumping take precedence. -1: nothing.
	void          Act(std::int32_t a_animation, bool a_loop, bool a_upperBody = false);
	void          StopActing();
	void          ActThenLoop(std::int32_t a_once, std::int32_t a_loop);   // sitting down, then sitting (an emote's state)
	std::int32_t  Acting();   // the animation the player acts, -1: none
	// When an event of one of their animations fires, in seconds into it, as their model marks it ("$CSS" a swing's
	// whoosh, "$HIT" the blow landing); nothing before the player is loaded or if the animation has none.
	std::optional<double> EventAt(std::uint32_t a_animation, const std::string& a_id);
	// Their sheath state, as the client keeps it: drawn by auto-attack (and the TOGGLESHEATH binding), and only put
	// away by that binding - it stays drawn after a fight and while the player casts (build/wowref after_*, drawn_cast_*).
	void          SetWeaponsDrawn(bool a_drawn, bool a_animate = false);   // animate: Z's sheath animation
	bool          WeaponsDrawn();
	void          SetCircle(const RE::NiPoint3& a_at, float a_radius, const std::array<float, 3>& a_color);   // radius 0: none
	void          SetArea(const RE::NiPoint3& a_at, float a_radius, bool a_inRange);   // radius 0: none
	void          SetAlpha(float a_alpha);
	Pose          Current();
	const Loaded* Data();   // nullptr until loaded

	constexpr float kUnitsPerYard = 64.0f;
}
