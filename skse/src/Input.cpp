#include "Input.h"
#include "Integration.h"

#include "Bridge.h"
#include "Forms.h"
#include "../../wowcore/include/wowcore/combat_feedback.hpp"

#include "ui/WowUi.h"

extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int a_key);   // user32

namespace Input
{
	namespace
	{
		bool                    g_cursorMode = false;
		bool                    g_rmbHeld = false;
		bool                    g_rightUi = false;   // right-button drags on WoW frames move actions, rather than steer
		Buttons                 g_buttons;
		float                   g_x = 0.f, g_y = 0.f;
		float                   g_sensitivity = 1.0f;
		std::set<std::uint32_t> g_boundKeys;
		Stats                   g_stats;

		// The client's name for a DirectInput scan code ("" for keys it has no name for).
		std::string_view KeyName(std::uint32_t a_code)
		{
			static const std::map<std::uint32_t, std::string_view> kNames = {
				{ 0x01, "ESCAPE" }, { 0x0C, "-" }, { 0x0D, "=" }, { 0x0E, "BACKSPACE" }, { 0x0F, "TAB" }, { 0x1A, "[" },
				{ 0x1B, "]" }, { 0x1C, "ENTER" }, { 0x27, ";" }, { 0x28, "'" }, { 0x2B, "\\" }, { 0x33, "," }, { 0x34, "." },
				{ 0x35, "/" }, { 0x37, "NUMPADMULTIPLY" }, { 0x39, "SPACE" }, { 0x3A, "CAPSLOCK" }, { 0x45, "NUMLOCK" },
				{ 0x46, "SCROLLLOCK" }, { 0x47, "NUMPAD7" }, { 0x48, "NUMPAD8" }, { 0x49, "NUMPAD9" }, { 0x4A, "NUMPADMINUS" },
				{ 0x4B, "NUMPAD4" }, { 0x4C, "NUMPAD5" }, { 0x4D, "NUMPAD6" }, { 0x4E, "NUMPADPLUS" }, { 0x4F, "NUMPAD1" },
				{ 0x50, "NUMPAD2" }, { 0x51, "NUMPAD3" }, { 0x52, "NUMPAD0" }, { 0x53, "NUMPADDECIMAL" }, { 0x57, "F11" },
				{ 0x58, "F12" }, { 0x9C, "ENTER" }, { 0xB5, "NUMPADDIVIDE" }, { 0xB7, "PRINTSCREEN" }, { 0xC5, "PAUSE" },
				{ 0xC7, "HOME" }, { 0xC8, "UP" }, { 0xC9, "PAGEUP" }, { 0xCB, "LEFT" }, { 0xCD, "RIGHT" }, { 0xCF, "END" },
				{ 0xD0, "DOWN" }, { 0xD1, "PAGEDOWN" }, { 0xD2, "INSERT" }, { 0xD3, "DELETE" },
			};
			static const char* const kRows[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
			static const std::uint32_t kRowStart[] = { 0x02, 0x10, 0x1E, 0x2C };
			for (int r = 0; r < 4; ++r) {
				const auto n = std::strlen(kRows[r]);
				if (a_code >= kRowStart[r] && a_code < kRowStart[r] + n) {
					return { kRows[r] + (a_code - kRowStart[r]), 1 };
				}
			}
			if (a_code >= 0x3B && a_code <= 0x44) {
				static const char* const kF[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10" };
				return kF[a_code - 0x3B];
			}
			const auto it = kNames.find(a_code);
			return it != kNames.end() ? it->second : std::string_view{};
		}

		// Shift, Ctrl and Alt are WoW's modifiers in play (not Skyrim's run, sneak and sprint).
		std::set<std::uint32_t> g_modifiers;
		bool Shift() { return g_modifiers.contains(0x2A) || g_modifiers.contains(0x36); }
		bool Ctrl() { return g_modifiers.contains(0x1D) || g_modifiers.contains(0x9D); }
		bool Alt() { return g_modifiers.contains(0x38) || g_modifiers.contains(0xB8); }
		// Ctrl also toggles sneak while moving; other shortcuts and clicks consume the tap.
		bool g_ctrlTap = false;
		bool IsModifier(std::uint32_t a_code) { return a_code == 0x2A || a_code == 0x36 || a_code == 0x1D || a_code == 0x9D || a_code == 0x38 || a_code == 0xB8; }

		void PublishModifiers()
		{
			const auto flag = [](bool a_on) { return a_on ? "1" : "nil"; };
			WowUi::Run(std::format("__WB.modifiers = {{ shift = {}, ctrl = {}, alt = {} }}", flag(Shift()), flag(Ctrl()), flag(Alt())));
		}

		// A modifier let go while the game did not have the keyboard (Alt-Tab) never sends its release: it stayed held,
		// and Z became Alt-Z, WoW's hide-the-interface (the user's playtest). Each frame, one no longer down is dropped.
		// Shift+Tab is Skyrim's Tab menu (the user's choice; WoW's previous-enemy is unbound for it). Read from the
		// keyboard's state, not its events: under Wine, Tab held with Shift reaches Windows but never DirectInput (X
		// sends it as ISO_Left_Tab), so the game never hears it. Opened through the menu handler, as the journal is.
		bool GameplayHasFocus();
		void PollShiftTab()
		{
			static bool was = false;
			const bool  now = g_cursorMode && GameplayHasFocus() && (GetAsyncKeyState(0x09) & 0x8000) && (GetAsyncKeyState(0x10) & 0x8000) &&
			                 !(GetAsyncKeyState(0x11) & 0x8000) && !(GetAsyncKeyState(0x12) & 0x8000);   // VK_TAB, SHIFT, CONTROL, MENU
			if (now && !was) {
				SKSE::GetTaskInterface()->AddTask([] { Bridge::OpenSkyrimMenu("Tween"); });
			}
			was = now;
		}

		void SyncModifiers()
		{
			static const std::pair<std::uint32_t, int> kKeys[] = { { 0x2A, 0xA0 }, { 0x36, 0xA1 }, { 0x1D, 0xA2 }, { 0x9D, 0xA3 },
				{ 0x38, 0xA4 }, { 0xB8, 0xA5 } };   // scan code, its virtual key (VK_LSHIFT ... VK_RMENU)
			bool changed = false;
			for (const auto& [code, vk] : kKeys) {
				if (g_modifiers.contains(code) && !(GetAsyncKeyState(vk) & 0x8000)) {
					g_modifiers.erase(code);
					changed = true;
				}
			}
			if (changed) {
				PublishModifiers();
			}
		}

		// Where a key pressed in play went, so its repeats and release go the same way whatever happens in between.
		struct Held
		{
			enum class Kind
			{
				Skyrim,   // unbound in WoW: Skyrim's key
				Remap,    // a WoW movement command, done by Skyrim's own control for it
				Turn,     // keyboard turning (Skyrim has none)
				Swallow,  // nobody's here: Shift+Tab, which PollShiftTab answers
				Wow       // the interface: a binding, the frame or edit box with the keyboard
			} kind = Kind::Skyrim;
			WowUi::KeyRoute::To to = WowUi::KeyRoute::To::Binding;
			std::string         what;        // the command, or the bare key
			RE::BSFixedString   userEvent;   // Remap
			int                 turn = 0;    // Turn: -1 left, +1 right
		};
		std::map<std::uint32_t, Held> g_held;

		// A turning key let go while the game did not hear it (focus away, Wine re-acquiring the keyboard) never sends
		// its release, and a second press of it would count twice: she spun on (the user's playtest). Turning is what is
		// held now, and each frame a turning key no longer down is dropped, as SyncModifiers does.
		void SyncTurning()
		{
			std::erase_if(g_held, [](const auto& a_held) {
				const auto& [code, h] = a_held;
				const UINT  scan = code & 0x80 ? 0xE000 | (code & 0x7F) : code;
				return h.kind == Held::Kind::Turn && !(GetAsyncKeyState(MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX)) & 0x8000);
			});
		}

		// The WoW commands Skyrim's own controls perform.
		const RE::BSFixedString* SkyrimControlFor(const std::string& a_command)
		{
			auto* ue = RE::UserEvents::GetSingleton();
			if (a_command == "MOVEFORWARD") return &ue->forward;
			if (a_command == "MOVEBACKWARD") return &ue->back;
			if (a_command == "STRAFELEFT") return &ue->strafeLeft;
			if (a_command == "STRAFERIGHT") return &ue->strafeRight;
			if (a_command == "JUMP") return &ue->jump;
			if (a_command == "TOGGLEAUTORUN") return &ue->autoMove;
			if (a_command == "TOGGLERUN") return &ue->toggleRun;
			return nullptr;
		}

		void Remap(RE::ButtonEvent* a_button, const RE::BSFixedString& a_userEvent)
		{
			a_button->userEvent = a_userEvent;
			const auto key = RE::ControlMap::GetSingleton()->GetMappedKey(a_userEvent, RE::INPUT_DEVICE::kKeyboard);
			if (key != RE::ControlMap::kInvalid) {
				a_button->idCode = key;
			}
		}

		Held Press(std::uint32_t a_code)
		{
			const std::string bare(KeyName(a_code));
			if (bare.empty()) {
				return {};
			}
			if (bare == "TAB" && Shift() && !Ctrl() && !Alt()) {
				return { Held::Kind::Swallow };   // PollShiftTab opens the Tab menu
			}
			std::string full = bare;
			if (Shift()) full = "SHIFT-" + full;
			if (Ctrl()) full = "CTRL-" + full;
			if (Alt()) full = "ALT-" + full;
			auto route = WowUi::Route(full, bare);
			if (route.to != WowUi::KeyRoute::To::Binding) {
				return { Held::Kind::Wow, route.to, bare };
			}
			// Ctrl's original hidden debug/sound bindings mask movement (W/Q/E/S).
			// In gameplay use the bare movement binding; focused UI/edit boxes above still win.
			if (Ctrl()) {
				const auto bareRoute = WowUi::Route(bare, bare);
				if (wowcore::isMovementCommand(bareRoute.command)) route = bareRoute;
			}
			const auto& command = route.command;
			if (command.empty()) {
				return {};
			}
			// Esc with nothing of WoW's to close or cancel goes to Skyrim: its pause menu, not WoW's game menu.
			if (command == "TOGGLEGAMEMENU" && WowUi::Eval("__WBEscapeHasWork()") != "1") {
				return {};
			}
			// Turning keys strafe while the right button steers, as in the client.
			if (command == "TURNLEFT" || command == "TURNRIGHT") {
				const bool left = command == "TURNLEFT";
				if (g_rmbHeld) {
					auto* ue = RE::UserEvents::GetSingleton();
					return { Held::Kind::Remap, {}, command, left ? ue->strafeLeft : ue->strafeRight };
				}
				return { Held::Kind::Turn, {}, command, {}, left ? -1 : 1 };
			}
			if (const auto* control = SkyrimControlFor(command)) {
				return { Held::Kind::Remap, {}, command, *control };
			}
			return { Held::Kind::Wow, WowUi::KeyRoute::To::Binding, command };
		}

		// A keyboard key in play, while the WoW interface is up. True: Skyrim does not see it.
		bool RouteKey(RE::ButtonEvent* a_button)
		{
			const auto code = a_button->GetIDCode();
			if (code == 0x29) {
				return false;   // the console key stays Skyrim's
			}
			if (IsModifier(code)) {
				const bool ctrl = code == 0x1D || code == 0x9D;
				if (a_button->IsDown()) {
					g_ctrlTap = ctrl;
					g_modifiers.insert(code);
					PublishModifiers();
				} else if (a_button->IsUp()) {
					g_modifiers.erase(code);
					PublishModifiers();
					if (ctrl && g_ctrlTap && Forms::SneakAllowed()) {   // a tap: Skyrim's Sneak, pressed (not as a bear)
						g_ctrlTap = false;
						Remap(a_button, RE::UserEvents::GetSingleton()->sneak);
						a_button->value = 1.0f;
						a_button->heldDownSecs = 0.0f;
						return false;
					}
				}
				return true;
			}
			if (a_button->IsDown()) {
				g_held[code] = Press(code);
				const auto& h = g_held[code];
				if (h.kind == Held::Kind::Wow) {
					WowUi::Key(h.to, h.what, true);
				}
			}
			const auto it = g_held.find(code);
			if (it == g_held.end()) {
				return false;   // held since before the interface was up
			}
			const Held h = it->second;
			if (!wowcore::isMovementCommand(h.what)) g_ctrlTap = false;
			if (a_button->IsUp()) {
				g_held.erase(it);
				if (h.kind == Held::Kind::Wow) {
					WowUi::Key(h.to, h.what, false);
				}
			}
			switch (h.kind) {
			case Held::Kind::Skyrim:
				return false;
			case Held::Kind::Remap:
				Remap(a_button, h.userEvent);
				return false;
			default:
				++g_stats.keysSwallowed;
				return true;
			}
		}

		// Everything held is let go (a menu opened, or the interface went away): bindings hear their release.
		void ReleaseAll()
		{
			for (const auto& [code, h] : g_held) {
				if (h.kind == Held::Kind::Wow) {
					WowUi::Key(h.to, h.what, false);
				}
			}
			g_held.clear();
			g_ctrlTap = false;
			if (!g_modifiers.empty()) {
				g_modifiers.clear();
				PublishModifiers();
			}
		}

		// Typed characters go to the interface's focused edit box, as UTF-8.
		bool RouteChar(const RE::CharEvent* a_char)
		{
			const auto c = a_char->keyCode;
			if (c < 0x20 || c == 0x7F || WowUi::Route({}, {}).to != WowUi::KeyRoute::To::Edit) {
				return false;
			}
			std::string utf8;
			if (c < 0x80) {
				utf8 += static_cast<char>(c);
			} else if (c < 0x800) {
				utf8 += static_cast<char>(0xC0 | (c >> 6)), utf8 += static_cast<char>(0x80 | (c & 0x3F));
			} else {
				utf8 += static_cast<char>(0xE0 | (c >> 12)), utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
				utf8 += static_cast<char>(0x80 | (c & 0x3F));
			}
			WowUi::Text(std::move(utf8));
			return true;
		}

		// Dialogue can own menu-mode input without UsesCursor or PausesGame
		// (including an approaching Courier). Yield before filtering any events.
		bool GameplayHasFocus()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!Integration::Active() || !ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME) ||
				ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
				return false;
			}
			for (const auto& menu : ui->menuStack) {
				if (menu && (menu->UsesCursor() || menu->PausesGame() || menu->UsesMenuContext())) {
					return false;
				}
			}
			return true;
		}
		std::atomic<std::uint64_t> g_captureEpoch = 0;
		std::atomic<bool> g_active = false;   // cursor mode in play (read by the interface's render)
		float             g_rmbTravel = 0;    // mouse travel while the right button is held: a drag steers
		std::atomic<bool> g_leftLook = false; // the left button went down on the world (render thread): its drags turn the camera
		float             g_lmbTravel = 0;
		bool              g_autoMoveOn = false;   // both buttons held: she runs, as in WoW (Skyrim's auto-move set by us)

		std::pair<float, float> ScreenSize()
		{
			const auto s = RE::BSGraphics::Renderer::GetScreenSize();
			return { static_cast<float>(s.width), static_cast<float>(s.height) };
		}

		bool Swallow(RE::InputEvent* a_event)
		{
			if (auto* move = a_event->AsMouseMoveEvent()) {
				if (!g_cursorMode || (g_rmbHeld && !g_rightUi) || g_leftLook) {
					const float travel = std::abs(static_cast<float>(move->mouseInputX)) + std::abs(static_cast<float>(move->mouseInputY));
					g_rmbTravel += travel;
					g_lmbTravel += travel;
					++g_stats.movesPassed;
					return false;
				}
				const auto [w, h] = ScreenSize();
				g_x = std::clamp(g_x + move->mouseInputX * g_sensitivity, 0.f, w - 1.f);
				g_y = std::clamp(g_y + move->mouseInputY * g_sensitivity, 0.f, h - 1.f);
				++g_stats.movesSwallowed;
				return true;
			}
			if (auto* ch = a_event->AsCharEvent()) {
				return g_cursorMode && RouteChar(ch);
			}
			auto* button = a_event->AsButtonEvent();
			if (!button) {
				return false;
			}
			const auto code = button->GetIDCode();
			if (button->GetDevice() == RE::INPUT_DEVICE::kMouse && button->IsDown()) {
				g_ctrlTap = false;   // Ctrl+click: the modifier
			}
			if (button->GetDevice() == RE::INPUT_DEVICE::kMouse && g_cursorMode) {
				if (code == 1) {  // right button: steering modifier
					if (button->IsDown()) {
						g_rmbTravel = 0;
						g_rightUi = WowUi::PointerOverInterface();
					}
					g_rmbHeld = button->IsPressed();
				}
				(code == 0 ? g_buttons.left : code == 1 ? g_buttons.right : g_buttons.middle) = button->IsPressed();
				if (code <= 2) {  // left/right/middle never reach native attack/block in cursor mode
					++g_stats.buttonsSwallowed;
					return true;
				}
				return false;  // wheel (8/9) passes through for zoom
			}
			if (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard && g_boundKeys.contains(code)) {
				++g_stats.keysSwallowed;
				return true;
			}
			if (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard && g_cursorMode && WowUi::Ready()) {
				return RouteKey(button);
			}
			return false;
		}
	}

	void Filter(RE::InputEvent** a_head)
	{
		Update();
		SyncModifiers();
		PollShiftTab();
		if (!GameplayHasFocus() || !g_cursorMode) {
			g_rmbHeld = false;
			g_leftLook = false;
			g_buttons = {};
			if (!g_held.empty() || !g_modifiers.empty()) {
				ReleaseAll();
			}
			if (!GameplayHasFocus()) {
				return;
			}
		}
		RE::InputEvent* prev = nullptr;
		for (auto* event = *a_head; event;) {
			auto* next = event->next;
			if (Swallow(event)) {
				(prev ? prev->next : *a_head) = next;
			} else {
				prev = event;
			}
			event = next;
		}
		SyncTurning();
	}

	void SetCursorMode(bool a_on)
	{
		if (a_on && !g_cursorMode) {
			const auto [w, h] = ScreenSize();
			g_x = w * 0.5f;
			g_y = h * 0.5f;
		}
		g_cursorMode = a_on;
		g_rmbHeld = false;
	}

	void Update()
	{
		const bool active = g_cursorMode && GameplayHasFocus();
		if (g_active && !active) {
			++g_captureEpoch;
			g_rmbHeld = false;
			g_leftLook = false;
			g_buttons = {};
			ReleaseAll();
			SyncTurning();
			UpdateRunByButtons();
		}
		g_active = active;
	}

	bool AltHeld() { return Alt(); }
	bool ShiftHeld() { return Shift(); }

	std::uint64_t CaptureEpoch() { return g_captureEpoch; }
	bool Active() { return g_active; }
	bool RightDragged() { return g_rmbTravel > 6.0f; }
	bool LeftDragged() { return g_lmbTravel > 6.0f; }

	void SetLeftLook(bool a_on)
	{
		g_leftLook = a_on;
		if (a_on) {
			g_lmbTravel = 0;
		}
	}

	// Both buttons held on the world: Skyrim's auto-move, on until either is let go (the right button steers meanwhile).
	void UpdateRunByButtons()
	{
		auto*      controls = RE::PlayerControls::GetSingleton();
		const bool both = g_cursorMode && g_leftLook && g_rmbHeld && g_buttons.left && g_buttons.right;
		if (controls && both != g_autoMoveOn) {
			controls->data.autoMove = both;
			g_autoMoveOn = both;
		}
	}
	bool CursorMode() { return g_cursorMode; }
	std::pair<float, float> Cursor() { return { g_x, g_y }; }
	void SetCursor(float a_x, float a_y) { g_x = a_x, g_y = a_y; }
	bool SteeringHeld() { return g_rmbHeld && !g_rightUi; }
	int Turning()
	{
		int turn = 0;
		for (const auto& [code, h] : g_held) {
			turn += h.turn;
		}
		return std::clamp(turn, -1, 1);
	}
	Buttons MouseButtons() { return g_active ? g_buttons : Buttons{}; }
	void BindKey(std::uint32_t a_scanCode) { g_boundKeys.insert(a_scanCode); }
	void UnbindKey(std::uint32_t a_scanCode) { g_boundKeys.erase(a_scanCode); }
	const Stats& GetStats() { return g_stats; }
	void ResetStats() { g_stats = {}; }
}
