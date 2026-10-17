#pragma once

// Gameplay input filter (runs on the main thread inside the engine's input dispatch).
// In cursor mode, mouse-look is converted into a free cursor and native mouse buttons are swallowed;
// holding the right mouse button passes mouse-look through (WoW-style steering). Keys go where the client would send
// them (WowUi::Route): WoW's movement commands are done by Skyrim's own controls for them (W/S/Q/E, Space, ...), A/D
// turn, the other bound keys run their FrameXML bindings, and keys unbound in WoW stay Skyrim's.
namespace Input
{
	struct Stats
	{
		std::uint64_t movesSwallowed = 0;
		std::uint64_t movesPassed = 0;
		std::uint64_t keysSwallowed = 0;
		std::uint64_t buttonsSwallowed = 0;
		std::uint64_t buttonsReconciled = 0;   // button-ups the OS knew about and the game never delivered
	};

	void Filter(RE::InputEvent** a_head);

	void SetCursorMode(bool a_on);
	bool CursorMode();
	void Update();          // main thread, every frame: whether a Skyrim menu has suspended the WoW cursor
	std::uint64_t CaptureEpoch(); // changes when a native menu/focus loss interrupted a pointer gesture
	bool Active();          // cursor mode, in play (no Skyrim menu has the mouse): the WoW cursor is drawn and used
	bool RightDragged();    // the right button moved the mouse since it went down (steering, not a click)
	bool LeftDragged();     // the same for the left button (a camera turn, not a click)
	void SetLeftLook(bool a_on);   // the left button is down on the world (the interface says): its drags turn the camera
	void UpdateRunByButtons();     // every frame: both buttons held on the world run the player forward, as in WoW
	std::pair<float, float> Cursor();  // screen pixels, origin top-left
	void SetCursor(float a_x, float a_y);
	bool SteeringHeld();
	bool AltHeld();    // WoW's Alt modifier held (Alt+right click attacks, Alt+left click grabs)
	bool ShiftHeld();  // captured when opening loot, before a delayed UI snapshot
	int  Turning();   // keyboard turning held: -1 left, +1 right, 0 none
	struct Buttons
	{
		bool left = false, right = false, middle = false;
	};
	Buttons MouseButtons();  // held now, in cursor mode (the WoW interface reads them each frame)

	void BindKey(std::uint32_t a_scanCode);  // keyboard scan codes swallowed during gameplay (tests)
	void UnbindKey(std::uint32_t a_scanCode);
	const Stats& GetStats();
	void ResetStats();
}
