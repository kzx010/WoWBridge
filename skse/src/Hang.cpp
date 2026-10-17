#include "Hang.h"

#include <array>
#include <cstring>
#include <dbghelp.h>
#include <psapi.h>

namespace Hang
{
	namespace
	{
		std::atomic<ULONGLONG> g_heartbeat = 0;
		std::atomic<DWORD> g_mainThread = 0;
		std::array<std::atomic<char>, 192> g_checkpoint{};
		std::atomic<unsigned> g_sequence = 0;
		std::atomic<bool> g_loading = true, g_mainMenu = true;

		struct Menus : RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event) {
					if (a_event->menuName == RE::LoadingMenu::MENU_NAME) g_loading.store(a_event->opening);
					if (a_event->menuName == RE::MainMenu::MENU_NAME) g_mainMenu.store(a_event->opening);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		} g_menus;

		std::string LastCheckpoint()
		{
			// Bounded: a hang in the writer itself must never make the watchdog spin.
			for (int attempt = 0; attempt < 3; ++attempt) {
				const auto sequence = g_sequence.load();
				if (sequence & 1) continue;
				std::array<char, g_checkpoint.size()> name{};
				for (std::size_t i = 0; i < name.size(); ++i) name[i] = g_checkpoint[i].load();
				if (sequence == g_sequence.load()) return name.data();
			}
			return "<checkpoint updating>";
		}

		struct Module
		{
			std::uintptr_t base;
			DWORD size;
			std::string name;
		};
		std::vector<Module> Modules()
		{
			std::array<HMODULE, 1024> handles{};
			DWORD needed = 0;
			std::vector<Module> modules;
			if (!EnumProcessModules(GetCurrentProcess(), handles.data(), sizeof(handles), &needed)) return modules;
			for (std::size_t i = 0; i < std::min<std::size_t>(needed / sizeof(HMODULE), handles.size()); ++i) {
				MODULEINFO info{};
				char name[MAX_PATH]{};
				if (!GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info))) continue;
				GetModuleBaseNameA(GetCurrentProcess(), handles[i], name, MAX_PATH);
				modules.push_back({reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll), info.SizeOfImage, name});
			}
			return modules;
		}
		const Module* FindModule(const std::vector<Module>& a_modules, DWORD64 a_at)
		{
			for (const auto& module : a_modules) if (a_at >= module.base && a_at - module.base < module.size) return &module;
			return nullptr;
		}
		std::string Where(const std::vector<Module>& a_modules, DWORD64 a_at)
		{
			if (const auto* module = FindModule(a_modules, a_at)) return std::format("{}+0x{:X}", module->name, a_at - module->base);
			return std::format("unknown+0x{:X}", a_at);
		}

		struct Snapshot
		{
			CONTEXT context{};
			std::array<std::byte, 64 * 1024> stack{};
			SIZE_T size = 0;
		};
		// Only the watchdog uses dbghelp and these callbacks. Never read the live stack after resuming.
		const Snapshot* g_snapshot = nullptr;
		const std::vector<Module>* g_modules = nullptr;
		BOOL CALLBACK ReadStack(HANDLE a_process, DWORD64 a_at, PVOID a_out, DWORD a_size, LPDWORD a_read)
		{
			*a_read = 0;
			const auto& snapshot = *g_snapshot;
			if (a_at >= snapshot.context.Rsp && a_at - snapshot.context.Rsp <= snapshot.size &&
				a_size <= snapshot.size - (a_at - snapshot.context.Rsp)) {
				std::memcpy(a_out, snapshot.stack.data() + (a_at - snapshot.context.Rsp), a_size);
				*a_read = a_size;
				return TRUE;
			}
			// Unwind tables and instructions are in module images, not in the mutable live stack.
			const auto* module = FindModule(*g_modules, a_at);
			if (!module || a_size > module->size - (a_at - module->base)) return FALSE;
			SIZE_T read = 0;
			const auto ok = ReadProcessMemory(a_process, reinterpret_cast<LPCVOID>(a_at), a_out, a_size, &read);
			*a_read = static_cast<DWORD>(read);
			return ok;
		}
		DWORD64 CALLBACK ModuleBase(HANDLE, DWORD64 a_at)
		{
			const auto* module = FindModule(*g_modules, a_at);
			return module ? module->base : 0;
		}

		void Stack(DWORD a_thread, bool a_symbols)
		{
			const auto modules = Modules();
			Snapshot snapshot;  // allocate and touch storage BEFORE suspending; no heap/logger/engine calls while suspended
			const auto thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, a_thread);
			if (!thread) { logger::error("hang: OpenThread {} failed {}", a_thread, GetLastError()); return; }
			struct Handle { HANDLE value; ~Handle() { CloseHandle(value); } } handle{thread};
			if (SuspendThread(thread) == DWORD(-1)) { logger::error("hang: SuspendThread failed {}", GetLastError()); return; }
			BOOL contextOK = FALSE;
			DWORD contextError = 0, resumeError = 0;
			{
				struct Resume
				{
					HANDLE value;
					~Resume() { if (value) ResumeThread(value); }
				} resume{thread};
				snapshot.context.ContextFlags = CONTEXT_FULL;
				contextOK = GetThreadContext(thread, &snapshot.context);
				contextError = contextOK ? 0 : GetLastError();
				if (contextOK) {
					MEMORY_BASIC_INFORMATION region{};
					if (VirtualQuery(reinterpret_cast<LPCVOID>(snapshot.context.Rsp), &region, sizeof(region)) && region.State == MEM_COMMIT &&
						!(region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
						const auto available = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize - snapshot.context.Rsp;
						ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(snapshot.context.Rsp), snapshot.stack.data(),
							std::min<std::size_t>(available, snapshot.stack.size()), &snapshot.size);
					}
				}
				const auto resumed = ResumeThread(thread);
				if (resumed != DWORD(-1)) resume.value = nullptr;
				else resumeError = GetLastError();
			}  // retry a failed resume before any diagnostics or allocation
			if (resumeError) { logger::error("hang: ResumeThread failed {}", resumeError); return; }
			if (!contextOK) { logger::error("hang: GetThreadContext failed {}", contextError); return; }
			logger::error("hang: thread {} RIP {} RSP {:X}, stack snapshot {} bytes", a_thread, Where(modules, snapshot.context.Rip), snapshot.context.Rsp, snapshot.size);
			if (a_symbols) {
				g_snapshot = &snapshot; g_modules = &modules;
				auto context = snapshot.context;
				STACKFRAME64 frame{};
				frame.AddrPC = {context.Rip, 0, AddrModeFlat};
				frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
				frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
				for (int i = 0; i < 48; ++i) {
					const auto pc = frame.AddrPC.Offset, sp = frame.AddrStack.Offset;
					logger::error("hang: stack {} {}", i, Where(modules, pc));
					if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), thread, &frame, &context,
						ReadStack, SymFunctionTableAccess64, ModuleBase, nullptr) || !frame.AddrPC.Offset ||
						(frame.AddrPC.Offset == pc && frame.AddrStack.Offset == sp)) break;
				}
				g_snapshot = nullptr; g_modules = nullptr;
			}
			// Same evidence as OnFault: these are candidates, not a claimed call chain. Also useful if unwind metadata fails.
			std::string own;
			int found = 0;
			for (SIZE_T i = 0; i + sizeof(std::uintptr_t) <= snapshot.size; i += sizeof(std::uintptr_t)) {
				std::uintptr_t value = 0;
				std::memcpy(&value, snapshot.stack.data() + i, sizeof(value));
				if (const auto* module = FindModule(modules, value); module && _stricmp(module->name.c_str(), "WoWBridge.dll") == 0) {
					own += std::format(" {:X}", value - module->base);
					if (++found == 24) break;
				}
			}
			logger::error("hang: WoWBridge.dll return addresses on the stack (offsets):{}", own);
		}

		bool Excluded(RE::UI* a_ui)
		{
			DWORD foreground = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
			// GameIsPaused() is exactly numPausesGame > 0. Copy the scalar without traversing mutable menu containers.
			std::uint32_t pauses = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), &a_ui->numPausesGame, &pauses, sizeof(pauses), nullptr)) return true;
			return pauses || g_loading.load() || g_mainMenu.load() || foreground != GetCurrentProcessId();
		}
		void Watch(RE::UI* a_ui) noexcept
		{
			// One initialization, on the sole dbghelp caller. No symbol downloads or line lookup are needed.
			const bool symbols = SymInitialize(GetCurrentProcess(), "", TRUE) != FALSE;
			ULONGLONG eligible = GetTickCount64(), reported = 0, observed = 0;
			for (;;) {
				Sleep(100);
				try {
					const auto now = GetTickCount64(), heartbeat = g_heartbeat.load();
					if (!heartbeat || Excluded(a_ui)) { eligible = now; reported = 0; continue; }
					if (heartbeat != observed) { observed = heartbeat; reported = 0; }
					const auto baseline = std::max(heartbeat, eligible);
					// 20 s: new games and cell loads keep the main thread busy for 5-15 s legitimately (suite 183934); a hang lasts minutes.
					if (now < baseline || now - baseline < 20000 || (reported && now - reported < 15000)) continue;
					reported = now;
					const auto thread = g_mainThread.load();
					logger::error("hang: main thread silent for {:.1f} s, last checkpoint {}, main thread id {}", (now - heartbeat) / 1000.0, LastCheckpoint(), thread);
					spdlog::default_logger()->flush();
					Stack(thread, symbols);
					spdlog::default_logger()->flush();
				} catch (...) { /* diagnostics must never terminate the game */ }
			}
		}
	}

	void Checkpoint(const char* a_name) noexcept
	{
		if (g_mainThread.load(std::memory_order_relaxed) != GetCurrentThreadId()) return;
		g_sequence.fetch_add(1);
		std::size_t i = 0;
		if (a_name) for (; i + 1 < g_checkpoint.size() && a_name[i]; ++i) g_checkpoint[i].store(a_name[i]);
		g_checkpoint[i].store('\0');
		g_sequence.fetch_add(1);
	}
	void Heartbeat() noexcept
	{
		if (!g_mainThread.load(std::memory_order_relaxed)) g_mainThread.store(GetCurrentThreadId());
		Checkpoint("frame");
		g_heartbeat.store(GetTickCount64());
	}
	void Start() noexcept
	{
		static bool started = false;
		if (started) return;
		try {
			auto* ui = RE::UI::GetSingleton();
			if (!ui) return;
			g_loading.store(ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));
			g_mainMenu.store(ui->IsMenuOpen(RE::MainMenu::MENU_NAME));
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menus);
			std::thread(Watch, ui).detach();
			started = true;
		} catch (...) { /* failure to start diagnostics must not fail plugin initialization */ }
	}
}
