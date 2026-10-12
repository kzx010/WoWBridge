#include "ui/UiPass.h"

#include "Config.h"
#include "Integration.h"
#include "render/WorldPass.h"
#include "Hooks.h"
#include "Input.h"
#include "ui/WowUi.h"
#include "ui/imgui_impl_dx11.h"

#include <d3d11.h>
#include <imgui.h>
#include <wowcore/mpq.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace UiPass
{
	namespace
	{
		std::mutex g_lock;
		Info       g_info;
		bool       g_debugOverlay = false;
		bool       g_imguiReady = false;
		ImFont*    g_friz = nullptr;
		std::vector<std::uint8_t> g_fontData;

		struct PendingCapture
		{
			ID3D11Texture2D* staging = nullptr;
			DXGI_FORMAT      format{};
			Capture          result;
		};
		std::map<std::string, PendingCapture> g_captures;

		template <class T>
		void Release(T*& a_p)
		{
			if (a_p) {
				a_p->Release();
				a_p = nullptr;
			}
		}

		bool InitImGui(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
		{
			IMGUI_CHECKVERSION();
			ImGui::CreateContext();
			auto& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			if (!ImGui_ImplDX11_Init(a_device, a_context)) {
				logger::error("ui pass: ImGui DX11 backend init failed");
				return false;
			}
			try {
				const wowcore::Mpq mpq(Config::ClientData());
				if (auto data = mpq.read(R"(Fonts\FRIZQT__.TTF)")) {
					g_fontData = std::move(*data);
					ImFontConfig font;
					font.FontDataOwnedByAtlas = false;
					g_friz = io.Fonts->AddFontFromMemoryTTF(g_fontData.data(), static_cast<int>(g_fontData.size()), 32.0f, &font);
				}
			} catch (const std::exception& e) {
				logger::warn("ui pass: client font unavailable: {}", e.what());
			}
			g_info.fontLoaded = g_friz != nullptr;
			logger::info("ui pass: ImGui {} ready; client font {}", IMGUI_VERSION, g_friz ? "loaded" : "MISSING");
			return true;
		}

		void LogBoundTarget(ID3D11DeviceContext* a_context)
		{
			ID3D11RenderTargetView* bound = nullptr;
			a_context->OMGetRenderTargets(1, &bound, nullptr);
			auto& rd = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData();
			g_info.boundIsFramebuffer = bound && bound == reinterpret_cast<ID3D11RenderTargetView*>(rd.renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER].RTV);
			int index = bound ? -2 : -1;
			for (int i = 0; bound && i < RE::RENDER_TARGETS::kTOTAL; ++i) {
				if (reinterpret_cast<ID3D11RenderTargetView*>(rd.renderTargets[i].RTV) == bound) {
					index = i;
				}
			}
			logger::info("ui pass: bound render target index {} (framebuffer={})", index, g_info.boundIsFramebuffer);
			Release(bound);
		}

		ID3D11Texture2D* BoundTexture(ID3D11DeviceContext* a_context)
		{
			ID3D11RenderTargetView* rtv = nullptr;
			a_context->OMGetRenderTargets(1, &rtv, nullptr);
			if (!rtv) {
				return nullptr;
			}
			ID3D11Resource* res = nullptr;
			rtv->GetResource(&res);
			Release(rtv);
			ID3D11Texture2D* tex = nullptr;
			if (res) {
				res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
				Release(res);
			}
			return tex;
		}

		void DrawDebugOverlay(float a_scale)
		{
			auto* dl = ImGui::GetForegroundDrawList();
			auto  v = [a_scale](float x, float y) { return ImVec2(x * a_scale, y * a_scale); };
			dl->AddRectFilled(v(100, 100), v(200, 150), IM_COL32(0, 255, 0, 255));
			if (g_friz) {
				const float size = 16.0f * a_scale;  // GameFontNormal is 12; larger for the probe
				const char* text = "Azeroth Bridge";
				dl->AddText(g_friz, size, ImVec2(v(100, 170).x + a_scale, v(100, 170).y + a_scale), IM_COL32(0, 0, 0, 255), text);  // shadow (1,-1) in WoW's y-up space
				dl->AddText(g_friz, size, v(100, 170), IM_COL32(255, 255, 255, 255), text);
			}
			if (Input::CursorMode()) {
				const auto [cx, cy] = Input::Cursor();
				dl->AddTriangleFilled({ cx, cy }, { cx, cy + 18 * a_scale }, { cx + 12 * a_scale, cy + 13 * a_scale }, IM_COL32(255, 255, 255, 255));
			}
		}

		void ConvertToRgba8(const D3D11_MAPPED_SUBRESOURCE& a_m, DXGI_FORMAT a_fmt, int a_w, int a_h, std::vector<std::uint8_t>& a_out)
		{
			a_out.resize(static_cast<std::size_t>(a_w) * a_h * 4);
			for (int y = 0; y < a_h; ++y) {
				const auto* row = static_cast<const std::uint8_t*>(a_m.pData) + static_cast<std::size_t>(y) * a_m.RowPitch;
				auto*       dst = a_out.data() + static_cast<std::size_t>(y) * a_w * 4;
				for (int x = 0; x < a_w; ++x) {
					const auto* p = row + x * 4;
					switch (a_fmt) {
					case DXGI_FORMAT_B8G8R8A8_UNORM:
					case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
					case DXGI_FORMAT_B8G8R8X8_UNORM:
						dst[0] = p[2], dst[1] = p[1], dst[2] = p[0], dst[3] = 255;
						break;
					case DXGI_FORMAT_R10G10B10A2_UNORM:
						{
							const auto v = *reinterpret_cast<const std::uint32_t*>(p);
							dst[0] = static_cast<std::uint8_t>((v & 0x3FF) >> 2), dst[1] = static_cast<std::uint8_t>(((v >> 10) & 0x3FF) >> 2), dst[2] = static_cast<std::uint8_t>(((v >> 20) & 0x3FF) >> 2), dst[3] = 255;
							break;
						}
					default:  // R8G8B8A8 variants
						dst[0] = p[0], dst[1] = p[1], dst[2] = p[2], dst[3] = 255;
						break;
					}
					dst += 4;
				}
			}
		}

		void ServiceCaptures(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
		{
			for (auto& [name, c] : g_captures) {
				if (c.result.done) {
					continue;
				}
				if (!c.staging) {
					auto* tex = BoundTexture(a_context);
					if (!tex) {
						continue;
					}
					D3D11_TEXTURE2D_DESC d{};
					tex->GetDesc(&d);
					d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d.MiscFlags = 0;
					if (SUCCEEDED(a_device->CreateTexture2D(&d, nullptr, &c.staging))) {
						a_context->CopyResource(c.staging, tex);
						c.format = d.Format;
						c.result.width = static_cast<int>(d.Width);
						c.result.height = static_cast<int>(d.Height);
					}
					Release(tex);
					continue;
				}
				D3D11_MAPPED_SUBRESOURCE m{};
				if (a_context->Map(c.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) {
					continue;
				}
				ConvertToRgba8(m, c.format, c.result.width, c.result.height, c.result.rgba);
				a_context->Unmap(c.staging, 0);
				Release(c.staging);
				// Encode off the render thread; the pixels stay available to TakeCapture immediately.
				auto path = *logger::log_directory() / std::format("capture_{}.png", name);
				std::thread([path, w = c.result.width, h = c.result.height, px = c.result.rgba] {
					stbi_write_png(path.string().c_str(), w, h, 4, px.data(), w * 4);
				}).detach();
				c.result.done = true;
				logger::info("ui pass: captured {} ({}x{}, format {})", name, c.result.width, c.result.height, static_cast<int>(c.format));
			}
		}

		// The interface belongs to play: not on the main menu or a loading screen, nor before the player exists.
		bool InWorld()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* ui = RE::UI::GetSingleton();
			// Not over Skyrim's menus that stop play (inventory, map, journal...): they are full screen and the
			// interface waits with the game. Dialogue does not stop play, and the HUD stays, as WoW's does.
			return player && player->Is3DLoaded() && ui && !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) &&
			       !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) && !ui->GameIsPaused();
		}

		void Render()
		{
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!renderer) {
				return;
			}
			auto& rd = renderer->GetRuntimeData();
			auto* device = reinterpret_cast<ID3D11Device*>(rd.forwarder);
			auto* context = reinterpret_cast<ID3D11DeviceContext*>(rd.context);
			if (!device || !context) {
				return;
			}
			std::lock_guard lock(g_lock);
			if (!g_imguiReady) {
				LogBoundTarget(context);
				if (!InitImGui(device, context)) {
					return;
				}
				g_imguiReady = true;
			}
			auto* tex = BoundTexture(context);
			if (!tex) {
				return;
			}
			D3D11_TEXTURE2D_DESC d{};
			tex->GetDesc(&d);
			Release(tex);
			g_info.width = static_cast<int>(d.Width), g_info.height = static_cast<int>(d.Height), g_info.format = static_cast<int>(d.Format);

			if (Integration::Active() && InWorld()) {
				WorldPass::DrawLate(device, context);
			}
			if (Integration::Active() && Config::Get().wowUi && InWorld()) {
				WowUi::Render(device, context, static_cast<int>(d.Width), static_cast<int>(d.Height));
			}
			auto& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(static_cast<float>(d.Width), static_cast<float>(d.Height));
			io.DeltaTime = 1.0f / 60.0f;
			ImGui_ImplDX11_NewFrame();
			ImGui::NewFrame();
			if (g_debugOverlay) {
				DrawDebugOverlay(d.Height / 768.0f);
			}
			ImGui::Render();
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			ServiceCaptures(device, context);
			g_info.drawing = true;
			++g_info.frames;
		}

		// Both engine call sites of the UI renderer (ID 82084); we draw first so Skyrim's UI covers us.
		struct UiRenderHook
		{
			static std::int64_t thunk1(std::int64_t a_menuManager)
			{
				Render();
				return func1(a_menuManager);
			}
			static std::int64_t thunk2(std::int64_t a_menuManager)
			{
				Render();
				return func2(a_menuManager);
			}
			static inline REL::Relocation<decltype(thunk1)> func1;
			static inline REL::Relocation<decltype(thunk2)> func2;
		};
	}

	bool Install()
	{
		const auto target = REL::Relocation<std::uintptr_t>{ REL::ID(82084) }.address();
		const auto site1 = REL::Relocation<std::uintptr_t>{ REL::ID(36555) }.address() + 0x371;
		const auto site2 = REL::Relocation<std::uintptr_t>{ REL::ID(39039) }.address() + 0x1A0;  // 1.7.x offset (SMF3)
		if (!Hooks::VerifyCall(site1, target, "UI render (main)") || !Hooks::VerifyCall(site2, target, "UI render (alt)")) {
			return false;
		}
		UiRenderHook::func1 = SKSE::GetTrampoline().write_call<5>(site1, UiRenderHook::thunk1);
		UiRenderHook::func2 = SKSE::GetTrampoline().write_call<5>(site2, UiRenderHook::thunk2);
		logger::info("hook UI render (2 sites) installed");
		return true;
	}

	float Scale()
	{
		std::lock_guard lock(g_lock);
		return g_info.height / 768.0f;
	}

	void SetDebugOverlay(bool a_on)
	{
		std::lock_guard lock(g_lock);
		g_debugOverlay = a_on;
	}

	void RequestCapture(const std::string& a_name)
	{
		std::lock_guard lock(g_lock);
		g_captures[a_name] = {};
	}

	Capture TakeCapture(const std::string& a_name)
	{
		std::lock_guard lock(g_lock);
		auto it = g_captures.find(a_name);
		if (it == g_captures.end() || !it->second.result.done) {
			return {};
		}
		auto r = std::move(it->second.result);
		g_captures.erase(it);
		return r;
	}

	Info GetInfo()
	{
		std::lock_guard lock(g_lock);
		return g_info;
	}
}
