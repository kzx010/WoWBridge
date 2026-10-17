#include "render/WorldPass.h"
#include "render/RenderState.h"

#include "Hooks.h"
#include "Integration.h"
#include "render/WowModels.h"
#include "render/Shadows.h"

#include <d3d11.h>

#include "Debug_PSMain.h"
#include "Debug_VSMain.h"

// Technique follows SkyCraft's in-frame renderer (MIT, chasmlol/SkyCraft skse/src/WorldRender.cpp):
// hook site, camera re-basing, Skyrim's motion-vector convention and full state save/restore.
namespace WorldPass
{
	namespace
	{
		template <class T>
		void Release(T*& a_p)
		{
			if (a_p) {
				a_p->Release();
				a_p = nullptr;
			}
		}

		struct FrameCB
		{
			float viewProj[4][4];
			float prevViewProj[4][4];
		};
		struct ObjectCB
		{
			float offset[4];
			float color[4];
		};

		std::mutex g_lock;  // guards the request/result state below (main thread vs. renderer)
		Info       g_info;
		View       g_view;
		std::vector<Box> g_boxes;

		struct Pending
		{
			RE::NiPoint3     world;
			Sample           result;
			ID3D11Texture2D* staging = nullptr;
			bool             copied = false;
		};
		std::map<int, Pending> g_samples;
		int                    g_nextTicket = 1;

		ID3D11Device*            g_device = nullptr;
		ID3D11VertexShader*      g_vs = nullptr;
		ID3D11PixelShader*       g_ps = nullptr;
		ID3D11InputLayout*       g_layout = nullptr;
		ID3D11Buffer*            g_vb = nullptr;
		ID3D11Buffer*            g_ib = nullptr;
		ID3D11Buffer*            g_frameCb = nullptr;
		ID3D11Buffer*            g_objectCb = nullptr;
		ID3D11DepthStencilState* g_depthState = nullptr;
		ID3D11RasterizerState*   g_raster = nullptr;
		ID3D11BlendState*        g_noBlend = nullptr;
		bool                     g_initFailed = false;
		bool                     g_reversedKnown = false;
		double                   g_prev[4][4]{};
		bool                     g_havePrev = false;
		// The view RenderWorld drew this frame with, for the models drawn after tonemapping (DrawLate).
		FrameCB      g_lastFrame{};
		RE::NiPoint3 g_lastCam;
		bool         g_haveLast = false;


		bool InitResources(ID3D11Device* a_device)
		{
			g_device = a_device;
			bool ok = SUCCEEDED(a_device->CreateVertexShader(g_Debug_VSMain, sizeof(g_Debug_VSMain), nullptr, &g_vs)) &&
			          SUCCEEDED(a_device->CreatePixelShader(g_Debug_PSMain, sizeof(g_Debug_PSMain), nullptr, &g_ps));
			const D3D11_INPUT_ELEMENT_DESC elems[] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
			ok = ok && SUCCEEDED(a_device->CreateInputLayout(elems, 1, g_Debug_VSMain, sizeof(g_Debug_VSMain), &g_layout));

			const float v[8][3] = { { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 }, { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } };
			const std::uint16_t idx[36] = { 0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1, 3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2 };
			D3D11_BUFFER_DESC bd{ sizeof(v), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
			D3D11_SUBRESOURCE_DATA sd{ v };
			ok = ok && SUCCEEDED(a_device->CreateBuffer(&bd, &sd, &g_vb));
			bd = { sizeof(idx), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER };
			sd = { idx };
			ok = ok && SUCCEEDED(a_device->CreateBuffer(&bd, &sd, &g_ib));
			bd = { sizeof(FrameCB), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
			ok = ok && SUCCEEDED(a_device->CreateBuffer(&bd, nullptr, &g_frameCb));
			bd.ByteWidth = sizeof(ObjectCB);
			ok = ok && SUCCEEDED(a_device->CreateBuffer(&bd, nullptr, &g_objectCb));

			D3D11_RASTERIZER_DESC rd{ D3D11_FILL_SOLID, D3D11_CULL_NONE };
			rd.DepthClipEnable = TRUE;
			ok = ok && SUCCEEDED(a_device->CreateRasterizerState(&rd, &g_raster));
			D3D11_BLEND_DESC bld{};
			bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			bld.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			bld.IndependentBlendEnable = TRUE;
			ok = ok && SUCCEEDED(a_device->CreateBlendState(&bld, &g_noBlend));
			if (!ok) {
				logger::error("world pass: resource creation failed");
			}
			return ok;
		}

		void EnsureDepthState(bool a_reversed)
		{
			if (g_depthState && g_reversedKnown && g_info.reversedZ == a_reversed) {
				return;
			}
			Release(g_depthState);
			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = TRUE;
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			dd.DepthFunc = a_reversed ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL;
			g_device->CreateDepthStencilState(&dd, &g_depthState);
			g_info.reversedZ = a_reversed;
			g_reversedKnown = true;
			logger::info("world pass: depth convention {}", a_reversed ? "reversed Z" : "standard Z");
		}

		using RenderState::StateBackup;

		float Half(std::uint16_t h)
		{
			const std::uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
			float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? INFINITY : std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
			return s ? -v : v;
		}

		// R11G11B10_FLOAT's unsigned channels: a 5-bit exponent over a_mantBits of mantissa.
		float Small(std::uint32_t v, int a_mantBits)
		{
			const std::uint32_t e = (v >> a_mantBits) & 0x1F, m = v & ((1u << a_mantBits) - 1);
			return e == 0 ? std::ldexp(static_cast<float>(m), -14 - a_mantBits) : e == 31 ? INFINITY : std::ldexp(static_cast<float>(m | (1u << a_mantBits)), static_cast<int>(e) - 15 - a_mantBits);
		}

		// One pixel of the scene target in its own format: DXVK gave Skyrim's main target R16G16B16A16_FLOAT, native D3D11
		// R11G11B10_FLOAT; an undecoded format leaves the sample black, so it is reported once.
		void Decode(DXGI_FORMAT a_format, const void* a_px, float* a_rgba)
		{
			switch (a_format) {
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
				for (int k = 0; k < 4; ++k) a_rgba[k] = Half(static_cast<const std::uint16_t*>(a_px)[k]);
				return;
			case DXGI_FORMAT_R11G11B10_FLOAT: {
				const auto v = *static_cast<const std::uint32_t*>(a_px);
				a_rgba[0] = Small(v & 0x7FF, 6); a_rgba[1] = Small((v >> 11) & 0x7FF, 6); a_rgba[2] = Small(v >> 22, 5); a_rgba[3] = 1.f;
				return;
			}
			case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
				const auto* b = static_cast<const std::uint8_t*>(a_px);
				const bool  bgr = a_format == DXGI_FORMAT_B8G8R8A8_UNORM || a_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
				a_rgba[0] = b[bgr ? 2 : 0] / 255.f; a_rgba[1] = b[1] / 255.f; a_rgba[2] = b[bgr ? 0 : 2] / 255.f; a_rgba[3] = b[3] / 255.f;
				return;
			}
			default:
				static bool reported = false;
				if (!reported) { reported = true; logger::warn("world pass: scene format {} not decoded for samples", static_cast<int>(a_format)); }
			}
		}

		// Projects a world point with the same camera-relative matrix the shader uses.
		bool Project(const FrameCB& a_fc, const RE::NiPoint3& a_rel, int a_w, int a_h, int& a_x, int& a_y)
		{
			float clip[4];
			for (int r = 0; r < 4; ++r) {
				clip[r] = a_fc.viewProj[r][0] * a_rel.x + a_fc.viewProj[r][1] * a_rel.y + a_fc.viewProj[r][2] * a_rel.z + a_fc.viewProj[r][3];
			}
			if (clip[3] <= 1e-3f) {
				return false;
			}
			const float nx = clip[0] / clip[3], ny = clip[1] / clip[3];
			a_x = static_cast<int>((nx * 0.5f + 0.5f) * a_w);
			a_y = static_cast<int>((0.5f - ny * 0.5f) * a_h);
			return a_x >= 0 && a_y >= 0 && a_x < a_w && a_y < a_h;
		}

		void Draw()
		{
			auto* camera = RE::Main::WorldRootCamera();
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!camera || !renderer || g_initFailed) {
				return;
			}
			auto& rd = renderer->GetRuntimeData();
			auto* context = reinterpret_cast<ID3D11DeviceContext*>(rd.context);
			auto* device = reinterpret_cast<ID3D11Device*>(rd.forwarder);
			const auto& scene = rd.renderTargets[RE::RENDER_TARGETS::kMAIN];
			const auto& motion = rd.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
			const auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			auto* sceneTex = reinterpret_cast<ID3D11Texture2D*>(scene.texture);
			auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(scene.RTV);
			auto* dsv = reinterpret_cast<ID3D11DepthStencilView*>(depth.views[0]);
			if (!context || !device || !sceneTex || !rtv || !dsv) {
				return;
			}
			if (!g_vs && !InitResources(device)) {
				g_initFailed = true;
				return;
			}
			D3D11_TEXTURE2D_DESC sdesc{};
			sceneTex->GetDesc(&sdesc);

			// Camera: Skyrim's world->clip, re-based on the camera so the GPU sees camera-relative coordinates.
			const auto& w2c = camera->GetRuntimeData().worldToCam;
			const auto  cam = camera->world.translate;
			FrameCB     fc{};
			for (int r = 0; r < 4; ++r) {
				for (int c = 0; c < 3; ++c) {
					fc.viewProj[r][c] = w2c[r][c];
					fc.prevViewProj[r][c] = static_cast<float>(g_havePrev ? g_prev[r][c] : w2c[r][c]);
				}
				fc.viewProj[r][3] = static_cast<float>(double(w2c[r][3]) + double(w2c[r][0]) * cam.x + double(w2c[r][1]) * cam.y + double(w2c[r][2]) * cam.z);
				const double* p = g_prev[r];
				fc.prevViewProj[r][3] = g_havePrev ? static_cast<float>(p[3] + p[0] * cam.x + p[1] * cam.y + p[2] * cam.z) : fc.viewProj[r][3];
			}
			for (int r = 0; r < 4; ++r) {
				for (int c = 0; c < 4; ++c) {
					g_prev[r][c] = w2c[r][c];
				}
			}
			g_havePrev = true;

			// Depth convention from the projection itself: nearer points map to larger z when reversed.
			const RE::NiPoint3 fwd{ camera->world.rotate.entry[0][0], camera->world.rotate.entry[1][0], camera->world.rotate.entry[2][0] };
			auto ndcZ = [&](float d) {
				const auto p = fwd * d;
				const float z = fc.viewProj[2][0] * p.x + fc.viewProj[2][1] * p.y + fc.viewProj[2][2] * p.z + fc.viewProj[2][3];
				const float w = fc.viewProj[3][0] * p.x + fc.viewProj[3][1] * p.y + fc.viewProj[3][2] * p.z + fc.viewProj[3][3];
				return std::fabs(w) > 1e-6f ? z / w : 0.0f;
			};

			std::lock_guard lock(g_lock);
			g_view.valid = true;
			g_view.cam = cam;
			g_view.width = static_cast<int>(sdesc.Width);
			g_view.height = static_cast<int>(sdesc.Height);
			for (int r = 0; r < 4; ++r) {
				for (int c = 0; c < 3; ++c) {
					g_view.viewProj[r][c] = w2c[r][c];
				}
				g_view.viewProj[r][3] = double(w2c[r][3]) + double(w2c[r][0]) * cam.x + double(w2c[r][1]) * cam.y + double(w2c[r][2]) * cam.z;
			}
			EnsureDepthState(ndcZ(100.0f) > ndcZ(10000.0f));
			g_info.width = static_cast<int>(sdesc.Width);
			g_info.height = static_cast<int>(sdesc.Height);
			g_info.sceneFormat = static_cast<int>(sdesc.Format);
			++g_info.frames;

			StateBackup backup;
			backup.Save(context);
			D3D11_MAPPED_SUBRESOURCE m{};
			if (SUCCEEDED(context->Map(g_frameCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
				std::memcpy(m.pData, &fc, sizeof(fc));
				context->Unmap(g_frameCb, 0);
			}
			ID3D11RenderTargetView* targets[2] = { rtv, reinterpret_cast<ID3D11RenderTargetView*>(motion.RTV) };
			context->OMSetRenderTargets(targets[1] ? 2 : 1, targets, dsv);
			const D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(sdesc.Width), static_cast<float>(sdesc.Height), 0, 1 };
			context->RSSetViewports(1, &vp);
			context->RSSetState(g_raster);
			context->OMSetDepthStencilState(g_depthState, 0);
			context->OMSetBlendState(g_noBlend, nullptr, 0xFFFFFFFF);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->IASetInputLayout(g_layout);
			const UINT stride = 12, zero = 0;
			context->IASetVertexBuffers(0, 1, &g_vb, &stride, &zero);
			context->IASetIndexBuffer(g_ib, DXGI_FORMAT_R16_UINT, 0);
			context->VSSetShader(g_vs, nullptr, 0);
			context->PSSetShader(g_ps, nullptr, 0);
			ID3D11Buffer* cbs[2] = { g_frameCb, g_objectCb };
			context->VSSetConstantBuffers(0, 2, cbs);
			context->PSSetConstantBuffers(0, 2, cbs);
			for (const auto& b : g_boxes) {
				const auto rel = b.center - cam;
				const ObjectCB oc{ { rel.x, rel.y, rel.z, b.half }, { b.rgba[0], b.rgba[1], b.rgba[2], b.rgba[3] } };
				if (SUCCEEDED(context->Map(g_objectCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
					std::memcpy(m.pData, &oc, sizeof(oc));
					context->Unmap(g_objectCb, 0);
				}
				context->DrawIndexed(36, 0, 0);
			}
			Shadows::DrawBlobs({ device, context, cam, &fc.viewProj[0][0],
				reinterpret_cast<ID3D11ShaderResourceView*>(depth.depthSRV), sdesc.Width, sdesc.Height }, rtv);
			backup.Restore(context);
			g_lastFrame = fc;
			g_lastCam = cam;
			g_haveLast = true;
			g_info.drawing = true;

			// Pixel oracle: copy requested pixels now (after our draw), map them on later frames.
			for (auto& [ticket, s] : g_samples) {
				if (s.result.done) {
					continue;
				}
				if (!s.copied) {
					s.result.onScreen = Project(fc, s.world - cam, g_info.width, g_info.height, s.result.x, s.result.y);
					if (!s.result.onScreen) {
						s.result.done = true;
						continue;
					}
					D3D11_TEXTURE2D_DESC td{ 1, 1, 1, 1, sdesc.Format, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ };
					if (FAILED(device->CreateTexture2D(&td, nullptr, &s.staging))) {
						s.result.done = true;
						continue;
					}
					const D3D11_BOX box{ static_cast<UINT>(s.result.x), static_cast<UINT>(s.result.y), 0, static_cast<UINT>(s.result.x) + 1, static_cast<UINT>(s.result.y) + 1, 1 };
					context->CopySubresourceRegion(s.staging, 0, 0, 0, 0, sceneTex, 0, &box);
					s.copied = true;
					continue;
				}
				if (context->Map(s.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
					Decode(sdesc.Format, m.pData, s.result.rgba);
					context->Unmap(s.staging, 0);
					Release(s.staging);
					s.result.done = true;
				}
			}
		}

		struct RenderWorldHook
		{
			static void thunk(bool a_arg)
			{
				func(a_arg);
				if (Integration::Active()) Draw();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void DrawLate(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer || !g_haveLast || !g_depthState || !g_frameCb) {
			return;
		}
		const auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto*       dsv = reinterpret_cast<ID3D11DepthStencilView*>(depth.views[0]);
		ID3D11RenderTargetView* target = nullptr;
		a_context->OMGetRenderTargets(1, &target, nullptr);
		if (!dsv || !target) {
			Release(target);
			return;
		}
		ID3D11Resource* resource = nullptr;
		target->GetResource(&resource);
		D3D11_TEXTURE2D_DESC desc{};
		static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
		Release(resource);
		StateBackup backup;
		backup.Save(a_context);
		FrameCB fc = g_lastFrame;
		std::memcpy(fc.prevViewProj, fc.viewProj, sizeof(fc.viewProj));
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(a_context->Map(g_frameCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			std::memcpy(m.pData, &fc, sizeof(fc));
			a_context->Unmap(g_frameCb, 0);
		}
		a_context->OMSetRenderTargets(1, &target, dsv);
		const D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1 };
		a_context->RSSetViewports(1, &vp);
		WowModels::Frame frame{ a_device, a_context, g_frameCb, g_depthState, g_info.reversedZ, g_lastCam,
			reinterpret_cast<ID3D11ShaderResourceView*>(depth.depthSRV) };
		std::memcpy(frame.viewProj, fc.viewProj, sizeof(frame.viewProj));
		// The depth the shadow passes read is kMAIN's: its own size (the UI target can differ under a render scale).
		UINT depthW = desc.Width, depthH = desc.Height;
		if (auto* dsrv = reinterpret_cast<ID3D11ShaderResourceView*>(frame.depth)) {
			ID3D11Resource* dres = nullptr; dsrv->GetResource(&dres);
			if (dres) { D3D11_TEXTURE2D_DESC dd{}; static_cast<ID3D11Texture2D*>(dres)->GetDesc(&dd); depthW = dd.Width; depthH = dd.Height; dres->Release(); }
		}
		const Shadows::Frame shadows{ a_device, a_context, frame.cam, frame.viewProj, frame.depth, depthW, depthH };
		Shadows::BeginLate(shadows);
		WowModels::Draw(frame);
		Shadows::DrawMask(shadows);
		backup.Restore(a_context);
		Release(target);
	}

	bool Install()
	{
		// Main::Render's call to RenderWorld (AE ID; verified by tools/addrlib.py and again here).
		const auto site = REL::Relocation<std::uintptr_t>{ REL::ID(36559) }.address() + 0x85E;
		if (!Hooks::VerifyCall(site, REL::Relocation<std::uintptr_t>{ REL::ID(107142) }.address(), "RenderWorld")) {
			return false;
		}
		RenderWorldHook::func = SKSE::GetTrampoline().write_call<5>(site, RenderWorldHook::thunk);
		std::lock_guard lock(g_lock);
		g_info.installed = true;
		logger::info("hook RenderWorld (world pass) installed");
		return true;
	}

	void SetBoxes(std::vector<Box> a_boxes)
	{
		std::lock_guard lock(g_lock);
		g_boxes = std::move(a_boxes);
	}

	int RequestSample(const RE::NiPoint3& a_world)
	{
		std::lock_guard lock(g_lock);
		const int t = g_nextTicket++;
		g_samples[t].world = a_world;
		return t;
	}

	Sample GetSample(int a_ticket)
	{
		std::lock_guard lock(g_lock);
		auto it = g_samples.find(a_ticket);
		if (it == g_samples.end()) {
			return {};
		}
		auto r = it->second.result;
		if (r.done) {
			g_samples.erase(it);
		}
		return r;
	}

	View LastView()
	{
		std::lock_guard lock(g_lock);
		return g_view;
	}

	Info GetInfo()
	{
		std::lock_guard lock(g_lock);
		return g_info;
	}
}
