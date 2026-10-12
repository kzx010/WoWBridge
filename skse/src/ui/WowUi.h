#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;

// The WoW 1.12 interface in Skyrim: wowcore's UiSession (the client's FrameXML over WoWBridge's API) drawn by the
// UI pass, with WoW art and fonts read from the WoW client's own archives (Config::ClientData()).
namespace WowUi
{
	// UI pass, once per frame with the target it draws into. Starts the interface on first use; does nothing if it
	// could not start (the reason is logged once).
	void Render(ID3D11Device* a_device, ID3D11DeviceContext* a_context, int a_width, int a_height);

	bool Ready();
    std::uint64_t Generation(); // changes whenever a fresh Lua interface becomes ready
	constexpr std::uint32_t kProfileRecord = 'UIPR', kProfileVersion = 1;
    void SaveProfile(SKSE::SerializationInterface* intfc);
    void LoadProfile(SKSE::SerializationInterface* intfc, std::uint32_t length, std::uint32_t version);
    void NewGame();
    void PrepareLoad();
    void OnGameLoaded();
    std::string ProfileKey();
    bool SelectProfileForTesting(std::string_view key);
    void ClearPending(); // discard the previous game's queued snapshots, input and commands

	// Main thread -> interface: the live state as a Lua table constructor for __WBApplyLive (wowcore/lua/wowapi_live.lua),
	// applied before the next interface frame. Only the latest one is applied.
	void Publish(std::string a_snapshot);

	// Interface -> main thread: presses the interface did not take (no WoW frame under the cursor), for world picking.
	struct Click
	{
		float x = 0, y = 0;
		int   button = 0;  // 0 left, 1 right
		bool  alt = false; // Alt held as it was let go (the click is handled a frame later)
	};
	std::vector<Click> TakeWorldClicks();
	bool               PointerOverInterface();   // the pointer is on a WoW frame (as last drawn), not the world

	// Main thread: the cursor the world under the pointer calls for ("ATTACK", "SPEAK", ... or "" for the default),
	// shown unless the interface has set its own (SetCursor) or the pointer is over a WoW frame.
	void SetWorldCursor(std::string a_name);
	// Main thread -> interface: Lua run in order before the next interface frame (casting events, auras, the log).
	void Run(std::string a_lua);

	// Keys in play (main thread). Where a key goes, as in the client: a focused edit box takes every key, then a
	// frame listening for keys (the Key Bindings panel), else the key's binding - by its full name ("SHIFT-1"), or
	// by the bare key when the combination has none. command is "" for an unbound key.
	struct KeyRoute
	{
		enum class To
		{
			Binding,
			Frame,
			Edit
		} to = To::Binding;
		std::string command;
	};
	KeyRoute Route(const std::string& a_key, const std::string& a_bareKey);
	// Delivered in order with Run before the next interface frame: a bound command pressed or released, a key for
	// the frame or edit box that has the keyboard (by bare name), typed text.
	void Key(KeyRoute::To a_to, std::string a_keyOrCommand, bool a_down);
	void Text(std::string a_utf8);

	// Interface -> main thread: the commands the interface issued for the game (useAction, castSpell, ...).
	struct Command
	{
		std::string              name;
		std::vector<std::string> args;
	};
	std::vector<Command> TakeCommands();

	// What the main thread needs of a spell's client record (read under the interface's lock).
	struct SpellFacts
	{
		bool        known = false;
		std::string name;
		bool        resurrect = false; // explicit corpse selection; never auto-target the caster
		bool        harmful = false;  // cast at an enemy
		bool        ground = false;   // cast at a place
		bool        self = false;     // on herself or around her, whoever is targeted (Barkskin, a form, Demoralizing Roar)
		float       rangeMax = 0;
		float       speed = 0;        // a missile's yards a second
		float       radius = 0;       // its area's, in yards (the widest of its effects')
	};
	SpellFacts Facts(std::uint32_t a_spell);

	// A spell's visual sounds (SoundEntries IDs) and playing one, at you or in the world; returns a loop's handle.
	struct SpellSounds
	{
		std::uint32_t precast = 0, cast = 0, impact = 0, state = 0, channel = 0;
		std::int32_t  precastAnim = -1, castAnim = -1, channelAnim = -1;   // the caster's animations (AnimationData ids)
	};
	SpellSounds   Sounds(std::uint32_t a_spell);
	// A melee swing's sounds (WeaponSwingSounds2 / WeaponImpactSounds): 0 when there is none.
	std::uint32_t SwingSound(std::uint32_t a_swingType, bool a_crit);
	std::uint32_t ImpactSound(std::uint32_t a_subclass, std::uint32_t a_parryType, std::uint32_t a_index, bool a_crit);
	std::uint64_t PlaySound(std::uint32_t a_soundId, const RE::NiPoint3* a_at = nullptr);
	// A melee blow's sounds by what strikes (docs/combat-world.md): a creature display (her form), a weapon subclass
	// (parry type 0 wood, 1 metal) or bare hands; a_index: what it lands on (0 flesh, 1 chain, 2 plate, 4 wood shield, 5
	// parried by metal).
	struct MeleeSounds
	{
		std::uint32_t swing = 0, impact = 0, exertion = 0;
	};
	enum class Striker
	{
		Unarmed,
		Weapon,
		Creature
	};
	MeleeSounds Melee(Striker a_by, std::uint32_t a_id, std::uint32_t a_parryType, std::uint32_t a_index, bool a_crit);

	// Saved variables and the character's layout cache (also flushed by Lua logout/reload requests).
	void SaveVariables();
	std::filesystem::path WtfFile(std::string_view a_file, bool a_character = false);

	// Lua in the interface's state (for the console and the in-game tests); returns the first result as text.
	std::string Eval(const std::string& a_lua);
	// SelfTest only: synthetic widget input on the render thread, without desktop
	// input. An empty source uses the existing cursor; an empty target is the world.
	void ProbeDrag(std::string a_source = {}, std::string a_target = {});
}
