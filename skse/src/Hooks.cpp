#include "Hooks.h"
#include "Integration.h"

#include "Frame.h"
#include "Input.h"
#include "SelfTest.h"

namespace Hooks
{
	bool VerifyCall(std::uintptr_t a_site, std::uintptr_t a_expected, std::string_view a_name)
	{
		const auto* code = reinterpret_cast<const std::uint8_t*>(a_site);
		if (code[0] != 0xE8) {
			logger::error("hook {}: expected call at {:X}, found opcode {:02X}", a_name, a_site, code[0]);
			return false;
		}
		const auto target = a_site + 5 + *reinterpret_cast<const std::int32_t*>(code + 1);
		if (a_expected && target != a_expected) {
			// Another plugin may already have redirected this call; chaining is still safe, but say so.
			logger::warn("hook {}: call target {:X} != expected {:X} (already hooked?)", a_name, target, a_expected);
		}
		return true;
	}

	namespace
	{
		struct PlayerUpdate
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				func(a_this, a_delta);
				Frame::Tick(a_delta);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct InputDispatch
		{
			static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent** a_events)
			{
				if (a_events) {
					Input::Filter(a_events);
				}
				func(a_dispatcher, a_events);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct TestFrontView
		{
			// CommonLib ThirdPersonState.h: primary vtable 0xD, void(bool weaponSheathed).
			// TDM's hook confirms that bypassing this call preserves free rotation when drawn.
			static void thunk(RE::ThirdPersonState* a_this, bool a_weaponSheathed)
			{
				if (!SelfTest::WantFrontView()) { func(a_this, a_weaponSheathed); return; }
				a_this->freeRotationEnabled = true;
				a_this->freeRotation = { RE::NI_PI, 0.f };
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Skyrim's camera shake (Game.ShakeCamera and its own callers: hits, stomps, explosions). WoW's camera never
		// shakes for any of them (the user: the shaking screen when the player is hit is disorienting), so it is replaced
		// outright; calls are only counted.
		std::atomic<std::uint32_t> g_shakes = 0;
		using ShakeFunction = void (*)(float, const RE::NiPoint3&, float);
		ShakeFunction g_nativeShake = nullptr;
		void NoShake(float a_strength, const RE::NiPoint3& a_position, float a_duration)
		{
			if (!Integration::Active()) { g_nativeShake(a_strength, a_position, a_duration); return; }
			if (g_shakes++ < 20) {
				logger::info("camera shake {:.2f} for {:.2f} s held still", a_strength, a_duration);
			}
		}
	}

	std::uint32_t Shakes() { return g_shakes; }

	bool Install()
	{
		REL::Relocation<std::uintptr_t> playerVtbl{ RE::VTABLE_PlayerCharacter[0] };
		PlayerUpdate::func = playerVtbl.write_vfunc(0xAD, PlayerUpdate::thunk);
		logger::info("hook PlayerCharacter::Update installed");
		static bool frontViewInstalled = false;
		if (!frontViewInstalled) {
			REL::Relocation<std::uintptr_t> thirdVtbl{ RE::VTABLE_ThirdPersonState[0] };
			TestFrontView::func = thirdVtbl.write_vfunc(0xD, TestFrontView::thunk);
			frontViewInstalled = true;
			logger::info("hook ThirdPersonState::SetFreeRotationMode installed (self-test only)");
		}

		const auto inputSite = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(67315, 68617) }.address() + 0x7B;
		if (!VerifyCall(inputSite, REL::Relocation<std::uintptr_t>{ REL::ID(68655) }.address(), "input dispatch")) {
			return false;
		}
		InputDispatch::func = SKSE::GetTrampoline().write_call<5>(inputSite, InputDispatch::thunk);
		logger::info("hook input dispatch installed");

		// ShakeCamera begins sub rsp,58h (48 83 EC 58): checked, so another version's code is never overwritten.
		const auto shake = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(32275, 33012) }.address();
		if (std::memcmp(reinterpret_cast<const void*>(shake), "\x48\x83\xEC\x58\x48\xC7\x44\x24\x20\xFE\xFF\xFF\xFF", 13) == 0) {
			// Two complete, position-independent instructions. Resume through an absolute jump.
			auto* gateway = static_cast<std::uint8_t*>(SKSE::GetTrampoline().allocate(27));
			std::memcpy(gateway, reinterpret_cast<const void*>(shake), 13);
			const std::uint8_t jump[] = {0xFF, 0x25, 0, 0, 0, 0};
			std::memcpy(gateway + 13, jump, 6);
			const auto resume = shake + 13;
			std::memcpy(gateway + 19, &resume, 8);
			FlushInstructionCache(GetCurrentProcess(), gateway, 27);
			g_nativeShake = reinterpret_cast<ShakeFunction>(gateway);
			SKSE::GetTrampoline().write_branch<5>(shake, NoShake);
			logger::info("hook camera shake installed");
		} else {
			logger::warn("hook camera shake: unexpected code at {:X}; Skyrim's shakes stay", shake);
		}
		return true;
	}
}
