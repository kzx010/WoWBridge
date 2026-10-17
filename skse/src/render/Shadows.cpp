#include "render/Shadows.h"
#include "render/RenderState.h"
#include "render/NpcShoulders.h"
#include "render/NpcWeapons.h"
#include "Avatar.h"
#include "Integration.h"
#include "ui/UiPass.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "WowShadow_VSMain.h"
#include "WowShadow_BlobPS.h"
#include "WowShadow_MaskPS.h"
#include "WowShadow_MaskProbePS.h"
#include "WowShadow_CascadeProbePS.h"

namespace Shadows
{
	namespace
	{
		using RenderState::Release;
		using Clock = std::chrono::steady_clock;
		// On by default (proved October 16: receive, blob and the cascade probe in Riverwood); wb shadows turns each off.
		std::atomic<bool> g_receive = true, g_blob = true, g_cascades = true;
		std::atomic<bool> g_maskCapture = false, g_maskDrawn = false;
		std::atomic<std::uint64_t> g_epoch = 0;
		std::mutex g_lock;
		std::map<RE::FormID, RE::NiPoint3> g_feet;
		nlohmann::json g_status = nlohmann::json::object();
		struct Constants
		{
			float proj[2][4][4]{};
			float splits[4]{}, params[4]{}, forward[4]{}, viewport[4]{}, control[4]{};
			float invVP[16]{}, vp[16]{}, feet[4]{}, blobInfo[4]{};
			float centers[32][4]{};
		};
		static_assert(sizeof(Constants) % 16 == 0);
		struct Cascade { float m[4][4]{}; float split = 0; UINT slice = 0; };
		Cascade g_snapshot[2];
		UINT g_count = 0;
		std::uint64_t g_captureEpoch = 0, g_maskEpoch = 0, g_maskQueueEpoch = 0, g_frames = 0;
		Clock::time_point g_captureTime{}, g_maskTime{}, g_maskQueued{}, g_nextProbe{}, g_cascadeProbeTime{}, g_blobTime{};
		std::uint64_t g_blobEpoch = 0;
		bool g_maskGood = false, g_convention = false, g_cascadeProbeGood = false, g_receiving = false;
		float g_sunVis = 1, g_minLit = 1;
		RE::NiPoint3 g_probeFeet, g_sampleFeet;
		std::uint64_t g_probeSerial = 0, g_blobDraws = 0;
		std::vector<std::pair<RE::NiPoint3, float>> g_blobs;
		ID3D11Device* g_device = nullptr;
		ID3D11VertexShader* g_vs = nullptr;
		ID3D11PixelShader* g_blobPs = nullptr, *g_maskPs = nullptr, *g_maskProbePs = nullptr, *g_cascadeProbePs = nullptr;
		ID3D11Buffer* g_cb = nullptr;
		ID3D11SamplerState* g_sampler = nullptr;
		ID3D11RasterizerState* g_raster = nullptr;
		ID3D11DepthStencilState* g_noDepth = nullptr;
		ID3D11BlendState* g_multiply = nullptr, *g_opaque = nullptr;
		ID3D11Texture2D* g_copy = nullptr;
		ID3D11ShaderResourceView* g_copySrv = nullptr;
		D3D11_TEXTURE2D_DESC g_copyDesc{};
		struct Probe
		{
			ID3D11Texture2D* target = nullptr, *staging = nullptr;
			ID3D11RenderTargetView* rtv = nullptr;
			bool pending = false;
		} g_maskProbe, g_cascadeProbe;

		RE::BSShadowDirectionalLight* Sun()
		{
			auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			return scene ? scene->GetRuntimeData().sunShadowDirLight : nullptr;
		}
		bool SunLight(RE::NiPoint3& direction, float& strength)
		{
			auto& state = RE::BSShaderManager::State::GetSingleton();
			auto* scene = state.shadowSceneNode[0];
			if (state.interior || !scene || !Sun()) return false;
			auto* sun = scene->GetRuntimeData().sunLight;
			auto* light = sun ? netimmerse_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr;
			if (!light) return false;
			const auto d = light->GetDirectionalLightRuntimeData().worldDir;
			const float length = d.Length();
			if (!std::isfinite(length) || length < 0.1f) return false;
			direction = d * (-1.0f / length);
			if (direction.z <= 0.03f) return false;
			const auto& c = light->GetLightRuntimeData().diffuse;
			const auto& ambient = state.directionalAmbientTransform;
			const float a = (std::max(0.05f, ambient.rotate.entry[0][2] + ambient.translate.x) +
				std::max(0.05f, ambient.rotate.entry[1][2] + ambient.translate.y) +
				std::max(0.05f, ambient.rotate.entry[2][2] + ambient.translate.z)) / 3;
			const float s = std::max(0.0f, (c.red + c.green + c.blue) / 3);
			strength = s / std::max(a + s, 0.001f);
			return std::isfinite(strength) && s > 0;
		}

		bool Init(ID3D11Device* d)
		{
			if (g_device) return g_device == d && g_cb;
			g_device = d;
			bool ok = SUCCEEDED(d->CreateVertexShader(g_WowShadow_VSMain, sizeof(g_WowShadow_VSMain), nullptr, &g_vs)) &&
				SUCCEEDED(d->CreatePixelShader(g_WowShadow_BlobPS, sizeof(g_WowShadow_BlobPS), nullptr, &g_blobPs)) &&
				SUCCEEDED(d->CreatePixelShader(g_WowShadow_MaskPS, sizeof(g_WowShadow_MaskPS), nullptr, &g_maskPs)) &&
				SUCCEEDED(d->CreatePixelShader(g_WowShadow_MaskProbePS, sizeof(g_WowShadow_MaskProbePS), nullptr, &g_maskProbePs)) &&
				SUCCEEDED(d->CreatePixelShader(g_WowShadow_CascadeProbePS, sizeof(g_WowShadow_CascadeProbePS), nullptr, &g_cascadeProbePs));
			D3D11_BUFFER_DESC bd{ sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
			ok = ok && SUCCEEDED(d->CreateBuffer(&bd, nullptr, &g_cb));
			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = D3D11_FLOAT32_MAX; sd.MaxAnisotropy = 1; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
			ok = ok && SUCCEEDED(d->CreateSamplerState(&sd, &g_sampler));
			D3D11_RASTERIZER_DESC rd{ D3D11_FILL_SOLID, D3D11_CULL_NONE };
			rd.DepthClipEnable = TRUE;
			ok = ok && SUCCEEDED(d->CreateRasterizerState(&rd, &g_raster));
			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
			dd.FrontFace.StencilFunc = dd.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
			dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
			dd.BackFace = dd.FrontFace;
			ok = ok && SUCCEEDED(d->CreateDepthStencilState(&dd, &g_noDepth));
			D3D11_BLEND_DESC blend{};
			blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			ok = ok && SUCCEEDED(d->CreateBlendState(&blend, &g_opaque));
			auto& b = blend.RenderTarget[0];
			b.BlendEnable = TRUE;
			b.SrcBlend = D3D11_BLEND_ZERO; b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D11_BLEND_OP_ADD;
			b.SrcBlendAlpha = D3D11_BLEND_ZERO; b.DestBlendAlpha = D3D11_BLEND_ONE; b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			ok = ok && SUCCEEDED(d->CreateBlendState(&blend, &g_multiply));
			if (!ok) {
				for (auto** r : { reinterpret_cast<IUnknown**>(&g_vs), reinterpret_cast<IUnknown**>(&g_blobPs), reinterpret_cast<IUnknown**>(&g_maskPs),
					reinterpret_cast<IUnknown**>(&g_maskProbePs), reinterpret_cast<IUnknown**>(&g_cascadeProbePs), reinterpret_cast<IUnknown**>(&g_cb),
					reinterpret_cast<IUnknown**>(&g_sampler), reinterpret_cast<IUnknown**>(&g_raster), reinterpret_cast<IUnknown**>(&g_noDepth),
					reinterpret_cast<IUnknown**>(&g_multiply), reinterpret_cast<IUnknown**>(&g_opaque) }) { if (*r) { (*r)->Release(); *r = nullptr; } }
				logger::error("shadows: resource creation failed; receiving and blobs disabled");
			}
			return ok;
		}

		// SkyCraft DepthReadFormat. Keep a typeless copy for the matching typed depth SRV.
		DXGI_FORMAT DepthReadFormat(DXGI_FORMAT f)
		{
			switch (f) {
			case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
			case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
			case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			default: return DXGI_FORMAT_UNKNOWN;
			}
		}
		DXGI_FORMAT Typeless(DXGI_FORMAT f)
		{
			switch (DepthReadFormat(f)) {
			case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
			case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return DXGI_FORMAT_R24G8_TYPELESS;
			case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
			case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return DXGI_FORMAT_R32G8X24_TYPELESS;
			default: return DXGI_FORMAT_UNKNOWN;
			}
		}
		void LogSun(RE::BSShadowDirectionalLight* sun)
		{
			if (!sun) return;
			auto& descs = sun->GetRuntimeData().shadowmapDescriptors;
			if (descs.empty()) return;
			static bool dumped = false;
			if (dumped) return;
			dumped = true;
			const auto& runtime = sun->GetShadowDirectionalLightRuntimeData();
			for (UINT k = 0; k < descs.size(); ++k) {
				const auto& d = descs[k];
				const auto* port = reinterpret_cast<const std::int32_t*>(&d.port);
				const auto& m = d.lightTransform.m;
				logger::info("shadows: cascade {} of {} shadowmapIndex {} port ({} {} {} {}) splits {}-{}",
					k, descs.size(), d.shadowmapIndex, port[0], port[1], port[2], port[3],
					k < 3 ? runtime.startSplitDistances[k] : 0, k < 3 ? runtime.endSplitDistances[k] : 0);
				for (int r = 0; r < 4; ++r) logger::info("shadows: lightTransform row {}: {} {} {} {}", r, m[r][0], m[r][1], m[r][2], m[r][3]);
			}
		}
		void LogTargets()
		{
			static bool dumped = false;
			if (dumped) return;
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!renderer) return;
			auto* mask = reinterpret_cast<ID3D11Texture2D*>(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSHADOW_MASK].texture);
			auto* maps = reinterpret_cast<ID3D11Texture2D*>(renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS].texture);
			logger::info("shadows: sunShadowDirLight {}", Sun() ? "present" : "absent");
			auto log = [](const char* name, ID3D11Texture2D* texture) {
				if (!texture) { logger::info("shadows: {} absent", name); return; }
				D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
				logger::info("shadows: {} {}x{} format {} array {} MSAA {}", name, d.Width, d.Height, static_cast<int>(d.Format), d.ArraySize, d.SampleDesc.Count);
			};
			log("kSHADOW_MASK", mask); log("kSHADOWMAPS", maps);
			dumped = true;
		}

		// SkyCraft CaptureSunShadows: the pooled array must be copied at Render return, before reuse.
		void CaptureSunShadows(RE::BSShadowLight* light)
		{
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!Integration::Active() || !g_cascades || !renderer || light != Sun()) return;
			auto* d = reinterpret_cast<ID3D11Device*>(renderer->GetRuntimeData().forwarder);
			auto* c = reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context);
			if (!d || !c || !Init(d)) return;
			auto* sun = static_cast<RE::BSShadowDirectionalLight*>(light);
			LogSun(sun);
			auto& descs = light->GetRuntimeData().shadowmapDescriptors;
			const UINT count = std::min<UINT>(descs.size(), 2);
			if (!count) { g_count = 0; return; }
			// The cascades' own render target (the descriptor names it; vanilla's is the pooled kSHADOWMAPS array): the
			// DSV bound at this hook was once a 2-slice array after a load (the review, October 16), so it only confirms.
			auto& depthStencils = renderer->GetDepthStencilData().depthStencils;
			auto slot = static_cast<std::uint32_t>(descs[0].renderTarget);
			for (UINT k = 1; k < count; ++k) if (descs[k].renderTarget != descs[0].renderTarget) slot = ~0u;
			if (slot >= static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL)) slot = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS);
			ID3D11Resource* resource = reinterpret_cast<ID3D11Texture2D*>(depthStencils[slot].texture);
			if (!resource) return;
			resource->AddRef();
			{
				ID3D11DepthStencilView* bound = nullptr; ID3D11Resource* boundResource = nullptr;
				c->OMGetRenderTargets(0, nullptr, &bound);
				if (bound) { bound->GetResource(&boundResource); Release(bound); }
				static bool warned = false;
				if (boundResource && boundResource != resource && !warned) { logger::warn("shadows: the DSV bound at Render is not the cascades' target (slot {}); copying from the target", slot); warned = true; }
				Release(boundResource);
			}
			D3D11_TEXTURE2D_DESC sd{};
			static_cast<ID3D11Texture2D*>(resource)->GetDesc(&sd);
			bool valid = sd.SampleDesc.Count == 1 && sd.Width == sd.Height && sd.MipLevels > 0 && DepthReadFormat(sd.Format) != DXGI_FORMAT_UNKNOWN;
			for (UINT k = 0; k < count; ++k) valid = valid && descs[k].shadowmapIndex < sd.ArraySize;
			if (!valid) { g_count = 0; Release(resource); return; }
			if (g_copy && (sd.Width != g_copyDesc.Width || sd.Height != g_copyDesc.Height || sd.Format != g_copyDesc.Format)) {
				Release(g_copySrv); Release(g_copy); g_count = 0; g_convention = false; g_cascadeProbeGood = false;
			}
			if (!g_copy) {
				auto cd = sd; cd.ArraySize = 2; cd.MipLevels = 1; cd.Format = Typeless(sd.Format);
				cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.CPUAccessFlags = cd.MiscFlags = 0;
				D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
				vd.Format = DepthReadFormat(sd.Format); vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
				vd.Texture2DArray.MipLevels = 1; vd.Texture2DArray.ArraySize = 2;
				if (FAILED(d->CreateTexture2D(&cd, nullptr, &g_copy)) || FAILED(d->CreateShaderResourceView(g_copy, &vd, &g_copySrv))) {
					Release(g_copySrv); Release(g_copy); Release(resource); return;
				}
				g_copyDesc = sd;
				logger::info("shadows: CaptureSunShadows copying {}x{} format {} array {} into 2 slices", sd.Width, sd.Height, static_cast<int>(sd.Format), sd.ArraySize);
			}
			RenderState::StateBackup backup; backup.Save(c);
			c->OMSetRenderTargets(0, nullptr, nullptr);
			ID3D11ShaderResourceView* none = nullptr; c->PSSetShaderResources(3, 1, &none);
			const auto& runtime = sun->GetShadowDirectionalLightRuntimeData();
			for (UINT k = 0; k < count; ++k) {
				c->CopySubresourceRegion(g_copy, k, 0, 0, 0, resource, D3D11CalcSubresource(0, descs[k].shadowmapIndex, sd.MipLevels), nullptr);
				std::memcpy(g_snapshot[k].m, descs[k].lightTransform.m, sizeof(g_snapshot[k].m));
				g_snapshot[k].split = runtime.endSplitDistances[k]; g_snapshot[k].slice = descs[k].shadowmapIndex;
			}
			backup.Restore(c);
			Release(resource);
			g_count = count; g_captureTime = Clock::now(); g_captureEpoch = g_epoch.load();
		}
		struct SunRender
		{
			static void thunk(RE::BSShadowLight* light, std::uint32_t& index) { func(light, index); CaptureSunShadows(light); }
			static inline REL::Relocation<decltype(thunk)> func;
		};

		bool SetSkyrimShadows(Constants& cb, const RE::NiPoint3& cam, const RE::NiPoint3& dir)
		{
			// The sun re-renders its cascades intermittently; the snapshot stays as current as the engine's own until the cell
			// changes (pixels outside a stale cascade read lit).
			if (!g_copySrv || !g_count || g_captureEpoch != g_epoch.load()) return false;
			// SkyCraft SetSkyrimShadows runtime row/column and depth-direction detection.
			for (UINT k = 0; k < g_count; ++k) {
				const auto& m = g_snapshot[k].m;
				const bool row = std::fabs(m[0][3]) + std::fabs(m[1][3]) + std::fabs(m[2][3]) < 1e-6f;
				auto at = [&](int r, int col) { return double(row ? m[r][col] : m[col][r]); };
				for (int out = 0; out < 4; ++out) {
					for (int in = 0; in < 3; ++in) cb.proj[k][out][in] = static_cast<float>(at(in, out));
					cb.proj[k][out][3] = static_cast<float>(at(3, out) + at(0, out) * cam.x + at(1, out) * cam.y + at(2, out) * cam.z);
				}
				if (k == 0) {
					const double gradient = at(0, 2) * -dir.x + at(1, 2) * -dir.y + at(2, 2) * -dir.z;
					if (!std::isfinite(gradient) || std::fabs(gradient) < 1e-12) return false;
					cb.params[3] = gradient > 0 ? 1.0f : -1.0f;
					if (!g_convention) logger::info("shadows: convention detected {} depth {} (gradient {}), {} cascades, slices {} {}",
						row ? "row-vector" : "column-vector", gradient > 0 ? "standard" : "reversed", gradient,
						g_count, g_snapshot[0].slice, g_count > 1 ? g_snapshot[1].slice : 0);
				}
				if (!std::isfinite(g_snapshot[k].split) || g_snapshot[k].split <= 0 || (k && g_snapshot[k].split <= g_snapshot[k - 1].split)) return false;
				for (auto& r : cb.proj[k]) for (float value : r) if (!std::isfinite(value)) return false;
			}
			g_convention = true;
			cb.splits[0] = g_snapshot[0].split; cb.splits[1] = g_snapshot[g_count - 1].split; cb.splits[3] = static_cast<float>(g_count);
			cb.params[0] = 0; cb.params[1] = g_count > 1 ? 1.0f : 0.0f; cb.params[2] = 1.0f / g_copyDesc.Width;
			return true;
		}
		ID3D11ShaderResourceView* Mask()
		{
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			auto* srv = renderer ? reinterpret_cast<ID3D11ShaderResourceView*>(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSHADOW_MASK].SRV) : nullptr;
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			if (srv) srv->GetDesc(&desc);
			return desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ? srv : nullptr;
		}
		Constants MakeConstants(const Frame& f)
		{
			Constants cb{};
			std::memcpy(cb.vp, f.viewProj, sizeof(cb.vp));
			const glm::mat4 inverse = glm::transpose(glm::inverse(glm::transpose(glm::make_mat4(f.viewProj))));
			std::memcpy(cb.invVP, glm::value_ptr(inverse), sizeof(cb.invVP));
			cb.viewport[0] = static_cast<float>(f.width); cb.viewport[1] = static_cast<float>(f.height);
			cb.viewport[2] = 1.0f / std::max(f.width, 1u); cb.viewport[3] = 1.0f / std::max(f.height, 1u);
			auto* camera = RE::Main::WorldRootCamera();
			if (camera) for (int i = 0; i < 3; ++i) cb.forward[i] = camera->world.rotate.entry[i][0];
			const auto feet = Avatar::Current().position - f.cam;
			cb.feet[0] = feet.x; cb.feet[1] = feet.y; cb.feet[2] = feet.z;
			RE::NiPoint3 dir; float strength = 0;
			const bool sun = SunLight(dir, strength);
			const bool mask = Mask() && g_maskGood && Clock::now() - g_maskTime < std::chrono::seconds(2) && g_maskEpoch == g_epoch.load();
			// The sun's mask channel (R/G/B/A); the shader reads that one.
			const std::uint32_t channel = Sun() ? Sun()->GetRuntimeData().maskIndex : 0;
			cb.feet[3] = static_cast<float>(channel);
			cb.control[0] = sun && mask && channel < 4 ? 1.0f : 0.0f;
			cb.control[1] = sun && g_cascades && SetSkyrimShadows(cb, f.cam, dir) ? 1.0f : 0.0f;
			cb.control[2] = sun && g_receive ? 1.0f : 0.0f;
			cb.control[3] = strength;
			return cb;
		}
		bool Bind(const Frame& f, const Constants& cb)
		{
			D3D11_MAPPED_SUBRESOURCE m{};
			if (FAILED(f.context->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
			std::memcpy(m.pData, &cb, sizeof(cb)); f.context->Unmap(g_cb, 0);
			f.context->PSSetConstantBuffers(3, 1, &g_cb);
			ID3D11ShaderResourceView* views[]{ Mask(), cb.control[1] > 0 ? g_copySrv : nullptr };
			f.context->PSSetShaderResources(2, 2, views); f.context->PSSetSamplers(1, 1, &g_sampler);
			return true;
		}
		void Fullscreen(const Frame& f, ID3D11RenderTargetView* target, ID3D11PixelShader* ps, UINT width, UINT height, bool multiply = false)
		{
			auto* c = f.context;
			c->OMSetRenderTargets(1, &target, nullptr);  // scene depth is sampled, never bound as DSV
			c->OMSetDepthStencilState(g_noDepth, 0); c->OMSetBlendState(multiply ? g_multiply : g_opaque, nullptr, 0xFFFFFFFF);
			c->RSSetState(g_raster); const D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1 }; c->RSSetViewports(1, &vp);
			c->SetPredication(nullptr, FALSE); c->GSSetShader(nullptr, nullptr, 0); c->HSSetShader(nullptr, nullptr, 0); c->DSSetShader(nullptr, nullptr, 0);
			c->IASetInputLayout(nullptr); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			c->VSSetShader(g_vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0); c->PSSetShaderResources(0, 1, &f.depth);
			c->Draw(3, 0);
			ID3D11ShaderResourceView* none[4]{}; c->PSSetShaderResources(0, 4, none);
		}
		bool InitProbe(Probe& p, UINT width)
		{
			if (p.target) return true;
			D3D11_TEXTURE2D_DESC td{ width, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET };
			bool ok = SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, &p.target)) && SUCCEEDED(g_device->CreateRenderTargetView(p.target, nullptr, &p.rtv));
			td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ok = ok && SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, &p.staging));
			if (!ok) { Release(p.target); Release(p.rtv); Release(p.staging); }
			return ok;
		}
		void QueueProbe(const Frame& f, Probe& p, ID3D11PixelShader* ps, UINT width, const Constants& cb)
		{
			if (p.pending || !InitProbe(p, width)) return;
			RenderState::StateBackup backup; backup.Save(f.context);
			// Unbind the DSV before Bind, because Bind can restore t2/t3 and Fullscreen reads t0.
			f.context->OMSetRenderTargets(0, nullptr, nullptr);
			if (Bind(f, cb)) {
				Fullscreen(f, p.rtv, ps, width, 1);
				f.context->CopyResource(p.staging, p.target); p.pending = true;
			}
			backup.Restore(f.context);
		}
		void ReadProbes(const Frame& f)
		{
			D3D11_MAPPED_SUBRESOURCE m{};
			if (g_maskProbe.pending && f.context->Map(g_maskProbe.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
				const auto* px = static_cast<const float*>(m.pData);
				float high = 0; bool valid = true;
				for (int i = 0; i < 16; ++i) { const float v = px[i * 4]; valid = valid && std::isfinite(v) && v >= 0 && v <= 1; high = std::max(high, v); }
				const float feet = px[16 * 4];
				g_sunVis = std::isfinite(feet) ? std::clamp(feet, 0.0f, 1.0f) : 1.0f;
				bool good = valid && high > 0.5f;
				static int failures = 0;   // one failed probe (a load, a dialogue snap) must not flicker the shadows off
				if (good) failures = 0; else if (++failures < 2) good = g_maskGood;
				if (good != g_maskGood || g_frames < 5) logger::info("shadows: periodic mask probe {} max {}", good ? "passed" : "failed (sunVis=1)", high);
				g_maskGood = good; g_maskTime = Clock::now(); g_maskEpoch = g_maskQueueEpoch;
				g_sampleFeet = g_probeFeet; ++g_probeSerial;
				f.context->Unmap(g_maskProbe.staging, 0); g_maskProbe.pending = false;
			}
			if (g_cascadeProbe.pending && f.context->Map(g_cascadeProbe.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
				const auto* px = static_cast<const float*>(m.pData);
				g_minLit = 1; bool valid = false;
				static bool logged = false;
				for (int i = 0; i < 16; ++i) {
					const float* raw = px + i * 4; const float lit = px[(i + 16) * 4];
					const bool inside = std::isfinite(raw[3]) && raw[3] < 10 && raw[3] >= 0 && std::isfinite(raw[0]) && std::isfinite(raw[1]);
					valid = valid || inside;
					if (inside && std::isfinite(lit)) g_minLit = std::min(g_minLit, lit);
					if (!logged) logger::info("shadows: SkyShadowProbePS {} depth {} stored {} flipped-v {} cascade/outside {} lit {}", i, raw[0], raw[1], raw[2], raw[3], lit);
				}
				logged = true; g_cascadeProbeGood = valid; g_cascadeProbeTime = Clock::now();
				f.context->Unmap(g_cascadeProbe.staging, 0); g_cascadeProbe.pending = false;
			}
		}
	}

	bool Install()
	{
		REL::Relocation<std::uintptr_t> table{ RE::VTABLE_BSShadowDirectionalLight[0] };
		SunRender::func = table.write_vfunc(0x0A, SunRender::thunk);
		logger::info("shadows: BSShadowDirectionalLight::Render vfunc 0x0A installed (SkyCraft)");
		return true;
	}
	void Tick()
	{
		static RE::TESObjectCELL* cell = nullptr;
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* current = player ? player->GetParentCell() : nullptr;
		if (current != cell) { cell = current; ++g_epoch; }
		std::map<RE::FormID, RE::NiPoint3> feet;
		const auto wearers = [&](RE::FormID ref) {
			if (feet.contains(ref)) return;
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(ref);
			if (actor && actor->Is3DLoaded()) feet[ref] = actor->GetPosition();
		};
		for (const auto& instance : NpcShoulders::Current()) wearers(instance.ref);
		for (const auto& instance : NpcWeapons::Current()) wearers(instance.ref);
		{ std::lock_guard lock(g_lock); g_feet = std::move(feet); }
		// UiPass holds its capture lock while calling DrawLate. Queue/take captures here, never in DrawLate.
		static bool queued = false;
		if (g_maskCapture && g_maskDrawn && !queued) { UiPass::RequestCapture("shadow_mask"); queued = true; }
		if (queued) {
			auto capture = UiPass::TakeCapture("shadow_mask");
			if (capture.done) {
				std::lock_guard lock(g_lock); g_status["mask_capture"] = { capture.width, capture.height };
				g_maskCapture = false; g_maskDrawn = false; queued = false;
			}
		}
	}
	bool NpcFeet(RE::FormID actor, RE::NiPoint3& feet)
	{
		std::lock_guard lock(g_lock); const auto it = g_feet.find(actor);
		if (it == g_feet.end()) return false;
		feet = it->second; return true;
	}
	void ClearBlobs() { std::lock_guard lock(g_lock); g_blobs.clear(); g_blobTime = Clock::now(); g_blobEpoch = g_epoch.load(); }
	void AddBlob(const RE::NiPoint3& center, float radius, bool priority)
	{
		std::lock_guard lock(g_lock);
		if (priority && g_blobs.size() == 32) g_blobs.pop_back();
		if (g_blobs.size() < 32 && std::isfinite(radius) && radius > 0 && std::isfinite(center.x) && std::isfinite(center.y) && std::isfinite(center.z)) g_blobs.emplace_back(center, radius);
	}
	bool Receiving() { return g_receiving && g_receive.load(); }
	void BeginLate(const Frame& f)
	{
		g_receiving = false;
		LogTargets(); LogSun(Sun());
		if (!Init(f.device)) return;
		++g_frames;
		ReadProbes(f);
		const auto now = Clock::now();
		if (g_maskProbe.pending && now - g_maskQueued > std::chrono::seconds(2)) g_maskGood = false;
		auto cb = MakeConstants(f);
		if (Mask() && now >= g_nextProbe) {
			if (!g_maskProbe.pending) { QueueProbe(f, g_maskProbe, g_maskProbePs, 17, cb); g_maskQueued = now; g_maskQueueEpoch = g_epoch.load(); g_probeFeet = Avatar::Current().position; }
			if (cb.control[1] > 0 && f.depth) QueueProbe(f, g_cascadeProbe, g_cascadeProbePs, 32, cb);
			g_nextProbe = now + std::chrono::milliseconds(250);
		}
		g_receiving = Bind(f, cb) && cb.control[2] > 0;
		const float visibility = g_receiving && cb.control[0] > 0 ? g_sunVis : 1.0f;
		static float lastVis = -1; static float lastFlags[3]{ -1, -1, -1 };
		if (std::fabs(visibility - lastVis) > 0.25f || lastFlags[0] != float(g_receive.load()) || lastFlags[1] != cb.control[0] || lastFlags[2] != cb.control[1]) {
			logger::info("shadows: sunVis {} receive {} mask {} cascades {}", visibility, g_receive.load(), cb.control[0], cb.control[1]);
			lastVis = visibility; lastFlags[0] = float(g_receive.load()); lastFlags[1] = cb.control[0]; lastFlags[2] = cb.control[1];
		}
		std::lock_guard lock(g_lock);
		g_status.update({ { "frames", g_frames }, { "sunVis", visibility }, { "mask_ok", cb.control[0] > 0 }, { "fresh_capture", cb.control[1] > 0 },
			{ "cascade_count", g_count }, { "convention_detected", g_convention }, { "probe_pass", g_cascadeProbeGood && cb.control[1] > 0 && now - g_cascadeProbeTime < std::chrono::seconds(1) },
			{ "min_lit", g_minLit }, { "blob_centres", g_blobs.size() }, { "blob_draws", g_blobDraws }, { "probe_serial", g_probeSerial },
			{ "probe_feet", { g_sampleFeet.x, g_sampleFeet.y, g_sampleFeet.z } } });
	}
	void DrawBlobs(const Frame& f, ID3D11RenderTargetView* target)
	{
		std::vector<std::pair<RE::NiPoint3, float>> blobs;
		{
			std::lock_guard lock(g_lock);
			if (!g_blob || g_blobs.empty() || g_blobEpoch != g_epoch.load() || Clock::now() - g_blobTime > std::chrono::milliseconds(500)) return;
			blobs = g_blobs;
		}
		if (!f.depth || !Init(f.device)) return;
		auto cb = MakeConstants(f);
		if (cb.control[0] < 0.5f || cb.control[3] <= 0) return;  // no usable mask: never darken native shade twice
		cb.blobInfo[0] = static_cast<float>(blobs.size());
		for (std::size_t i = 0; i < blobs.size(); ++i) {
			const auto p = blobs[i].first - f.cam;
			cb.centers[i][0] = p.x; cb.centers[i][1] = p.y; cb.centers[i][2] = p.z; cb.centers[i][3] = blobs[i].second;
		}
		if (Bind(f, cb)) { Fullscreen(f, target, g_blobPs, f.width, f.height, true); ++g_blobDraws; }
	}
	void DrawMask(const Frame& f)
	{
		if (!g_maskCapture || !Mask() || !Init(f.device)) return;
		ID3D11RenderTargetView* target = nullptr; f.context->OMGetRenderTargets(1, &target, nullptr);
		if (target && Bind(f, MakeConstants(f))) { Fullscreen(f, target, g_maskPs, f.width, f.height); g_maskDrawn = true; }
		Release(target);
	}
	nlohmann::json Status()
	{
		std::lock_guard lock(g_lock); auto result = g_status;
		result.update({ { "receive", g_receive.load() }, { "blob", g_blob.load() }, { "cascades", g_cascades.load() }, { "mask_pending", g_maskCapture.load() } });
		if (!g_receive) result["sunVis"] = 1.0f;
		return result;
	}
	bool Command(std::string_view route, std::string_view value, std::string& message)
	{
		if (route == "status" && value.empty()) { message = Status().dump(); return true; }
		if (route == "mask" && value.empty()) {
			g_maskCapture = true; message = "Full-resolution mask diagnostic requested: capture_shadow_mask.png (UI overlays retained)"; return true;
		}
		std::atomic<bool>* toggle = route == "receive" ? &g_receive : route == "blob" ? &g_blob : route == "cascades" ? &g_cascades : nullptr;
		if (toggle && (value == "on" || value == "off")) { *toggle = value == "on"; message = std::format("shadows {} {}", route, value); return true; }
		message = "shadows status|mask|receive on|off|blob on|off|cascades on|off"; return false;
	}
}
