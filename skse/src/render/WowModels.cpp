#include "render/WowModels.h"
#include "render/GroundItems.h"

#include "Avatar.h"
#include "../../../wowcore/include/wowcore/model_layers.hpp"
#include "wowcore/item_preview.hpp"
#include "Portrait.h"
#include "SpellVisuals.h"

#include <d3d11.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "WowModel_PSMain.h"
#include "WowModel_VSMain.h"
#include "WowQuad_PSMain.h"
#include "WowQuad_VSMain.h"
#include "WowDecal_PSMain.h"
#include "WowDecal_VSMain.h"

namespace WowModels
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

		struct Vertex
		{
			float         pos[3];
			float         normal[3];
			float         uv[2];
			std::uint8_t  index[4];
			std::uint8_t  weight[4];
		};
		static_assert(sizeof(Vertex) == 40);

		struct ObjectCB
		{
			float world[16];
			float prevWorld[16];
			float lightDir[4];
			float lightColor[4];
			float ambientColor[4];
			float material[4];
			float tint[4];
			float uvRow0[4];
			float uvRow1[4];
		};
		struct QuadVertex
		{
			float pos[3];
			float uv[2];
			float color[4];
		};
		const D3D11_INPUT_ELEMENT_DESC elems[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
		constexpr std::size_t kMaxQuads = 8192;
		constexpr std::size_t kMaxBones = 256;

		struct Batch
		{
			std::uint32_t             indexStart = 0, indexCount = 0;
			ID3D11ShaderResourceView* srv = nullptr;
			std::uint16_t             blend = 0, flags = 0, geoset = 0;
			std::uint32_t             index = 0;   // in the model's batches
		};
		struct GpuPart
		{
			const Avatar::Part* part = nullptr;   // her parts
			ID3D11Buffer*       vb = nullptr;
			ID3D11Buffer*       ib = nullptr;
			std::vector<Batch>  batches;
		};
		std::map<const SpellVisuals::Asset*, GpuPart> g_effects;   // effect models, uploaded when first drawn
		ID3D11VertexShader* g_quadVs = nullptr;
		ID3D11PixelShader*  g_quadPs = nullptr;
		ID3D11InputLayout*  g_quadLayout = nullptr;
		ID3D11Buffer*       g_quadVb = nullptr;
		ID3D11Buffer*       g_quadIb = nullptr;

		ID3D11Device*            g_device = nullptr;
		ID3D11VertexShader*      g_vs = nullptr;
		ID3D11VertexShader*      g_decalVs = nullptr;
		ID3D11PixelShader*       g_decalPs = nullptr;
		ID3D11Buffer*            g_decalCb = nullptr;
		ID3D11BlendState*        g_decalBlend = nullptr;
		ID3D11PixelShader*       g_ps = nullptr;
		ID3D11InputLayout*       g_layout = nullptr;
		ID3D11Buffer*            g_objectCb = nullptr;
		ID3D11Buffer*            g_bonesCb = nullptr;
		ID3D11SamplerState*      g_sampler = nullptr;
		ID3D11RasterizerState*   g_raster = nullptr;
		ID3D11DepthStencilState* g_depthRead = nullptr;
		bool                     g_depthReadReversed = false;
		ID3D11BlendState*        g_blend[8]{};
		std::map<const wowcore::Image*, ID3D11ShaderResourceView*> g_textures;
		std::vector<GpuPart>     g_parts;
		GpuPart g_mountGpu;
		const Avatar::Loaded* g_mountUploaded = nullptr;
		const Avatar::Loaded*    g_uploaded = nullptr;
		bool                     g_failed = false;
		Stats                    g_stats;
		std::mutex               g_statsLock;
		std::atomic<ID3D11ShaderResourceView*> g_portrait = nullptr;
		// Target and party faces share the same renderer, with one retained texture per slot.
		struct UnitFace
		{
			ID3D11Texture2D* color = nullptr;
			ID3D11RenderTargetView* rtv = nullptr;
			ID3D11DepthStencilView* dsv = nullptr;
			ID3D11ShaderResourceView* srv = nullptr;
			ID3D11DepthStencilState* depth = nullptr;
			std::atomic<std::uint32_t> drawn = 0;
		};
		std::array<UnitFace, 5> g_unitFaces;
		RE::NiPoint3             g_prevPosition;   // last frame's, absolute (re-based on this frame's camera)
		float                    g_prevFacing = 0;
		bool                     g_havePrev = false;

		// Model space (yards, Z up, facing +X) -> camera-relative Skyrim units, turned to a Skyrim heading.
		glm::mat4 Placement(const RE::NiPoint3& a_position, float a_facing, const RE::NiPoint3& a_cam)
		{
			const glm::vec3 at(a_position.x - a_cam.x, a_position.y - a_cam.y, a_position.z - a_cam.z);
			return glm::scale(glm::rotate(glm::translate(glm::mat4(1.0f), at), glm::half_pi<float>() - a_facing, glm::vec3(0, 0, 1)),
				glm::vec3(Avatar::kUnitsPerYard));
		}

		// M2 blend modes (wowdev M2 materials): 0 opaque, 1 alpha key, 2 alpha, 3 add, 4 add by alpha, 5 modulate,
		// 6 modulate 2x, 7 blend add. Only the opaque ones write motion vectors (target 1).
		bool CreateStates(ID3D11Device* a_device)
		{
			for (int mode = 0; mode < 8; ++mode) {
				D3D11_BLEND_DESC d{};
				d.IndependentBlendEnable = TRUE;
				auto& rt = d.RenderTarget[0];
				// Colour only: a texture's alpha is not coverage (the opaque ones carry anything there), and her portrait
				// is composited by its target's alpha.
				rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
				rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
				rt.SrcBlendAlpha = D3D11_BLEND_ZERO, rt.DestBlendAlpha = D3D11_BLEND_ONE;
				const struct { D3D11_BLEND src, dst; } kModes[8] = {
					{ D3D11_BLEND_ONE, D3D11_BLEND_ZERO }, { D3D11_BLEND_ONE, D3D11_BLEND_ZERO },
					{ D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA }, { D3D11_BLEND_ONE, D3D11_BLEND_ONE },
					{ D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE }, { D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO },
					{ D3D11_BLEND_DEST_COLOR, D3D11_BLEND_SRC_COLOR }, { D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA } };
				rt.BlendEnable = mode >= 2;
				rt.SrcBlend = kModes[mode].src, rt.DestBlend = kModes[mode].dst;
				d.RenderTarget[1].RenderTargetWriteMask = mode <= 1 ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
				if (FAILED(a_device->CreateBlendState(&d, &g_blend[mode]))) {
					return false;
				}
			}
			// M2 triangles face the side they wind counter-clockwise from (the lit side: two-sided lighting turns the
			// normal of a face seen from behind).
			D3D11_RASTERIZER_DESC rd{ D3D11_FILL_SOLID, D3D11_CULL_NONE, TRUE };
			rd.DepthClipEnable = TRUE;
			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_ANISOTROPIC, sd.MaxAnisotropy = 8;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			D3D11_BUFFER_DESC ob{ sizeof(ObjectCB), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
			D3D11_BUFFER_DESC bb{ static_cast<UINT>(kMaxBones * 64), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };

			const D3D11_INPUT_ELEMENT_DESC quadElems[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
			std::vector<std::uint16_t> quadIndices;   // 0 1 2  0 2 3 per quad (8192 quads: 32768 vertices, in 16 bits)
			for (std::size_t q = 0; q < kMaxQuads; ++q) {
				const auto b = static_cast<std::uint16_t>(q * 4);
				quadIndices.insert(quadIndices.end(), { b, static_cast<std::uint16_t>(b + 1), static_cast<std::uint16_t>(b + 2), b,
					static_cast<std::uint16_t>(b + 2), static_cast<std::uint16_t>(b + 3) });
			}
			D3D11_BUFFER_DESC qv{ static_cast<UINT>(kMaxQuads * 4 * sizeof(QuadVertex)), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE };
			D3D11_BUFFER_DESC qi{ static_cast<UINT>(quadIndices.size() * 2), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER };
			D3D11_SUBRESOURCE_DATA qis{ quadIndices.data() };
			if (FAILED(a_device->CreateVertexShader(g_WowQuad_VSMain, sizeof(g_WowQuad_VSMain), nullptr, &g_quadVs)) ||
				FAILED(a_device->CreatePixelShader(g_WowQuad_PSMain, sizeof(g_WowQuad_PSMain), nullptr, &g_quadPs)) ||
				FAILED(a_device->CreateInputLayout(quadElems, 3, g_WowQuad_VSMain, sizeof(g_WowQuad_VSMain), &g_quadLayout)) ||
				FAILED(a_device->CreateBuffer(&qv, nullptr, &g_quadVb)) || FAILED(a_device->CreateBuffer(&qi, &qis, &g_quadIb))) {
				return false;
			}
			D3D11_BUFFER_DESC dc{ 128, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
			D3D11_BLEND_DESC  db{};
			// Added to the ground, as the client adds it (on snow a hostile circle raises red alone, evidence/charref/
			// wow_circle_hostile.png).
			db.RenderTarget[0] = { TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ZERO, D3D11_BLEND_ONE,
				D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE };
			if (FAILED(a_device->CreateVertexShader(g_WowDecal_VSMain, sizeof(g_WowDecal_VSMain), nullptr, &g_decalVs)) ||
				FAILED(a_device->CreatePixelShader(g_WowDecal_PSMain, sizeof(g_WowDecal_PSMain), nullptr, &g_decalPs)) ||
				FAILED(a_device->CreateBuffer(&dc, nullptr, &g_decalCb)) || FAILED(a_device->CreateBlendState(&db, &g_decalBlend))) {
				return false;
			}
			return SUCCEEDED(a_device->CreateVertexShader(g_WowModel_VSMain, sizeof(g_WowModel_VSMain), nullptr, &g_vs)) &&
			       SUCCEEDED(a_device->CreatePixelShader(g_WowModel_PSMain, sizeof(g_WowModel_PSMain), nullptr, &g_ps)) &&
			       SUCCEEDED(a_device->CreateInputLayout(elems, 5, g_WowModel_VSMain, sizeof(g_WowModel_VSMain), &g_layout)) &&
			       SUCCEEDED(a_device->CreateRasterizerState(&rd, &g_raster)) && SUCCEEDED(a_device->CreateSamplerState(&sd, &g_sampler)) &&
			       SUCCEEDED(a_device->CreateBuffer(&ob, nullptr, &g_objectCb)) && SUCCEEDED(a_device->CreateBuffer(&bb, nullptr, &g_bonesCb));
		}

		void EnsureDepthRead(bool a_reversed)
		{
			if (g_depthRead && g_depthReadReversed == a_reversed) {
				return;
			}
			Release(g_depthRead);
			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = TRUE;
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			dd.DepthFunc = a_reversed ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL;
			g_device->CreateDepthStencilState(&dd, &g_depthRead);
			g_depthReadReversed = a_reversed;
		}

		// A WoW image as a mipmapped texture (the client samples minified art from its mips).
		ID3D11ShaderResourceView* Texture(ID3D11DeviceContext* a_context, const wowcore::Image* a_image,
			ID3D11Device* a_device = g_device, std::map<const wowcore::Image*, ID3D11ShaderResourceView*>& a_cache = g_textures)
		{
			if (!a_image || a_image->rgba.empty()) {
				return nullptr;
			}
			if (auto it = a_cache.find(a_image); it != a_cache.end()) {
				return it->second;
			}
			D3D11_TEXTURE2D_DESC d{};
			d.Width = static_cast<UINT>(a_image->w), d.Height = static_cast<UINT>(a_image->h);
			d.MipLevels = 0, d.ArraySize = 1, d.Format = DXGI_FORMAT_R8G8B8A8_UNORM, d.SampleDesc.Count = 1;
			d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
			ID3D11Texture2D*          tex = nullptr;
			ID3D11ShaderResourceView* srv = nullptr;
			if (SUCCEEDED(a_device->CreateTexture2D(&d, nullptr, &tex))) {
				a_context->UpdateSubresource(tex, 0, nullptr, a_image->rgba.data(), static_cast<UINT>(a_image->w) * 4, 0);
				D3D11_SHADER_RESOURCE_VIEW_DESC v{};
				v.Format = d.Format, v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, v.Texture2D.MipLevels = static_cast<UINT>(-1);
				if (SUCCEEDED(a_device->CreateShaderResourceView(tex, &v, &srv))) {
					a_context->GenerateMips(srv);
				}
				tex->Release();
			}
			a_cache[a_image] = srv;
			return srv;
		}

		bool Upload(ID3D11DeviceContext* a_context, const wowcore::Model& m, const std::vector<const wowcore::Image*>& a_images, GpuPart& a_out,
			ID3D11Device* a_device = g_device, std::map<const wowcore::Image*, ID3D11ShaderResourceView*>& a_cache = g_textures)
		{
			std::vector<Vertex> verts(m.vertices.size());
			for (std::size_t i = 0; i < verts.size(); ++i) {
				const auto& s = m.vertices[i];
				auto&       v = verts[i];
				std::memcpy(v.pos, &s.position, 12);
				std::memcpy(v.normal, &s.normal, 12);
				std::memcpy(v.uv, &s.texCoords[0], 8);
				for (int k = 0; k < 4; ++k) {
					v.index[k] = s.boneIndices[k];
					v.weight[k] = s.boneWeights[k];
				}
			}
			D3D11_BUFFER_DESC     vd{ static_cast<UINT>(verts.size() * sizeof(Vertex)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
			D3D11_SUBRESOURCE_DATA vs{ verts.data() };
			D3D11_BUFFER_DESC     id{ static_cast<UINT>(m.indices.size() * 2), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER };
			D3D11_SUBRESOURCE_DATA is{ m.indices.data() };
			if (verts.empty() || m.indices.empty() || FAILED(a_device->CreateBuffer(&vd, &vs, &a_out.vb)) || FAILED(a_device->CreateBuffer(&id, &is, &a_out.ib))) {
				return false;
			}
			for (std::size_t i = 0; i < m.batches.size(); ++i) {
				const auto& b = m.batches[i];
				const auto* mat = b.materialIndex < m.materials.size() ? &m.materials[b.materialIndex] : nullptr;
				a_out.batches.push_back({ b.indexStart, b.indexCount, Texture(a_context, i < a_images.size() ? a_images[i] : nullptr, a_device, a_cache),
					static_cast<std::uint16_t>(mat ? std::min<std::uint16_t>(mat->blendMode, 7) : 0), static_cast<std::uint16_t>(mat ? mat->flags : 0),
					b.submeshId, static_cast<std::uint32_t>(i) });
			}
			return true;
		}

		bool Write(ID3D11DeviceContext* a_context, ID3D11Buffer* a_buffer, const void* a_data, std::size_t a_size)
		{
			D3D11_MAPPED_SUBRESOURCE m{};
			if (SUCCEEDED(a_context->Map(a_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
				std::memcpy(m.pData, a_data, a_size);
				a_context->Unmap(a_buffer, 0);
				return true;
			}
			return false;
		}

		// glm is column-major; the shader's row_major matrices want the transpose in memory.
		void Store(float* a_out, const glm::mat4& a_m) { std::memcpy(a_out, glm::value_ptr(glm::transpose(a_m)), 64); }

		// Skyrim's light where she stands: the sun (direction and colour) and the ambient from above.
		// How bright Skyrim made the ground around her this moment, against a clear day's: she is drawn after Skyrim's
		// tonemapping (her colours as the client's), so its exposure - night, a dark interior, a torch beside her - is
		// matched here. Eight points on a ring at her feet are read from the frame before she is drawn, three frames
		// late (no stall), their mean luminance followed smoothly.
		float g_exposure = 1.0f;
		float g_ground = 0.0f;   // the last reading (logged)
		constexpr int   kPoints = 8;
		constexpr float kDayGround = 0.15f;   // linear luminance: Riverwood's ground reads 0.18-0.34 by day (exposure 1), 0.04 at 23:30

		void Expose(const Frame& a_frame, const RE::NiPoint3& a_feet)
		{
			static ID3D11Texture2D* staging[3]{};
			static bool             filled[3]{};
			static DXGI_FORMAT      format = DXGI_FORMAT_UNKNOWN;
			static std::uint64_t   frame = 0;
			auto*                   ctx = a_frame.context;
			ID3D11RenderTargetView* rtv = nullptr;
			ctx->OMGetRenderTargets(1, &rtv, nullptr);
			if (!rtv) {
				return;
			}
			ID3D11Resource* target = nullptr;
			rtv->GetResource(&target);
			Release(rtv);
			D3D11_TEXTURE2D_DESC desc{};
			static_cast<ID3D11Texture2D*>(target)->GetDesc(&desc);
			if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
				Release(target);
				return;   // an 8-bit frame is what this reads
			}
			if (format != desc.Format) {
				for (auto*& t : staging) {
					Release(t);
				}
				D3D11_TEXTURE2D_DESC s{ kPoints, 1, 1, 1, desc.Format, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
				for (auto*& t : staging) {
					a_frame.device->CreateTexture2D(&s, nullptr, &t);
				}
				format = desc.Format;
				std::ranges::fill(filled, false);
			}
			const int now = static_cast<int>(frame++ % 3);
			if (filled[now] && staging[now]) {   // three frames ago
				D3D11_MAPPED_SUBRESOURCE m{};
				if (SUCCEEDED(ctx->Map(staging[now], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
					const auto* px = static_cast<const std::uint8_t*>(m.pData);
					float       sum = 0;
					for (int i = 0; i < kPoints; ++i) {
						const bool bgr = format == DXGI_FORMAT_B8G8R8A8_UNORM;
						const auto lin = [](std::uint8_t v) { return std::pow(v / 255.0f, 2.2f); };
						const float r = lin(px[i * 4 + (bgr ? 2 : 0)]), g = lin(px[i * 4 + 1]), b = lin(px[i * 4 + (bgr ? 0 : 2)]);
						sum += 0.2126f * r + 0.7152f * g + 0.0722f * b;
					}
					ctx->Unmap(staging[now], 0);
					g_ground = sum / kPoints;
					const float wanted = std::clamp(g_ground / kDayGround, 0.12f, 1.0f);
					g_exposure += (wanted - g_exposure) * 0.05f;   // about a second to follow
				}
			}
			filled[now] = false;
			if (staging[now]) {
				const glm::mat4 vp = glm::transpose(glm::make_mat4(a_frame.viewProj));
				for (int i = 0; i < kPoints; ++i) {
					const float     a = glm::two_pi<float>() * i / kPoints;
					const glm::vec4 p = vp * glm::vec4(a_feet.x - a_frame.cam.x + std::cos(a) * 96.0f, a_feet.y - a_frame.cam.y + std::sin(a) * 96.0f,
												 a_feet.z - a_frame.cam.z + 4.0f, 1.0f);
					const float x = (p.x / p.w * 0.5f + 0.5f) * desc.Width, y = (0.5f - p.y / p.w * 0.5f) * desc.Height;
					const UINT  cx = static_cast<UINT>(std::clamp(x, 0.0f, desc.Width - 1.0f)), cy = static_cast<UINT>(std::clamp(y, 0.0f, desc.Height - 1.0f));
					const D3D11_BOX box{ cx, cy, 0, cx + 1, cy + 1, 1 };
					ctx->CopySubresourceRegion(staging[now], 0, i, 0, 0, target, 0, &box);
				}
				filled[now] = true;
			}
			Release(target);
		}

		void Light(ObjectCB& a_cb)
		{
			glm::vec3 toLight(0.3f, -0.4f, 0.85f), color(1.0f), ambient(0.45f);
			auto&     state = RE::BSShaderManager::State::GetSingleton();
			if (auto* ssn = state.shadowSceneNode[0]) {
				auto* sun = ssn->GetRuntimeData().sunLight;
				if (auto* light = sun ? netimmerse_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr) {
					const auto& d = light->GetDirectionalLightRuntimeData().worldDir;
					if (d.Length() > 0.1f) {
						toLight = -glm::normalize(glm::vec3(d.x, d.y, d.z));
					}
					const auto& c = light->GetLightRuntimeData().diffuse;
					color = glm::vec3(c.red, c.green, c.blue);
				}
			}
			const auto& amb = state.directionalAmbientTransform;
			ambient = glm::vec3(amb.rotate.entry[0][2] + amb.translate.x, amb.rotate.entry[1][2] + amb.translate.y, amb.rotate.entry[2][2] + amb.translate.z);
			ambient = glm::max(ambient, glm::vec3(0.05f));
			// The client lights a model so that a face turned to the sun sees about 1 (ambient plus diffuse); WoW's art is
			// painted for that. Skyrim's light keeps its colour and its sun/ambient balance but is brought into that range.
			const glm::vec3 peak = ambient + color;
			const float     scale = g_exposure / std::max(1.0f, std::max({ peak.r, peak.g, peak.b }));
			ambient *= scale;
			color *= scale;
			static int logged = 0;
			if (logged++ % 600 == 0) {
				logger::info("wow models: sun {:.2f} {:.2f} {:.2f} towards {:.2f} {:.2f} {:.2f}, ambient {:.2f} {:.2f} {:.2f}, exposure {:.2f} (ground {:.3f})",
					color.r, color.g, color.b, toLight.x, toLight.y, toLight.z, ambient.r, ambient.g, ambient.b, g_exposure, g_ground);
			}
			std::memcpy(a_cb.lightDir, glm::value_ptr(glm::vec4(toLight, 0.0f)), 16);
			std::memcpy(a_cb.lightColor, glm::value_ptr(glm::vec4(color, 1.0f)), 16);
			std::memcpy(a_cb.ambientColor, glm::value_ptr(glm::vec4(ambient, 1.0f)), 16);
		}

		int SequenceFor(const wowcore::Model& a_model, std::uint32_t a_animation)
		{
			return wowcore::sequenceWithFallback(a_model, a_animation);
		}

		float TimeIn(const wowcore::Model& a_model, int a_seq, float a_timeMs, bool a_loop)
		{
			if (a_seq < 0 || static_cast<std::size_t>(a_seq) >= a_model.sequences.size()) {
				return 0.0f;
			}
			const float duration = static_cast<float>(std::max<std::uint32_t>(a_model.sequences[a_seq].duration, 1));
			return a_loop ? std::fmod(a_timeMs, duration) : std::min(a_timeMs, duration - 1.0f);
		}

		struct DrawResources
		{
			ID3D11Buffer* objectCb;
			ID3D11Buffer* bonesCb;
			ID3D11DepthStencilState* depthRead;
			ID3D11BlendState** blend;
			bool menu = false;
		};

		// Her parts as they are; an effect model (a_look) with each batch's colour and texture animation at its time.
		bool DrawPart(ID3D11DeviceContext* a_context, const GpuPart& a_part, const std::vector<glm::mat4>& a_bones, ObjectCB& a_cb,
			const glm::mat4& a_world, const glm::mat4& a_prevWorld, const std::set<std::uint16_t>* a_geosets, int a_pass,
			ID3D11DepthStencilState* a_depthWrite, std::uint32_t& a_drawn, const wowcore::Model* a_look = nullptr, float a_timeMs = 0,
			int a_sequence = 0, const DrawResources* a_resources = nullptr)
		{
			const DrawResources resources = a_resources ? *a_resources : DrawResources{ g_objectCb, g_bonesCb, g_depthRead, g_blend };
			std::array<glm::mat4, kMaxBones> palette;
			palette.fill(glm::mat4(1.0f));
			const std::size_t                count = std::min(a_bones.size(), kMaxBones);
			for (std::size_t i = 0; i < count; ++i) {
				palette[i] = glm::transpose(a_bones[i]);
			}
			if (!Write(a_context, resources.bonesCb, palette.data(), std::max<std::size_t>(count, 1) * 64)) { return false; }
			Store(a_cb.world, a_world);
			Store(a_cb.prevWorld, a_prevWorld);
			const UINT stride = sizeof(Vertex), zero = 0;
			a_context->IASetVertexBuffers(0, 1, &a_part.vb, &stride, &zero);
			a_context->IASetIndexBuffer(a_part.ib, DXGI_FORMAT_R16_UINT, 0);
			for (const auto& b : a_part.batches) {
				if ((a_geosets && !a_geosets->count(b.geoset)) || !b.srv || (b.blend >= 2) != (a_pass == 1)) {
					continue;
				}
				a_cb.material[0] = b.blend == 1 ? 0.878f : 0.0f;   // alpha key at the client's 224/255
				a_cb.material[1] = (b.flags & 0x1) ? 1.0f : 0.0f;  // unlit
				const bool fadedOpaque = a_cb.material[2] > 0.0f && b.blend <= 1;   // her stealth: the opaque ones blended
				a_cb.material[3] = resources.menu ? (b.blend >= 2 ? 24.0f : 8.0f) : fadedOpaque ? 4.0f : 0.0f;
				wowcore::BatchLook look;
				if (a_look && b.index < a_look->batches.size()) {
					look = wowcore::batchLook(*a_look, a_look->batches[b.index], a_sequence, a_timeMs, a_timeMs);
					if (look.color.a <= 0.003f) {
						continue;
					}
				}
				std::memcpy(a_cb.tint, glm::value_ptr(look.color), 16);
				const float rows[8] = { look.uv[0][0], look.uv[1][0], look.uv[2][0], 0, look.uv[0][1], look.uv[1][1], look.uv[2][1], 0 };
				std::memcpy(a_cb.uvRow0, rows, 32);
				if (!Write(a_context, resources.objectCb, &a_cb, sizeof(a_cb))) { return false; }
				a_context->OMSetBlendState(resources.blend[fadedOpaque ? 2 : b.blend], nullptr, 0xFFFFFFFF);
				a_context->OMSetDepthStencilState(b.blend <= 1 && !(b.flags & 0x10) ? a_depthWrite : resources.depthRead, 0);
				a_context->PSSetShaderResources(0, 1, &b.srv);
				a_context->DrawIndexed(b.indexCount, b.indexStart, 0);
				++a_drawn;
			}
			return true;
		}
	}

	namespace
	{
		// Separate ownership from avatar uploads: redressing her must not release dropped-item textures.
		struct GroundGpu
		{
			std::vector<GpuPart> parts;
			std::map<const wowcore::Image*, ID3D11ShaderResourceView*> textures;
			std::vector<std::vector<glm::mat4>> bones;
			std::vector<int> sequences;
			bool ready = false;
			~GroundGpu()
			{
				for (auto& p : parts) { Release(p.vb); Release(p.ib); }
				for (auto& [image, texture] : textures) { Release(texture); }
			}
		};
		using ItemPtr = std::shared_ptr<const wowcore::ItemPreview>;
		std::map<ItemPtr, std::unique_ptr<GroundGpu>, std::owner_less<ItemPtr>> g_groundGpu;
		std::mutex g_groundLock;
		std::map<ItemPtr, std::chrono::steady_clock::time_point, std::owner_less<ItemPtr>> g_groundReady;

		GroundGpu& GroundUpload(ID3D11DeviceContext* context, const ItemPtr& item)
		{
			if (const auto it = g_groundGpu.find(item); it != g_groundGpu.end()) { return *it->second; }
			auto gpu = std::make_unique<GroundGpu>();
			const auto count = item->armorOnly ? item->parts.size() : 1;
			gpu->parts.resize(count); gpu->bones.resize(count); gpu->sequences.resize(count);
			bool ready = count > 0;
			for (std::size_t i = 0; i < count && ready; ++i) {
				const auto& part = item->parts[i];
				std::vector<const wowcore::Image*> images;
				for (const auto& path : part.batchTextures) {
					const auto it = part.images.find(path);
					images.push_back(it == part.images.end() ? nullptr : &it->second);
				}
				ready = part.model.bones.size() <= kMaxBones && Upload(context, part.model, images, gpu->parts[i], g_device, gpu->textures) &&
					!gpu->parts[i].batches.empty() && !std::ranges::any_of(gpu->parts[i].batches, [](const auto& b) { return !b.srv || b.blend > 7; });
				gpu->sequences[i] = SequenceFor(part.model, 0);
				wowcore::poseBones(part.model, gpu->sequences[i], 0, 0, gpu->bones[i]);
				ready = ready && gpu->bones[i].size() <= kMaxBones;
			}
			gpu->ready = ready;
			logger::info("ground items: upload display={} parts={} ready={}", item->displayId, count, ready);
			return *g_groundGpu.emplace(item, std::move(gpu)).first->second;
		}

		void DrawGround(const Frame& frame)
		{
			const auto instances = GroundItems::Current();
			std::vector<GroundItems::DrawInfo> draws;
			std::map<ItemPtr, bool, std::owner_less<ItemPtr>> ready;
			auto* c = frame.context;
			c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			c->IASetInputLayout(g_layout);
			c->VSSetShader(g_vs, nullptr, 0);
			c->PSSetShader(g_ps, nullptr, 0);
			ID3D11Buffer* cbs[] = { frame.frameCb, g_objectCb, g_bonesCb };
			c->VSSetConstantBuffers(0, 3, cbs); c->PSSetConstantBuffers(0, 3, cbs);
			c->PSSetSamplers(0, 1, &g_sampler); c->RSSetState(g_raster);
			ObjectCB cb{};
			if (!instances.empty()) { Light(cb); }
			const auto toCamera = glm::translate(glm::mat4(1.0f), glm::vec3(-frame.cam.x, -frame.cam.y, -frame.cam.z));
			// All opaque/alpha-key geometry writes scene depth before the blended batches, including overlaps
			// between different references. No private/menu depth or menu lighting is used here.
			std::vector<std::uint32_t> batches(instances.size());
			std::vector<bool> ok(instances.size(), true);
			for (int pass = 0; pass < 2; ++pass) {
				for (std::size_t n = 0; n < instances.size(); ++n) {
					const auto& instance = instances[n];
					const auto& item = instance.asset->item;
					auto& gpu = GroundUpload(c, item);
					if (!gpu.ready) { ready[item] = false; continue; }
					ready.try_emplace(item, true);  // the main thread can hide it on the next frame
					if (!instance.hidden) { continue; }
					for (std::size_t i = 0; i < gpu.parts.size(); ++i) {
						const auto& part = item->parts[i];
						const glm::mat4 world = toCamera * instance.world * part.modelToPreview;
						ok[n] = DrawPart(c, gpu.parts[i], gpu.bones[i], cb, world, world, nullptr, pass, frame.depthWrite,
							batches[n], &part.model, 0, gpu.sequences[i]) && ok[n];
					}
					if (!ok[n]) { ready[item] = false; }
				}
			}
			const bool deviceOk = SUCCEEDED(frame.device->GetDeviceRemovedReason());
			for (std::size_t n = 0; n < instances.size(); ++n) {
				const auto& instance = instances[n];
				if (deviceOk && ok[n] && batches[n]) {
					draws.push_back({ instance.ref, instance.entry, instance.asset->item->displayId, batches[n], instance.world });
				}
			}
			{
				std::lock_guard lock(g_groundLock);
				for (const auto& [item, success] : ready) {
					if (success && deviceOk) { g_groundReady[item] = std::chrono::steady_clock::now(); }
					else { g_groundReady.erase(item); }
				}
			}
			GroundItems::RecordDraws(std::move(draws));
		}

		// Her face, drawn once into a 128 x 128 texture through the model's portrait camera: the body (stand, first
		// frame) and what hangs on her head and shoulders, lit from the camera at the level measured off the client's
		// (evidence/charref/portrait_client_vs_skyrim.png: the hood within a few percent).
		void DrawPortrait(const Frame& a_frame, const Avatar::Loaded& a_data)
		{
			const auto& body = a_data.body.model;
			const auto  cam = std::ranges::find_if(body.cameras, [](const auto& c) { return c.type == 0; });
			if (cam == body.cameras.end()) {
				return;
			}
			constexpr UINT kSize = 128;
			D3D11_TEXTURE2D_DESC td{ kSize, kSize, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
				D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE };
			D3D11_TEXTURE2D_DESC dd{ kSize, kSize, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL };
			ID3D11Texture2D *color = nullptr, *depth = nullptr;
			ID3D11RenderTargetView*   rtv = nullptr;
			ID3D11DepthStencilView*   dsv = nullptr;
			ID3D11ShaderResourceView* srv = nullptr;
			ID3D11DepthStencilState*  less = nullptr;
			D3D11_DEPTH_STENCIL_DESC  ds{};
			ds.DepthEnable = TRUE, ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			const bool ok = SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, &color)) && SUCCEEDED(g_device->CreateTexture2D(&dd, nullptr, &depth)) &&
			                SUCCEEDED(g_device->CreateRenderTargetView(color, nullptr, &rtv)) && SUCCEEDED(g_device->CreateDepthStencilView(depth, nullptr, &dsv)) &&
			                SUCCEEDED(g_device->CreateShaderResourceView(color, nullptr, &srv)) && SUCCEEDED(g_device->CreateDepthStencilState(&ds, &less));
			Release(color), Release(depth);
			if (!ok) {
				Release(rtv), Release(dsv), Release(srv), Release(less);
				return;
			}
			auto* context = a_frame.context;
			const float clear[4] = { 0, 0, 0, 1 };   // the client's portraits are on black
			context->ClearRenderTargetView(rtv, clear);
			context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
			context->OMSetRenderTargets(1, &rtv, dsv);
			const D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1 };
			context->RSSetViewports(1, &vp);
			// The camera: model space, Z up; D3D's depth range.
			const glm::mat4 view = glm::lookAtRH(cam->positionBase, cam->targetBase, glm::vec3(0, 0, 1));
			// An M2 camera's field of view is diagonal; square, the vertical one is narrower by sqrt 2 (in tangent).
			const float     vertical = 2.0f * std::atan(std::tan(cam->fov * 0.5f) / std::sqrt(2.0f));
			const glm::mat4 proj = glm::perspectiveRH_ZO(vertical, 1.0f, cam->nearClip, cam->farClip);
			float frame[32];
			Store(frame, proj * view);
			Store(frame + 16, proj * view);
			Write(context, a_frame.frameCb, frame, sizeof(frame));
			ObjectCB cb{};
			const glm::vec3 toLight = glm::normalize(cam->positionBase - cam->targetBase + glm::vec3(0, 0, 0.5f));
			std::memcpy(cb.lightDir, glm::value_ptr(glm::vec4(toLight, 0)), 16);
			std::memcpy(cb.lightColor, glm::value_ptr(glm::vec4(0.56f, 0.56f, 0.56f, 1)), 16);
			std::memcpy(cb.ambientColor, glm::value_ptr(glm::vec4(0.42f, 0.42f, 0.42f, 1)), 16);
			std::vector<glm::mat4> bones;
			wowcore::poseBones(body, std::max(wowcore::findSequence(body, 0), 0), 0.0f, 0.0f, bones);
			std::uint32_t drawn = 0;
			for (int pass = 0; pass < 2; ++pass) {
				DrawPart(context, g_parts[0], bones, cb, glm::mat4(1.0f), glm::mat4(1.0f), &a_data.look.geosets, pass, less, drawn);
				for (std::size_t i = 1; i < g_parts.size(); ++i) {
					const auto& part = *g_parts[i].part;
					if (part.attached->sheathed) {
						continue;   // the staff is not in the picture
					}
					if (const auto place = wowcore::placeAttached(body, bones, *part.attached, false)) {
						std::vector<glm::mat4> itemBones;
						wowcore::poseBones(part.model, 0, 0.0f, 0.0f, itemBones);
						DrawPart(context, g_parts[i], itemBones, cb, *place, *place, nullptr, pass, less, drawn);
					}
				}
			}
			Release(rtv), Release(dsv), Release(less);
			g_portrait = srv;
			logger::info("wow models: her portrait drawn ({} batches)", drawn);
		}

		ID3D11ShaderResourceView* SkyrimTexture(const RE::NiPointer<RE::NiSourceTexture>& a_texture)
		{
			auto* t = a_texture ? a_texture->rendererTexture : nullptr;
			return t ? reinterpret_cast<ID3D11ShaderResourceView*>(t->resourceView) : nullptr;
		}

		// The target's face, as the client draws a unit's portrait: from in front and to its right (the angle of a
		// player model's own portrait camera), close, lit from the eye.
		void DrawTargetPortrait(const Frame& a_frame, const Portrait::Head& a_head, UnitFace& a_face)
		{
			constexpr UINT kSize = 128;
			auto*          context = a_frame.context;
			if (!a_face.rtv) {
				D3D11_TEXTURE2D_DESC td{ kSize, kSize, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
					D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE };
				D3D11_TEXTURE2D_DESC dd{ kSize, kSize, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL };
				ID3D11Texture2D*     depth = nullptr;
				D3D11_DEPTH_STENCIL_DESC ds{};
				ds.DepthEnable = TRUE, ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
				const bool ok = SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, &a_face.color)) && SUCCEEDED(g_device->CreateTexture2D(&dd, nullptr, &depth)) &&
				                SUCCEEDED(g_device->CreateRenderTargetView(a_face.color, nullptr, &a_face.rtv)) &&
				                SUCCEEDED(g_device->CreateDepthStencilView(depth, nullptr, &a_face.dsv)) &&
				                SUCCEEDED(g_device->CreateShaderResourceView(a_face.color, nullptr, &a_face.srv)) &&
				                SUCCEEDED(g_device->CreateDepthStencilState(&ds, &a_face.depth));
				Release(depth);
				if (!ok) {
					Release(a_face.color), Release(a_face.rtv), Release(a_face.dsv), Release(a_face.srv), Release(a_face.depth);
					return;
				}
			}
			ID3D11RenderTargetView* target = nullptr;
			ID3D11DepthStencilView* targetDepth = nullptr;
			D3D11_VIEWPORT          viewport{};
			UINT                    viewports = 1;
			context->OMGetRenderTargets(1, &target, &targetDepth);
			context->RSGetViewports(&viewports, &viewport);
			const float clear[4] = { 0, 0, 0, 1 };
			context->ClearRenderTargetView(a_face.rtv, clear);
			context->ClearDepthStencilView(a_face.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
			context->OMSetRenderTargets(1, &a_face.rtv, a_face.dsv);
			const D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1 };
			context->RSSetViewports(1, &vp);
			// Head space: x its right, y its facing, z up. The camera 36 degrees to its right of straight on.
			const float     vertical = 2.0f * std::atan(std::tan(0.785f * 0.5f) / std::sqrt(2.0f));   // the portrait camera's
			const glm::vec3 toEye = glm::normalize(glm::vec3(0.59f, 0.8f, 0.05f));
			const float     distance = a_head.radius / std::tan(vertical * 0.5f) * 0.95f;
			const glm::vec3 eye = a_head.centre + toEye * distance;
			const glm::mat4 viewProj = glm::perspectiveRH_ZO(vertical, 1.0f, 1.0f, distance * 4.0f) * glm::lookAtRH(eye, a_head.centre, glm::vec3(0, 0, 1));
			float frame[32];
			Store(frame, viewProj);
			Store(frame + 16, viewProj);
			Write(context, a_frame.frameCb, frame, sizeof(frame));
			ObjectCB cb{};
			Store(cb.world, glm::mat4(1.0f));
			Store(cb.prevWorld, glm::mat4(1.0f));
			std::memcpy(cb.lightDir, glm::value_ptr(glm::vec4(glm::normalize(toEye + glm::vec3(0, 0, 0.5f)), 0)), 16);
			std::memcpy(cb.lightColor, glm::value_ptr(glm::vec4(0.56f, 0.56f, 0.56f, 1)), 16);
			std::memcpy(cb.ambientColor, glm::value_ptr(glm::vec4(0.42f, 0.42f, 0.42f, 1)), 16);
			cb.uvRow0[0] = 1, cb.uvRow1[1] = 1;   // no texture animation
			const glm::mat4 identity(1.0f);
			Write(context, g_bonesCb, &identity, 64);
			const UINT stride = sizeof(Vertex), zero = 0;
			// Opaque parts, then the hair (alpha-keyed), then what lies on the head (brows).
			for (int pass = 0; pass < 3; ++pass) {
				for (const auto& p : a_head.parts) {
					if ((p.alphaKey ? 1 : p.blended ? 2 : 0) != pass) {
						continue;
					}
					auto* diffuse = SkyrimTexture(p.diffuse);
					if (!diffuse) {
						continue;
					}
					std::vector<Vertex> verts(p.positions.size());
					for (std::size_t i = 0; i < verts.size(); ++i) {
						std::memcpy(verts[i].pos, &p.positions[i], 12);
						std::memcpy(verts[i].normal, &p.normals[i], 12);
						std::memcpy(verts[i].uv, &p.uvs[i], 8);
					}
					D3D11_BUFFER_DESC      vd{ static_cast<UINT>(verts.size() * sizeof(Vertex)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
					D3D11_SUBRESOURCE_DATA vs{ verts.data() };
					D3D11_BUFFER_DESC      id{ static_cast<UINT>(p.indices.size() * 2), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER };
					D3D11_SUBRESOURCE_DATA is{ p.indices.data() };
					ID3D11Buffer *vb = nullptr, *ib = nullptr;
					if (FAILED(g_device->CreateBuffer(&vd, &vs, &vb)) || FAILED(g_device->CreateBuffer(&id, &is, &ib))) {
						Release(vb), Release(ib);
						continue;
					}
					auto* tint = SkyrimTexture(p.tint);
					cb.material[0] = p.alphaKey ? 0.5f : 0.0f;
					cb.material[1] = 0.0f;
					cb.material[2] = 0.0f;
					cb.material[3] = static_cast<float>((tint ? 1 : 0) | 2);
					std::memcpy(cb.tint, glm::value_ptr(glm::vec4(p.color, 1.0f)), 16);
					Write(context, g_objectCb, &cb, sizeof(cb));
					context->IASetVertexBuffers(0, 1, &vb, &stride, &zero);
					context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
					ID3D11ShaderResourceView* srvs[2] = { diffuse, tint };
					context->PSSetShaderResources(0, 2, srvs);
					context->OMSetBlendState(g_blend[pass == 2 ? 2 : 0], nullptr, 0xFFFFFFFF);
					context->OMSetDepthStencilState(a_face.depth, 0);
					context->DrawIndexed(static_cast<UINT>(p.indices.size()), 0, 0);
					Release(vb), Release(ib);
				}
			}
			ID3D11ShaderResourceView* none[2]{};
			context->PSSetShaderResources(0, 2, none);
			context->OMSetRenderTargets(1, &target, targetDepth);
			context->RSSetViewports(1, &viewport);
			Release(target), Release(targetDepth);
			a_face.drawn = a_head.serial;
		}

		// A ground decal laid on Skyrim's depth before she is drawn (so she stands over it): the target's selection circle,
		// turned to face the camera in its reaction's colour, or a ground spell's area in its own (world-aligned).
		void DrawDecal(const Frame& a_frame, const RE::NiPoint3& a_at, float a_radius, const std::array<float, 3>& a_color,
			const wowcore::Image& a_image, bool a_area)
		{
			auto* image = Texture(a_frame.context, &a_image);
			if (a_radius <= 0.0f || !a_frame.depth || !image) {
				return;
			}
			auto* context = a_frame.context;
			ID3D11RenderTargetView* target = nullptr;
			ID3D11DepthStencilView* depth = nullptr;
			context->OMGetRenderTargets(1, &target, &depth);
			D3D11_VIEWPORT vp{};
			UINT           viewports = 1;
			context->RSGetViewports(&viewports, &vp);
			struct
			{
				float invViewProj[16];
				float center[4];
				float toCamera[4];
				float color[4];
				float viewport[4];
			} cb{};
			Store(cb.invViewProj, glm::inverse(glm::transpose(glm::make_mat4(a_frame.viewProj))));
			const glm::vec3 at(a_at.x - a_frame.cam.x, a_at.y - a_frame.cam.y, a_at.z - a_frame.cam.z);
			const glm::vec2 level(-at.x, -at.y);   // the camera is at the origin
			const glm::vec2 toCamera = a_area ? glm::vec2(0.0f, -1.0f) : glm::length(level) > 1.0f ? glm::normalize(level) : glm::vec2(0.0f, -1.0f);
			std::memcpy(cb.center, glm::value_ptr(glm::vec4(at, a_radius)), 16);
			std::memcpy(cb.toCamera, glm::value_ptr(glm::vec4(toCamera, a_area ? 1.0f : 0.0f, 0.0f)), 16);
			std::memcpy(cb.color, a_color.data(), 12);
			cb.color[3] = 1.0f;
			cb.viewport[0] = vp.Width, cb.viewport[1] = vp.Height;
			Write(context, g_decalCb, &cb, sizeof(cb));
			context->OMSetRenderTargets(1, &target, nullptr);   // its depth is read, not tested
			context->IASetInputLayout(nullptr);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->VSSetShader(g_decalVs, nullptr, 0);
			context->PSSetShader(g_decalPs, nullptr, 0);
			context->PSSetConstantBuffers(0, 1, &g_decalCb);
			ID3D11ShaderResourceView* srvs[2] = { a_frame.depth, image };
			context->PSSetShaderResources(0, 2, srvs);
			context->PSSetSamplers(0, 1, &g_sampler);
			context->OMSetBlendState(g_decalBlend, nullptr, 0xFFFFFFFF);
			context->RSSetState(g_raster);
			context->Draw(3, 0);
			ID3D11ShaderResourceView* none[2]{};
			context->PSSetShaderResources(0, 2, none);
			context->OMSetRenderTargets(1, &target, depth);
			Release(target), Release(depth);
			std::lock_guard lock(g_statsLock);
			++g_stats.circles;
		}

		// The particles and ribbons: written into the dynamic buffer camera-relative, drawn by texture and blend in turn.
		void DrawQuads(const Frame& a_frame, const std::vector<SpellVisuals::DrawQuads>& a_groups, std::uint32_t& a_drawn)
		{
			auto* context = a_frame.context;
			context->IASetInputLayout(g_quadLayout);
			context->VSSetShader(g_quadVs, nullptr, 0);
			context->PSSetShader(g_quadPs, nullptr, 0);
			const UINT stride = sizeof(QuadVertex), zero = 0;
			context->IASetVertexBuffers(0, 1, &g_quadVb, &stride, &zero);
			context->IASetIndexBuffer(g_quadIb, DXGI_FORMAT_R16_UINT, 0);
			context->OMSetDepthStencilState(g_depthRead, 0);
			for (const auto& group : a_groups) {
				for (std::size_t start = 0; start < group.quads.size(); start += kMaxQuads) {
					const std::size_t count = std::min(kMaxQuads, group.quads.size() - start);
					D3D11_MAPPED_SUBRESOURCE m{};
					if (FAILED(context->Map(g_quadVb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
						return;
					}
					auto* v = static_cast<QuadVertex*>(m.pData);
					for (std::size_t i = 0; i < count; ++i) {
						const auto& q = group.quads[start + i];
						for (int k = 0; k < 4; ++k) {
							auto& o = v[i * 4 + k];
							o.pos[0] = q.corner[k].x - a_frame.cam.x, o.pos[1] = q.corner[k].y - a_frame.cam.y, o.pos[2] = q.corner[k].z - a_frame.cam.z;
							o.uv[0] = q.uv[k].x, o.uv[1] = q.uv[k].y;
							std::memcpy(o.color, glm::value_ptr(q.color), 16);
						}
					}
					context->Unmap(g_quadVb, 0);
					// Runs of the same texture and blend.
					for (std::size_t i = 0; i < count;) {
						const auto& first = group.quads[start + i];
						std::size_t n = 1;
						while (i + n < count && group.quads[start + i + n].texture == first.texture && group.quads[start + i + n].blend == first.blend) {
							++n;
						}
						const auto* image = first.texture < group.asset->textureImages.size() ? group.asset->textureImages[first.texture] : nullptr;
						if (auto* srv = Texture(context, image)) {
							ObjectCB cb{};
							cb.material[0] = first.blend == 1 ? 0.878f : 0.0f;
							Write(context, g_objectCb, &cb, sizeof(cb));
							context->OMSetBlendState(g_blend[std::min<std::uint16_t>(first.blend, 7)], nullptr, 0xFFFFFFFF);
							context->PSSetShaderResources(0, 1, &srv);
							context->DrawIndexed(static_cast<UINT>(n * 6), static_cast<UINT>(i * 6), 0);
							++a_drawn;
						}
						i += n;
					}
				}
			}
		}
	}

	namespace
	{
		// Separate from g_parts/g_textures and all world constant buffers. Only the menu renderer owns these.
		struct MenuResources
		{
			ID3D11Device* device = nullptr;
			ID3D11VertexShader* vs = nullptr, *quadVs = nullptr;
			ID3D11PixelShader* ps = nullptr, *quadPs = nullptr;
			ID3D11InputLayout* layout = nullptr, *quadLayout = nullptr;
			ID3D11Buffer* frame = nullptr, *object = nullptr, *bones = nullptr, *quadFrame = nullptr, *quadObject = nullptr, *quad = nullptr;
			ID3D11RasterizerState* raster = nullptr;
			ID3D11SamplerState* sampler = nullptr;
			ID3D11BlendState* blend[8]{};
			ID3D11DepthStencilState* writeDepth = nullptr, *readDepth = nullptr, *noDepth = nullptr;
			ID3D11RenderTargetView* rtv = nullptr;
			ID3D11DepthStencilView* dsv = nullptr;
			ID3D11ShaderResourceView* srv = nullptr;
			UINT width = 0, height = 0;
			std::vector<GpuPart> parts;
			std::map<const wowcore::Image*, ID3D11ShaderResourceView*> textures;
			std::shared_ptr<const wowcore::ItemPreview> uploaded;

			void DropAsset()
			{
				for (auto& part : parts) { Release(part.vb), Release(part.ib); }
				parts.clear();
				for (auto& [image, texture] : textures) { Release(texture); }
				textures.clear();
				uploaded.reset();
			}
			void DropTarget()
			{
				Release(rtv), Release(dsv), Release(srv);
				width = height = 0;
			}
			~MenuResources()
			{
				DropAsset();
				DropTarget();
				Release(vs), Release(ps), Release(quadVs), Release(quadPs), Release(layout), Release(quadLayout);
				Release(frame), Release(object), Release(bones), Release(quadFrame), Release(quadObject), Release(quad);
				Release(raster), Release(sampler), Release(writeDepth), Release(readDepth), Release(noDepth);
				for (auto*& state : blend) { Release(state); }
			}
		};
		std::unique_ptr<MenuResources> g_menu;

		bool CreateMenu(MenuResources& m, ID3D11Device* d)
		{
			m.device = d;
			D3D11_BUFFER_DESC cb{ 128, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
			if (FAILED(d->CreateBuffer(&cb, nullptr, &m.frame))) { return false; }
			cb.ByteWidth = sizeof(ObjectCB);
			if (FAILED(d->CreateBuffer(&cb, nullptr, &m.object))) { return false; }
			cb.ByteWidth = kMaxBones * 64;
			if (FAILED(d->CreateBuffer(&cb, nullptr, &m.bones))) { return false; }
			float identity[32];
			Store(identity, glm::mat4(1.0f));
			Store(identity + 16, glm::mat4(1.0f));
			cb = { sizeof(identity), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER };
			D3D11_SUBRESOURCE_DATA init{ identity };
			if (FAILED(d->CreateBuffer(&cb, &init, &m.quadFrame))) { return false; }
			const float zero[4]{};
			cb.ByteWidth = sizeof(zero);
			init.pSysMem = zero;
			if (FAILED(d->CreateBuffer(&cb, &init, &m.quadObject))) { return false; }
			const QuadVertex quad[] = {
				{ {-1, 1, 0}, {0, 0}, {1, 1, 1, 1} }, { {1, 1, 0}, {1, 0}, {1, 1, 1, 1} },
				{ {-1, -1, 0}, {0, 1}, {1, 1, 1, 1} }, { {1, -1, 0}, {1, 1}, {1, 1, 1, 1} }
			};
			D3D11_BUFFER_DESC vb{ sizeof(quad), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
			init.pSysMem = quad;
			const D3D11_INPUT_ELEMENT_DESC qe[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 }
			};
			D3D11_RASTERIZER_DESC rd{ D3D11_FILL_SOLID, D3D11_CULL_NONE, TRUE };
			rd.DepthClipEnable = TRUE, rd.ScissorEnable = TRUE;
			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			if (FAILED(d->CreateBuffer(&vb, &init, &m.quad)) ||
				FAILED(d->CreateVertexShader(g_WowModel_VSMain, sizeof(g_WowModel_VSMain), nullptr, &m.vs)) ||
				FAILED(d->CreatePixelShader(g_WowModel_PSMain, sizeof(g_WowModel_PSMain), nullptr, &m.ps)) ||
				FAILED(d->CreateInputLayout(elems, 5, g_WowModel_VSMain, sizeof(g_WowModel_VSMain), &m.layout)) ||
				FAILED(d->CreateVertexShader(g_WowQuad_VSMain, sizeof(g_WowQuad_VSMain), nullptr, &m.quadVs)) ||
				FAILED(d->CreatePixelShader(g_WowQuad_PSMain, sizeof(g_WowQuad_PSMain), nullptr, &m.quadPs)) ||
				FAILED(d->CreateInputLayout(qe, 3, g_WowQuad_VSMain, sizeof(g_WowQuad_VSMain), &m.quadLayout)) ||
				FAILED(d->CreateRasterizerState(&rd, &m.raster)) || FAILED(d->CreateSamplerState(&sd, &m.sampler))) { return false; }
			// The M2 blend modes over premultiplied colour with coverage in alpha: 0-1 opaque, 2 alpha; the glows (3 add,
			// 4 add by alpha, 7 blend add) and the modulates (5, 6) change colour but not coverage.
			for (int i = 0; i < 8; ++i) {
				constexpr struct { D3D11_BLEND src, dst; } kColour[8] = { { D3D11_BLEND_ONE, D3D11_BLEND_ZERO }, { D3D11_BLEND_ONE, D3D11_BLEND_ZERO },
					{ D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA }, { D3D11_BLEND_ONE, D3D11_BLEND_ONE }, { D3D11_BLEND_ONE, D3D11_BLEND_ONE },
					{ D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO }, { D3D11_BLEND_DEST_COLOR, D3D11_BLEND_SRC_COLOR },
					{ D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA } };
				const bool coverage = i == 2;
				D3D11_BLEND_DESC bd{};
				bd.RenderTarget[0] = { i >= 2, kColour[i].src, kColour[i].dst, D3D11_BLEND_OP_ADD, coverage ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO,
					coverage ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL };
				if (FAILED(d->CreateBlendState(&bd, &m.blend[i]))) { return false; }
			}
			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = TRUE, dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			if (FAILED(d->CreateDepthStencilState(&dd, &m.writeDepth))) { return false; }
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			if (FAILED(d->CreateDepthStencilState(&dd, &m.readDepth))) { return false; }
			dd.DepthEnable = FALSE;
			return SUCCEEDED(d->CreateDepthStencilState(&dd, &m.noDepth));
		}

		bool MenuTarget(MenuResources& m, UINT w, UINT h)
		{
			if (m.width == w && m.height == h) { return true; }
			m.DropTarget();
			ID3D11Texture2D* color = nullptr, *depth = nullptr;
			D3D11_TEXTURE2D_DESC td{ w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
				D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE };
			bool ok = SUCCEEDED(m.device->CreateTexture2D(&td, nullptr, &color));
			td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT, td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			ok = ok && SUCCEEDED(m.device->CreateTexture2D(&td, nullptr, &depth)) &&
				SUCCEEDED(m.device->CreateRenderTargetView(color, nullptr, &m.rtv)) &&
				SUCCEEDED(m.device->CreateShaderResourceView(color, nullptr, &m.srv)) &&
				SUCCEEDED(m.device->CreateDepthStencilView(depth, nullptr, &m.dsv));
			Release(color), Release(depth);
			if (!ok) { m.DropTarget(); return false; }
			m.width = w, m.height = h;
			logger::info("item preview: private RGBA8/D24S8 target {}x{}", w, h);
			return true;
		}

		void MenuBindings(ID3D11DeviceContext* c, const ItemFrame& f, MenuResources& m)
		{
			c->GSSetShader(nullptr, nullptr, 0);
			c->HSSetShader(nullptr, nullptr, 0);
			c->DSSetShader(nullptr, nullptr, 0);
			c->SetPredication(nullptr, FALSE);
			c->RSSetState(m.raster);
			const D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(f.width), static_cast<float>(f.height), 0, 1 };
			const D3D11_RECT sc{ f.left, f.top, f.right, f.bottom };
			c->RSSetViewports(1, &vp);
			c->RSSetScissorRects(1, &sc);
			c->PSSetSamplers(0, 1, &m.sampler);
		}
	}

	bool DrawItem(const ItemFrame& f, const std::shared_ptr<const wowcore::ItemPreview>& item, std::uint32_t& batches)
	{
		batches = 0;
		if (!item || item->status != wowcore::ItemPreviewStatus::Ready || item->parts.empty() || !f.device || !f.context) { return false; }
		if (!g_menu || g_menu->device != f.device) {
			auto menu = std::make_unique<MenuResources>();
			if (!CreateMenu(*menu, f.device)) { return false; }
			g_menu = std::move(menu);
		}
		auto& m = *g_menu;
		auto* c = f.context;
		if (!MenuTarget(m, f.width, f.height)) { return false; }
		const auto count = item->armorOnly ? item->parts.size() : 1;  // standalone shoulders: first side
		if (m.uploaded != item) {
			m.DropAsset();
			m.parts.resize(count);
			std::size_t uploadedBatches = 0;
			for (std::size_t i = 0; i < count; ++i) {
				const auto& part = item->parts[i];
				std::vector<const wowcore::Image*> images;
				for (const auto& path : part.batchTextures) {
					const auto it = part.images.find(path);
					images.push_back(it == part.images.end() ? nullptr : &it->second);
				}
				auto& gpu = m.parts[i];
				if (part.model.bones.size() > kMaxBones || !Upload(c, part.model, images, gpu, f.device, m.textures) || gpu.batches.empty() ||
					std::ranges::any_of(gpu.batches, [](const auto& b) { return !b.srv || b.blend > 7; })) {
					m.DropAsset(); return false;
				}
				uploadedBatches += gpu.batches.size();
			}
			m.uploaded = item;
			logger::info("item preview: uploaded entry={} armorOnly={} parts={} batches={} textures={}", item->entry, item->armorOnly,
				count, uploadedBatches, m.textures.size());
		}
		float frame[32];
		std::memcpy(frame, f.viewProj, 64);
		std::memcpy(frame + 16, f.viewProj, 64);
		if (!Write(c, m.frame, frame, sizeof(frame))) { return false; }
		ID3D11ShaderResourceView* none[2]{};
		c->PSSetShaderResources(0, 2, none);
		const float clear[4]{};
		c->ClearRenderTargetView(m.rtv, clear);
		c->ClearDepthStencilView(m.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
		c->OMSetRenderTargets(1, &m.rtv, m.dsv);
		MenuBindings(c, f, m);
		c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		c->IASetInputLayout(m.layout);
		c->VSSetShader(m.vs, nullptr, 0);
		c->PSSetShader(m.ps, nullptr, 0);
		ID3D11Buffer* cbs[] = { m.frame, m.object, m.bones };
		c->VSSetConstantBuffers(0, 3, cbs);
		c->PSSetConstantBuffers(0, 3, cbs);
		ObjectCB cb{};
		std::memcpy(cb.lightDir, f.lightDir, 12);
		std::memcpy(cb.lightColor, glm::value_ptr(glm::vec4(0.45f)), 16);
		std::memcpy(cb.ambientColor, glm::value_ptr(glm::vec4(0.55f)), 16);
		const glm::mat4 world = glm::transpose(glm::make_mat4(f.world));
		const DrawResources resources{ m.object, m.bones, m.readDepth, m.blend, true };
		std::vector<std::vector<glm::mat4>> bones(count);
		std::vector<int> sequences(count);
		for (std::size_t i = 0; i < count; ++i) {
			sequences[i] = std::max(wowcore::findSequence(item->parts[i].model, 0), 0);
			wowcore::poseBones(item->parts[i].model, sequences[i], 0, 0, bones[i]);
			if (bones[i].size() > kMaxBones) { return false; }
		}
		for (int pass = 0; pass < 2; ++pass) {
			for (std::size_t i = 0; i < count; ++i) {
				const auto& part = item->parts[i];
				const auto placed = world * part.modelToPreview;
				if (!DrawPart(c, m.parts[i], bones[i], cb, placed, placed, nullptr, pass, m.writeDepth, batches, &part.model, 0, sequences[i], &resources)) { return false; }
			}
		}
		c->PSSetShaderResources(0, 2, none);
		return batches > 0 && SUCCEEDED(f.device->GetDeviceRemovedReason());
	}

	bool CompositeItem(const ItemFrame& f, ID3D11RenderTargetView* destination)
	{
		if (!g_menu || !g_menu->srv || !destination || FAILED(f.device->GetDeviceRemovedReason())) { return false; }
		auto& m = *g_menu;
		auto* c = f.context;
		c->OMSetRenderTargets(1, &destination, nullptr);
		MenuBindings(c, f, m);
		c->IASetInputLayout(m.quadLayout);
		c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		const UINT stride = sizeof(QuadVertex), offset = 0;
		c->IASetVertexBuffers(0, 1, &m.quad, &stride, &offset);
		c->VSSetShader(m.quadVs, nullptr, 0);
		c->PSSetShader(m.quadPs, nullptr, 0);
		c->VSSetConstantBuffers(0, 1, &m.quadFrame);
		c->PSSetConstantBuffers(1, 1, &m.quadObject);
		c->PSSetShaderResources(0, 1, &m.srv);
		c->OMSetBlendState(m.blend[2], nullptr, 0xFFFFFFFF);
		c->OMSetDepthStencilState(m.noDepth, 0);
		c->Draw(4, 0);
		ID3D11ShaderResourceView* none = nullptr;
		c->PSSetShaderResources(0, 1, &none);
		return SUCCEEDED(f.device->GetDeviceRemovedReason());
	}

	void Draw(const Frame& a_frame)
	{
		const auto* data = Avatar::Data();
		const auto  pose = Avatar::Current();
		if (g_failed) {
			g_havePrev = false;
			GroundItems::RecordDraws({});
			return;
		}
		auto* context = a_frame.context;
		if (!g_device) {
			g_device = a_frame.device;
			if (!CreateStates(g_device)) {
				logger::error("wow models: D3D11 resources failed");
				g_failed = true;
				return;
			}
		}
		EnsureDepthRead(a_frame.reversedZ);
		if (data) { Expose(a_frame, pose.position); }
		DrawGround(a_frame);  // independent of her visibility, shapeshift, menu previews and portrait redraws
		if (!data) { g_havePrev = false; return; }
		if (g_uploaded != data) {
			// Dressed again: her buffers go, and every texture with them (they are found by image address, and the old
			// images' memory may hold the new ones), and so the effect models' batches that point at them.
			for (auto& part : g_parts) {
				Release(part.vb), Release(part.ib);
			}
			for (auto& [asset, part] : g_effects) {
				Release(part.vb), Release(part.ib);
			}
			for (auto& [image, srv] : g_textures) {
				Release(srv);
			}
			if (g_uploaded) {   // a new look (a form, other gear): her portrait is drawn again from it
				auto* portrait = g_portrait.exchange(nullptr);
				Release(portrait);
			}
			Release(g_mountGpu.vb), Release(g_mountGpu.ib);
			g_mountGpu = {}; g_mountUploaded = nullptr;
			g_effects.clear();
			g_textures.clear();
			g_parts.clear();
			if (!Upload(context, data->body.model, data->body.batchImages, g_parts.emplace_back())) {
				g_failed = true;
				return;
			}
			g_parts[0].part = &data->body;
			for (const auto& part : data->attached) {
				GpuPart gpu;
				if (Upload(context, part.model, part.batchImages, gpu)) {
					gpu.part = &part;
					g_parts.push_back(std::move(gpu));
				}
			}
			g_uploaded = data;
			logger::info("wow models: avatar uploaded ({} parts, {} textures)", g_parts.size(), g_textures.size());
		}
		EnsureDepthRead(a_frame.reversedZ);
		if (!g_portrait) {
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->IASetInputLayout(g_layout);
			context->VSSetShader(g_vs, nullptr, 0);
			context->PSSetShader(g_ps, nullptr, 0);
			ID3D11Buffer* pcbs[3] = { a_frame.frameCb, g_objectCb, g_bonesCb };
			context->VSSetConstantBuffers(0, 3, pcbs);
			context->PSSetConstantBuffers(0, 3, pcbs);
			context->PSSetSamplers(0, 1, &g_sampler);
			context->RSSetState(g_raster);
			ID3D11RenderTargetView* target = nullptr;
			ID3D11DepthStencilView* targetDepth = nullptr;
			D3D11_VIEWPORT          viewport{};
			UINT                    viewports = 1;
			context->OMGetRenderTargets(1, &target, &targetDepth);
			context->RSGetViewports(&viewports, &viewport);
			DrawPortrait(a_frame, *data);
			context->OMSetRenderTargets(1, &target, targetDepth);
			context->RSSetViewports(1, &viewport);
			Release(target), Release(targetDepth);
			return;   // the frame's view was overwritten; she is drawn from the next frame
		}

		// Her pose: the body's bones for the animation, then each attached model where it hangs.
		const auto& body = data->body.model;
		const int   seq = SequenceFor(body, pose.animation);
		static const auto start = std::chrono::steady_clock::now();
		const float globalMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
		std::vector<glm::mat4> bones;
		// WoW eases her from one animation into the next over the entered one's blend time (wowcore::blendDurationMs);
		// the one she left keeps its clock meanwhile.
		static int   lastSeq = -1, fromSeq = -1;
		static float lastTime = 0, fromTime = 0, switchAt = 0, blendMs = 0;
		static bool  lastLoop = true, fromLoop = true;
		static const Avatar::Loaded* blendLook = nullptr;
		if (blendLook != data) {   // another look (a shift): its sequences are not the last one's, nothing to ease from
			blendLook = data;
			lastSeq = fromSeq = -1;
		}
		const float  timeMs = TimeIn(body, seq, pose.timeMs, pose.loop);
		if (seq != lastSeq) {
			if (lastSeq >= 0) {
				fromSeq = lastSeq, fromTime = lastTime, fromLoop = lastLoop, switchAt = globalMs;
				blendMs = static_cast<float>(wowcore::blendDurationMs(body, lastSeq, seq));
			}
			lastSeq = seq;
		}
		if (const float since = globalMs - switchAt; fromSeq >= 0 && blendMs > 0.0f && since < blendMs) {
			wowcore::poseBonesBlended(body, fromSeq, TimeIn(body, fromSeq, fromTime + since, fromLoop), seq, timeMs, since / blendMs, globalMs, bones);
		} else {
			wowcore::poseBones(body, seq, timeMs, globalMs, bones);
		}
		if (pose.upperAnimation >= 0) {
			wowcore::layerUpperBody(body, wowcore::findSequence(body, static_cast<std::uint32_t>(pose.upperAnimation)),
				pose.actionTimeMs, globalMs, bones);
		}
		lastTime = timeMs, lastLoop = pose.loop;

		const auto* mount = Avatar::MountData();
        if (!pose.mountDisplay || !mount || mount->display != pose.mountDisplay) mount = nullptr;
        std::vector<glm::mat4> mountBones;
        glm::mat4 saddle(1.0f), mountWorld(1.0f);
        const auto placement = Placement(pose.position, pose.facing, a_frame.cam);
        if (mount) {
            if (g_mountUploaded != mount) {
                Release(g_mountGpu.vb), Release(g_mountGpu.ib); g_mountGpu = {};
                if (Upload(context,mount->body.model,mount->body.batchImages,g_mountGpu)) g_mountUploaded=mount;
                else { g_mountUploaded=nullptr; mount=nullptr; }
            }
            if (mount) {
                const auto& model=mount->body.model;
                const int mseq=SequenceFor(model,pose.mountAnimation);
                wowcore::poseBones(model,mseq,TimeIn(model,mseq,pose.mountTimeMs,pose.loop),globalMs,mountBones);
                const auto at=wowcore::mountedRiderMatrix(model,mountBones,mount->look.scale);
                if (at) saddle=*at; else mount=nullptr;
                if (mount) mountWorld=glm::scale(placement,glm::vec3(mount->look.scale));
            }
        }
        const glm::mat4 world = glm::scale(placement * saddle, glm::vec3(data->look.scale));   // a form's size
		const glm::mat4 prevWorld = g_havePrev ? glm::scale(Placement(g_prevPosition, g_prevFacing, a_frame.cam) * saddle,glm::vec3(data->look.scale)) : world;

		DrawDecal(a_frame, pose.circleAt, pose.circleRadius, pose.circleColor, data->selection, false);
		DrawDecal(a_frame, pose.areaAt, pose.areaRadius, {}, pose.areaOk ? data->areaOk : data->areaBad, true);
		for (std::size_t slot = 0; slot < g_unitFaces.size(); ++slot) {
			const auto head = Portrait::Current(slot);
			if (!head || head->serial == g_unitFaces[slot].drawn) continue;
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->IASetInputLayout(g_layout);
			context->VSSetShader(g_vs, nullptr, 0);
			context->PSSetShader(g_ps, nullptr, 0);
			ID3D11Buffer* tcbs[3] = { a_frame.frameCb, g_objectCb, g_bonesCb };
			context->VSSetConstantBuffers(0, 3, tcbs);
			context->PSSetConstantBuffers(0, 3, tcbs);
			context->PSSetSamplers(0, 1, &g_sampler);
			context->RSSetState(g_raster);
			DrawTargetPortrait(a_frame, *head, g_unitFaces[slot]);
			return;   // the frame's view was overwritten; she is drawn from the next frame
		}
		ObjectCB cb{};
		Light(cb);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->IASetInputLayout(g_layout);
		context->VSSetShader(g_vs, nullptr, 0);
		context->PSSetShader(g_ps, nullptr, 0);
		ID3D11Buffer* cbs[3] = { a_frame.frameCb, g_objectCb, g_bonesCb };
		context->VSSetConstantBuffers(0, 3, cbs);
		context->PSSetConstantBuffers(0, 3, cbs);
		context->PSSetSamplers(0, 1, &g_sampler);
		context->RSSetState(g_raster);

		std::uint32_t drawn = 0, mountBatches = 0;
		cb.material[2] = 1.0f - pose.alpha;   // her fade (the shader's), not her spells' effects'
		for (int pass = 0; pass < 2 && pose.visible; ++pass) {   // opaque and alpha-keyed, then the blended
			if (mount) { const auto before=drawn; DrawPart(context,g_mountGpu,mountBones,cb,mountWorld,mountWorld,&mount->look.geosets,pass,a_frame.depthWrite,drawn); mountBatches+=drawn-before; }
            DrawPart(context, g_parts[0], bones, cb, world, prevWorld, &data->look.geosets, pass, a_frame.depthWrite, drawn);
			for (std::size_t i = 1; i < g_parts.size(); ++i) {
				const auto& part = *g_parts[i].part;
				const auto  place = wowcore::placeAttached(body, bones, *part.attached, pose.weaponsDrawn);
				if (!place) {
					continue;
				}
				std::vector<glm::mat4> itemBones;
				const int itemSeq = SequenceFor(part.model, 0);
				wowcore::poseBones(part.model, itemSeq, TimeIn(part.model, itemSeq, globalMs, true), globalMs, itemBones);
				DrawPart(context, g_parts[i], itemBones, cb, world * *place, prevWorld * *place, nullptr, pass, a_frame.depthWrite, drawn);
			}
		}
		cb.material[2] = 0.0f;
		// Her spells' effects: stepped on (in absolute world), their models drawn as they look now, then their particles
		// and ribbons.
		static auto last = std::chrono::steady_clock::now();
		const auto  now = std::chrono::steady_clock::now();
		const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
		last = now;
		const auto& rot = RE::Main::WorldRootCamera()->world.rotate;
		const glm::vec3 right(rot.entry[0][2], rot.entry[1][2], rot.entry[2][2]), up(rot.entry[0][1], rot.entry[1][1], rot.entry[2][1]);
		SpellVisuals::Her her{ &body, &bones, glm::scale(Placement(pose.position, pose.facing, RE::NiPoint3{}) * saddle,glm::vec3(data->look.scale)), pose.visible };
		std::vector<SpellVisuals::DrawModel> effectModels;
		std::vector<SpellVisuals::DrawQuads> effectQuads;
		SpellVisuals::Step(dt, her, glm::vec3(a_frame.cam.x, a_frame.cam.y, a_frame.cam.z), right, up, effectModels, effectQuads);
		const glm::mat4 toCamera = glm::translate(glm::mat4(1.0f), glm::vec3(-a_frame.cam.x, -a_frame.cam.y, -a_frame.cam.z));
		for (int pass = 0; pass < 2; ++pass) {
			for (const auto& e : effectModels) {
				auto it = g_effects.find(e.asset);
				if (it == g_effects.end()) {
					it = g_effects.emplace(e.asset, GpuPart{}).first;
					Upload(context, e.asset->model, e.asset->batchImages, it->second);
				}
				if (it->second.vb) {
					const glm::mat4 w = toCamera * e.world;
					DrawPart(context, it->second, e.bones, cb, w, w, nullptr, pass, a_frame.depthWrite, drawn, &e.asset->model, e.timeMs, e.sequence);
				}
			}
		}
		DrawQuads(a_frame, effectQuads, drawn);
		g_prevPosition = pose.position;
		g_prevFacing = pose.facing;
		g_havePrev = true;
		std::lock_guard lock(g_statsLock);
		++g_stats.frames;
		g_stats.batches = drawn;
		g_stats.mountDisplay = mount && mountBatches ? mount->display : 0;
		g_stats.mountBatches = mountBatches;
	}

	ID3D11ShaderResourceView* Portrait() { return g_portrait; }

	ID3D11ShaderResourceView* UnitPortrait(std::size_t a_slot, RE::FormID a_actor)
	{
		if (a_slot >= g_unitFaces.size()) return nullptr;
		const auto head = Portrait::Current(a_slot);
		const auto& face = g_unitFaces[a_slot];
		return head && (!a_actor || head->actor == a_actor) && head->serial == face.drawn ? face.srv : nullptr;
	}

	ID3D11ShaderResourceView* TargetPortrait() { return UnitPortrait(0); }

	Stats GetStats()
	{
		std::lock_guard lock(g_statsLock);
		return g_stats;
	}

	bool GroundReady(const std::shared_ptr<const wowcore::ItemPreview>& a_item)
	{
		std::lock_guard lock(g_groundLock);
		const auto it = g_groundReady.find(a_item);
		return it != g_groundReady.end() && std::chrono::steady_clock::now() - it->second < std::chrono::seconds(1);
	}
}
