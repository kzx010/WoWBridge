#include "Integration.h"
#include "CompassLocations.h"
#include "Mounts.h"
#include "Avatar.h"

#include <psapi.h>
#include "Bridge.h"
#include "Cast.h"
#include "Carriers.h"
#include "Character.h"
#include "Console.h"
#include "Gear.h"
#include "Bags.h"
#include "Merchant.h"
#include "Loot.h"
#include "Trainers.h"
#include "Taxi.h"
#include "SkyrimAuras.h"
#include "Forms.h"
#include "SkyrimSpells.h"
#include "Melee.h"
#include "Progression.h"
#include "Quests.h"
#include "WorldLog.h"
#include "SpellVisuals.h"
#include "ui/WowUi.h"
#include "Config.h"
#include "Frame.h"
#include "Hooks.h"
#include "render/WorldPass.h"
#include "render/ItemPreview.h"
#include "render/GroundItems.h"
#include "ui/UiPass.h"
#include "SelfTest.h"

namespace
{
	// What the save keeps of her WoW state: each module writes its own record and reads it back by its type.
	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
        Carriers::Save(a_intfc);
		Cast::Save(a_intfc);
		Gear::Save(a_intfc);
		Bags::Save(a_intfc);
		Progression::Save(a_intfc);
		SkyrimSpells::Save(a_intfc);
		Character::Save(a_intfc);
		Integration::Save(a_intfc);
        WowUi::SaveProfile(a_intfc);
	}

	void OnLoad(SKSE::SerializationInterface* a_intfc)
	{
		std::uint32_t type = 0, version = 0, length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (type==Carriers::kRecord) {
                Carriers::Load(a_intfc,length,version);
            } else if (type == WowUi::kProfileRecord) {
                WowUi::LoadProfile(a_intfc, length, version);
            } else if (type == Integration::kRecord) {
				Integration::Load(a_intfc, length, version);
			} else if (type == Cast::kRecord && version == Cast::kRecordVersion) {
				Cast::Load(a_intfc, length);
			} else if (type == Gear::kRecord && version == Gear::kRecordVersion) {
				Gear::Load(a_intfc, length);
			} else if (type == Bags::kRecord && (version == 1 || version == Bags::kRecordVersion)) {
				Bags::Load(a_intfc, length, version);
			} else if (type == Progression::kRecord && version == Progression::kRecordVersion) {
				Progression::Load(a_intfc, length);
			} else if (type == Progression::kTalentRecord && version == Progression::kTalentRecordVersion) {
				Progression::LoadTalents(a_intfc, length);
			} else if (type == SkyrimSpells::kRecord && version == SkyrimSpells::kRecordVersion) {
				SkyrimSpells::Load(a_intfc, length);
			} else if (type == Character::kRecord && version == Character::kRecordVersion) {
				Character::LoadSex(a_intfc, length);
			} else {
				logger::warn("save: a record {:08X} v{} this build does not read", type, version);
			}
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			logger::info("kDataLoaded");
			SelfTest::OnDataLoaded();
			Character::Load();
			Integration::Init();
			Gear::Init();
			Bags::Init();
			ItemPreview::Init();
			Merchant::Init(); // native barter; ItemPreview retains its independent menu hook
			Loot::Init();
			Trainers::Init();
			Cast::Init();
			Avatar::Init(Forms::kDisplays);
			Mounts::Init();
			WorldLog::Init();
			SpellVisuals::Init();
			Frame::Until([] {
				Integration::Tick();
				if (!Integration::Active()) return false;
				Bridge::Tick();
				Gear::Tick();
				Bags::Tick();
				Loot::Tick();
				Trainers::Tick();
				Taxi::Tick();
				GroundItems::Tick();
				SkyrimAuras::Tick();
				Progression::Tick();
				WorldLog::Tick();
				Quests::Tick();
				Forms::Tick();
				SkyrimSpells::Tick();
				return false;  // every frame
			});
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
            Carriers::PrepareLoad();
			Integration::PrepareLoad();
            WowUi::PrepareLoad();
			Bridge::ForgetNameplates();
			CompassLocations::Reset();
			GroundItems::Reset();
			Character::PrepareLoad();
			Console::CancelPending();
			ItemPreview::Reset();
			Merchant::Reset();
			Taxi::Reset();
			Mounts::Reset();
			Bags::Reset();
			Loot::Reset();
			Trainers::Reset();
			Gear::Reset();   // a save without its record has not been given the kit
			Progression::Reset();
			Quests::Reset();   // a save without its record starts from her skills, not the last game's book
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			Mounts::Reset();
			logger::info("kPostLoadGame ok={}", a_msg->data != nullptr);
			if (a_msg->data != nullptr) { WowUi::OnGameLoaded(); Integration::OnGameLoaded(); }
            if (a_msg->data!=nullptr) Carriers::OnGameLoaded();
			// Integration applies the saved preset/kit after Skyrim restores gameplay controls.
			Cast::Reset();
			SkyrimAuras::Reset();
			Forms::Reset();
			SkyrimSpells::Reset();
			Progression::Reset();
			Quests::Reset();
			Bridge::ForgetLevel();
			Melee::Reset();
			Avatar::SetWeaponsDrawn(false);   // a login starts with her weapon sheathed
			Bridge::SetTarget(nullptr);
			break;
		case SKSE::MessagingInterface::kSaveGame:
			WowUi::SaveVariables();
			break;
		case SKSE::MessagingInterface::kNewGame:
            Carriers::PrepareLoad();
			Integration::NewGame();
            WowUi::NewGame();
			Bridge::ForgetNameplates();
			CompassLocations::Reset();
			GroundItems::Reset();
			logger::info("kNewGame");
			Character::ResetSex();
			Console::CancelPending();
			Merchant::Reset();
			Taxi::Reset();
			Mounts::Reset();
			Bags::Reset();
			Loot::Reset();
			Trainers::Reset();
			Gear::Reset();
			Gear::OnGameLoaded();
			Cast::Reset();
			SkyrimAuras::Reset();
			Forms::Reset();
			SkyrimSpells::Reset();
			Progression::Reset();
			Quests::Reset();
			Bridge::ForgetLevel();
			Melee::Reset();
			Avatar::SetWeaponsDrawn(false);   // a login starts with her weapon sheathed
			Bridge::SetTarget(nullptr);
			break;
		default:
			break;
		}
	}

	// A fault logged before the game goes (no crash logger is installed): its code and address, as module+offset, and the
	// return addresses into this plugin found on the stack - `llvm-symbolizer --obj=build/skse/WoWBridge.dll 0x180000000+off`
	// names them (the PDB beside it).
	LONG WINAPI OnFault(EXCEPTION_POINTERS* a_info)
	{
		static std::atomic<int> logged = 0;
		const auto code = a_info->ExceptionRecord->ExceptionCode;
		if ((code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
				code != EXCEPTION_INT_DIVIDE_BY_ZERO) || logged++ >= 3) {
			return EXCEPTION_CONTINUE_SEARCH;
		}
		const auto where = [](std::uintptr_t a_at) {
			HMODULE module = nullptr;
			char    name[MAX_PATH] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(a_at), &module)) {
				GetModuleFileNameA(module, name, MAX_PATH);
			}
			return std::format("{}+{:X}", std::strrchr(name, '\\') ? std::strrchr(name, '\\') + 1 : name, a_at - reinterpret_cast<std::uintptr_t>(module));
		};
		const auto at = reinterpret_cast<std::uintptr_t>(a_info->ExceptionRecord->ExceptionAddress);
		logger::critical("fault {:08X} at {} (reading {:X}), thread {}", code, where(at), a_info->ExceptionRecord->ExceptionInformation[1],
			GetCurrentThreadId());
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&OnFault), &self);
		MODULEINFO info{};
		GetModuleInformation(GetCurrentProcess(), self, &info, sizeof(info));
		const auto lo = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll), hi = lo + info.SizeOfImage;
		std::string frames;
		const auto high = reinterpret_cast<std::uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
		const auto* sp = reinterpret_cast<const std::uintptr_t*>(a_info->ContextRecord->Rsp);
		for (int i = 0, found = 0; reinterpret_cast<std::uintptr_t>(sp + i + 1) <= high && found < 24; ++i) {
			if (const auto v = sp[i]; v > lo && v < hi) {
				frames += std::format(" {:X}", v - lo);
				++found;
			}
		}
		logger::critical("fault: WoWBridge.dll return addresses on the stack (offsets):{}", frames);
		spdlog::default_logger()->flush();
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	try {
		SKSE::Init(a_skse);  // CommonLib-NG sets up WoWBridge.log here
		logger::info("runtime {}", REL::Module::get().version().string());
		AddVectoredExceptionHandler(1, OnFault);
		Config::Load();  // needed before the first UI frame (main menu)
		Console::Install();   // testing commands: without them she still plays
		SKSE::AllocTrampoline(1 << 10);
		if (!Hooks::Install() || !WorldPass::Install() || !UiPass::Install()) {
			logger::critical("hook installation failed; plugin inactive");
			return false;
		}
		if (!ItemPreview::Install()) {
			logger::warn("item preview hooks unavailable; native item previews retained");
		}

		auto* saves = SKSE::GetSerializationInterface();
		saves->SetUniqueID('WBRG');
		saves->SetSaveCallback(OnSave);
		saves->SetLoadCallback(OnLoad);

		if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
			logger::critical("messaging listener registration failed");
			return false;
		}
	} catch (const std::exception& e) {
		logger::critical("exception during load: {}", e.what());
		return false;
	}
	logger::info("loaded");
	return true;
}
