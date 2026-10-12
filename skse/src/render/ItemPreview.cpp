#include "render/ItemPreview.h"
#include "Integration.h"

#include "Character.h"
#include "Config.h"
#include "Gear.h"
#include "SelfTest.h"
#include "render/RenderState.h"
#include "render/WowModels.h"
#include "wowcore/item_preview.hpp"
#include "wowcore/effect.hpp"
#include "wowcore/mpq.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace ItemPreview
{
	namespace
	{
		std::mutex g_lock;
		std::mutex g_loading;   // archive/mesh work stays on one background worker at a time
		wowcore::Appearance g_appearance;
		std::uint64_t g_appearanceGeneration = 0;
		bool g_started = false;
		std::map<std::uint32_t, std::shared_ptr<const Asset>> g_assets;
		using DisplayKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;
		std::map<DisplayKey, std::shared_ptr<const Asset>> g_displays;
		RE::TESBoundObject* g_selected = nullptr;
		std::uint32_t g_entry = 0;
		std::uint64_t g_generation = 0;
		Info g_info;
		bool g_routeFailed = false;  // one unsupported native route disables replacement until the next menu session
		thread_local RE::IMenu* g_owner = nullptr;
		thread_local const char* g_menuName = "";
		std::uint64_t g_baselineGeneration = 0;
		RE::NiAVObject* g_baselineNode = nullptr;
		glm::mat3 g_initialRotation(1.0f);

		glm::mat3 Matrix(const RE::NiMatrix3& r)
		{
			glm::mat3 m;
			for (int row = 0; row < 3; ++row) {
				for (int col = 0; col < 3; ++col) { m[col][row] = r.entry[row][col]; }
			}
			return m;
		}
		glm::vec3 Point(const RE::NiPoint3& p) { return { p.x, p.y, p.z }; }
		void Store(float* out, const glm::mat4& m) { std::memcpy(out, glm::value_ptr(glm::transpose(m)), 64); }
		bool Finite(const glm::mat4& m)
		{
			for (int col = 0; col < 4; ++col) {
				for (int row = 0; row < 4; ++row) { if (!std::isfinite(m[col][row])) { return false; } }
			}
			return true;
		}

		void Select(RE::TESBoundObject* form, const char* why)
		{
			std::lock_guard lock(g_lock);
			if (form != g_selected || !form) {
				g_selected = form;
				g_entry = form ? Gear::EntryOf(form) : 0;
				++g_generation;
				g_info.wow = false;
				g_info.armorOnly = false;
				g_info.entry = g_entry;
				g_info.form = form ? form->GetFormID() : 0;
				g_info.generation = g_generation;
				g_info.reason = why;
				g_routeFailed = false;
			}
			logger::info("item preview: {} form={:08X} entry={} generation={}", why, form ? form->GetFormID() : 0, g_entry, g_generation);
		}

		// Stand at t=0; measure vertices referenced by visible batches, using the same pose as DrawItem.
		bool Bounds(Asset& asset)
		{
			if (asset.item->parts.empty()) { return false; }
			std::vector<glm::vec3> vertices;
			const auto count = asset.item->armorOnly ? asset.item->parts.size() : 1;
			for (std::size_t partIndex = 0; partIndex < count; ++partIndex) {
				const auto& part = asset.item->parts[partIndex];
				const auto& m = part.model;
				if (m.bones.size() > 256) { return false; }
				std::vector<glm::mat4> bones;
				const int sequence = std::max(wowcore::findSequence(m, 0), 0);
				wowcore::poseBones(m, sequence, 0, 0, bones);
				for (const auto& b : m.batches) {
					if (b.materialIndex >= m.materials.size() || m.materials[b.materialIndex].blendMode > 7 ||
						b.indexStart > m.indices.size() || b.indexCount > m.indices.size() - b.indexStart) { return false; }
					if (wowcore::batchLook(m, b, sequence, 0, 0).color.a <= 0.003f) { continue; }
					for (std::size_t i = b.indexStart; i < b.indexStart + b.indexCount; ++i) {
						if (m.indices[i] >= m.vertices.size()) { return false; }
						const auto& v = m.vertices[m.indices[i]];
						glm::vec4 p(0.0f);
						float total = 0;
						for (int k = 0; k < 4; ++k) {
							if (v.boneWeights[k]) {
								if (v.boneIndices[k] >= bones.size()) { return false; }
								const float w = v.boneWeights[k] / 255.0f;
								p += w * bones[v.boneIndices[k]] * glm::vec4(v.position, 1.0f);
								total += w;
							}
						}
						const glm::vec3 at = glm::vec3(part.modelToPreview * (total > 0 ? p : glm::vec4(v.position, 1.0f)));
						if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) { return false; }
						vertices.push_back(at);
					}
				}
			}
			if (vertices.empty()) { return false; }
			glm::vec3 lo = vertices.front(), hi = lo;
			for (const auto& v : vertices) { lo = glm::min(lo, v); hi = glm::max(hi, v); }
			asset.centre = (lo + hi) * 0.5f;
			const glm::vec3 size = hi - lo;
			asset.size = size;
			asset.longAxis = size.x >= size.y && size.x >= size.z ? glm::vec3(1, 0, 0) : size.y >= size.z ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
			for (const auto& v : vertices) { asset.radius = std::max(asset.radius, glm::length(v - asset.centre)); }
			return std::isfinite(asset.radius) && asset.radius > 1e-5f;
		}

		struct LoadHook
		{
			static void thunk(RE::Inventory3DManager* manager, RE::TESBoundObject* form, RE::ExtraDataList* extra)
			{
				func(manager, form, extra);  // internal unload calls may invalidate the previous selection
				Select(form, "load");
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct UnloadHook
		{
			static void thunk(RE::Inventory3DManager* manager)
			{
				Select(nullptr, "unload");
				func(manager);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct EndHook
		{
			static void thunk(RE::Inventory3DManager* manager)
			{
				Select(nullptr, "end3D");
				func(manager);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <int Index>
		struct MovieHook
		{
			static void thunk(RE::IMenu* menu)
			{
				struct OwnerScope
				{
					RE::IMenu* previous = g_owner;
					const char* name = g_menuName;
					~OwnerScope() { g_owner = previous; g_menuName = name; }
				} scope;
				g_owner = menu;
				constexpr const char* names[] = { "InventoryMenu", "BarterMenu", "ContainerMenu" };
				g_menuName = names[Index];
				func(menu);  // Display is called once by Skyrim, followed by Inventory3DManager::Render
				if constexpr (Index == 0) { SelfTest::ItemMenuFrame(static_cast<RE::InventoryMenu*>(menu)); }
				if constexpr (Index == 1) { SelfTest::BarterMenuFrame(); }
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CloseSink : RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (e && !e->opening && (e->menuName == RE::InventoryMenu::MENU_NAME || e->menuName == RE::BarterMenu::MENU_NAME ||
					e->menuName == RE::ContainerMenu::MENU_NAME)) { Select(nullptr, "menu close"); }
				return RE::BSEventNotifyControl::kContinue;
			}
		} g_close;

		void Diagnostics(RE::Inventory3DManager* manager, const RE::LoadedInventoryModel* loaded, RE::NiCamera* camera,
			ID3D11DeviceContext* context, std::uint64_t generation)
		{
			static std::uint64_t lastGeneration = std::numeric_limits<std::uint64_t>::max();
			const auto info = GetInfo();
			if (lastGeneration == generation && info.frames % 120 != 0) { return; }
			lastGeneration = generation;
			RE::GViewport movie;
			g_owner->uiMovie->GetViewport(&movie);
			const auto visible = g_owner->uiMovie->GetVisibleFrameRect();
			logger::info("item preview: owner={} movie={:X} depth={} movieBuffer={}x{} viewport=({},{},{},{}) visible=({},{},{},{}) loadedBase={:08X} modelObj={:08X} loadTask={:X}",
				g_menuName, reinterpret_cast<std::uintptr_t>(g_owner->uiMovie.get()), static_cast<int>(g_owner->depthPriority),
				movie.bufferWidth, movie.bufferHeight, movie.left, movie.top, movie.width, movie.height,
				visible.left, visible.top, visible.right, visible.bottom,
				loaded && loaded->itemBase ? loaded->itemBase->GetFormID() : 0, loaded && loaded->modelObj ? loaded->modelObj->GetFormID() : 0,
				manager ? reinterpret_cast<std::uintptr_t>(manager->GetRuntimeData().loadTask.get()) : 0);
			if (loaded && loaded->spModel && manager) {
				const auto& node = *loaded->spModel;
				const auto& local = node.local;
				const auto& world = node.world;
				logger::info("item preview: local pos=({},{},{}) scale={} world pos=({},{},{}) scale={} bound=({},{},{};{}) cachedRadius={} culled={} itemPos=({},{},{}) itemPosCopy=({},{},{}) itemScale={}/{} zoom={}/{}",
					local.translate.x, local.translate.y, local.translate.z, local.scale, world.translate.x, world.translate.y, world.translate.z, world.scale,
					node.worldBound.center.x, node.worldBound.center.y, node.worldBound.center.z, node.worldBound.radius, loaded->boundRadius, node.GetAppCulled(),
					manager->itemPos.x, manager->itemPos.y, manager->itemPos.z, manager->itemPosCopy.x, manager->itemPosCopy.y, manager->itemPosCopy.z,
					manager->itemScale, manager->itemScaleCopy, manager->GetRuntimeData().zoomProgress, manager->GetRuntimeData().zoomDistance);
				for (int row = 0; row < 3; ++row) {
					logger::info("item preview: rotation row {} local=({},{},{}) world=({},{},{})", row,
						local.rotate.entry[row][0], local.rotate.entry[row][1], local.rotate.entry[row][2],
						world.rotate.entry[row][0], world.rotate.entry[row][1], world.rotate.entry[row][2]);
				}
			}
			if (camera) {
				const auto& frustum = camera->GetRuntimeData2().viewFrustum;
				const auto port = std::bit_cast<std::array<float, 4>>(camera->GetRuntimeData2().port);
				logger::info("item preview: UI camera={:X} pos=({},{},{}) frustum=({},{},{},{},{},{}) ortho={} port=({},{},{},{})",
					reinterpret_cast<std::uintptr_t>(camera), camera->world.translate.x, camera->world.translate.y, camera->world.translate.z,
					frustum.fLeft, frustum.fRight, frustum.fTop, frustum.fBottom, frustum.fNear, frustum.fFar, frustum.bOrtho, port[0], port[1], port[2], port[3]);
				for (int row = 0; row < 4; ++row) {
					const auto& m = camera->GetRuntimeData().worldToCam[row];
					logger::info("item preview: worldToCam row {} = ({},{},{},{})", row, m[0], m[1], m[2], m[3]);
				}
			}
			if (context) {
				ID3D11RenderTargetView* target = nullptr;
				ID3D11DepthStencilView* depth = nullptr;
				ID3D11DepthStencilState* state = nullptr;
				UINT stencil = 0;
				context->OMGetRenderTargets(1, &target, &depth);
				context->OMGetDepthStencilState(&state, &stencil);
				ID3D11Resource* resource = nullptr;
				ID3D11Texture2D* texture = nullptr;
				D3D11_TEXTURE2D_DESC td{};
				D3D11_DEPTH_STENCIL_DESC ds{};
				D3D11_RENDER_TARGET_VIEW_DESC rt{};
				if (state) { state->GetDesc(&ds); }
				if (target) {
					target->GetDesc(&rt);
					target->GetResource(&resource);
					if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))) { texture->GetDesc(&td); }
				}
				logger::info("item preview: destination RTV={:X} resource={:X} {}x{} format={} viewFormat={} samples={} DSV={:X} depthTest={} func={} stencilEnable={} ref={} masks={}/{}",
					reinterpret_cast<std::uintptr_t>(target), reinterpret_cast<std::uintptr_t>(resource), td.Width, td.Height, static_cast<int>(td.Format),
					static_cast<int>(rt.Format), td.SampleDesc.Count, reinterpret_cast<std::uintptr_t>(depth), ds.DepthEnable, static_cast<int>(ds.DepthFunc),
					ds.StencilEnable, stencil, ds.StencilReadMask, ds.StencilWriteMask);
				D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
				D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
				UINT vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, scCount = vpCount;
				context->RSGetViewports(&vpCount, vps);
				context->RSGetScissorRects(&scCount, scissors);
				for (UINT i = 0; i < vpCount; ++i) {
					logger::info("item preview: D3D viewport {} = ({},{},{},{},{},{})", i, vps[i].TopLeftX, vps[i].TopLeftY, vps[i].Width, vps[i].Height, vps[i].MinDepth, vps[i].MaxDepth);
				}
				for (UINT i = 0; i < scCount; ++i) {
					logger::info("item preview: D3D scissor {} = ({},{},{},{})", i, scissors[i].left, scissors[i].top, scissors[i].right, scissors[i].bottom);
				}
				RenderState::Release(texture), RenderState::Release(resource), RenderState::Release(target), RenderState::Release(depth), RenderState::Release(state);
			}
		}

		// Uses only the selected native sphere, movie viewport and observed UI camera. No assumed SWF object path.
		bool Prepare(RE::Inventory3DManager* manager, const RE::LoadedInventoryModel& native, RE::NiCamera* camera,
			const Asset& asset, std::uint64_t generation, ID3D11Device* device, ID3D11DeviceContext* context,
			const RenderState::StateBackup& saved, WowModels::ItemFrame& f, std::string& reason)
		{
			if (!saved.rtv[0] || saved.vpCount != 1) { reason = "missing target or multiple viewports"; return false; }
			D3D11_DEPTH_STENCIL_DESC destinationDepth{};
			if (saved.depth) { saved.depth->GetDesc(&destinationDepth); }
			if (destinationDepth.StencilEnable) { reason = "destination stencil route unsupported"; return false; }
			// Other targets bound beside the menu's (Skyrim draws the item to several): the composite writes the first only,
			// and every binding is restored after.
			ID3D11Resource* resource = nullptr;
			saved.rtv[0]->GetResource(&resource);
			ID3D11Texture2D* texture = nullptr;
			const HRESULT hr = resource ? resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)) : E_FAIL;
			RenderState::Release(resource);
			if (FAILED(hr)) { reason = "destination is not a texture2D"; return false; }
			D3D11_TEXTURE2D_DESC td{};
			texture->GetDesc(&td);
			RenderState::Release(texture);
			D3D11_RENDER_TARGET_VIEW_DESC rt{};
			saved.rtv[0]->GetDesc(&rt);
			if (!td.Width || !td.Height || td.Width > 8192 || td.Height > 8192 || td.SampleDesc.Count != 1 || td.ArraySize != 1 ||
				rt.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rt.Texture2D.MipSlice != 0 ||
				(rt.Format != DXGI_FORMAT_R8G8B8A8_UNORM && rt.Format != DXGI_FORMAT_B8G8R8A8_UNORM)) {
				reason = "unsupported target format, size or samples"; return false;
			}
			const auto& vp = saved.vps[0];
			RE::GViewport movie;
			g_owner->uiMovie->GetViewport(&movie);
			if (movie.bufferWidth != static_cast<int>(td.Width) || movie.bufferHeight != static_cast<int>(td.Height) ||
				movie.width <= 0 || movie.height <= 0 || vp.Width <= 0 || vp.Height <= 0) {   // its depth range is Skyrim's (0..0): ours is our own
				reason = "movie/target viewport mismatch"; return false;
			}
			glm::mat4 projection;
			const auto& matrix = camera->GetRuntimeData().worldToCam;
			for (int row = 0; row < 4; ++row) {
				for (int col = 0; col < 4; ++col) { projection[col][row] = matrix[row][col]; }
			}
			const auto& node = *native.spModel;
			const float radius = node.worldBound.radius;
			const glm::vec3 centre = Point(node.worldBound.center);
			const glm::vec4 clip = projection * glm::vec4(centre, 1.0f);
			if (!Finite(projection) || !std::isfinite(radius) || radius <= 0 || !std::isfinite(clip.w) || clip.w <= 1e-4f ||
				!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.z) || clip.z / clip.w < 0 || clip.z / clip.w > 1) {
				reason = "invalid native projection/bound"; return false;
			}
			// NiCamera's port is normalized; its public helper is the authority on how worldToCam maps into it.
			float x = 0, y = 0, z = 0;
			if (!camera->WorldPtToScreenPt3(node.worldBound.center, x, y, z, 1e-4f) || !std::isfinite(x) || !std::isfinite(y) ||
				x < 0 || x > 1 || y < 0 || y > 1) { reason = "native centre outside camera port"; return false; }
			const auto port = std::bit_cast<std::array<float, 4>>(camera->GetRuntimeData2().port);
			if (port[0] < 0 || port[1] > 1 || port[3] < 0 || port[2] > 1 || port[1] <= port[0] || port[2] <= port[3]) {
				reason = "invalid camera port"; return false;
			}
			const float nx = clip.x / clip.w, ny = clip.y / clip.w;
			const float expectedX = port[0] + (nx + 1) * 0.5f * (port[1] - port[0]);
			const float expectedY = port[3] + (ny + 1) * 0.5f * (port[2] - port[3]);
			if (std::abs(x - expectedX) > 0.002f || std::abs(y - expectedY) > 0.002f) {
				reason = "worldToCam convention mismatch"; return false;
			}
			// Map the camera port through the observed D3D viewport, then into a full-size private target.
			glm::mat4 screen(1.0f);
			screen[0][0] = vp.Width / td.Width * (port[1] - port[0]);
			screen[1][1] = vp.Height / td.Height * (port[2] - port[3]);
			screen[3][0] = (2 * vp.TopLeftX + vp.Width * (port[0] + port[1])) / td.Width - 1;
			screen[3][1] = 1 - (2 * vp.TopLeftY + vp.Height * (2 - port[2] - port[3])) / td.Height;
			projection = screen * projection;
			auto project = [&](const glm::vec3& p, glm::vec2& out) {
				const auto c = projection * glm::vec4(p, 1.0f);
				if (!std::isfinite(c.w) || c.w <= 1e-4f) { return false; }
				out = { (c.x / c.w * 0.5f + 0.5f) * td.Width, (0.5f - c.y / c.w * 0.5f) * td.Height };
				return std::isfinite(out.x) && std::isfinite(out.y);
			};
			glm::vec2 pixel, lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
			if (!project(centre, pixel)) { reason = "centre projection failed"; return false; }
			// A cube encloses the sphere through every rotation; project all corners for a conservative scissor.
			for (int k = 0; k < 8; ++k) {
				glm::vec2 p;
				if (!project(centre + glm::vec3(k & 1 ? radius : -radius, k & 2 ? radius : -radius, k & 4 ? radius : -radius), p)) {
					reason = "bound crosses camera"; return false;
				}
				lo = glm::min(lo, p), hi = glm::max(hi, p);
			}
			f.device = device, f.context = context, f.width = td.Width, f.height = td.Height;
			f.left = static_cast<long>(std::max(lo.x, static_cast<float>(std::max(movie.left, 0))));
			f.top = static_cast<long>(std::max(lo.y, static_cast<float>(std::max(movie.top, 0))));
			f.right = static_cast<long>(std::min(hi.x + 1, static_cast<float>(std::min(movie.left + movie.width, movie.bufferWidth))));
			f.bottom = static_cast<long>(std::min(hi.y + 1, static_cast<float>(std::min(movie.top + movie.height, movie.bufferHeight))));
			if (movie.flags.any(RE::GViewport::Flag::kUseScissorRect)) {
				f.left = std::max(f.left, static_cast<long>(movie.scissorLeft));
				f.top = std::max(f.top, static_cast<long>(movie.scissorTop));
				f.right = std::min(f.right, static_cast<long>(movie.scissorLeft + movie.scissorWidth));
				f.bottom = std::min(f.bottom, static_cast<long>(movie.scissorTop + movie.scissorHeight));
			}
			D3D11_RASTERIZER_DESC rd{};
			if (saved.raster) { saved.raster->GetDesc(&rd); }
			if (rd.ScissorEnable) {
				if (saved.scissorCount != 1) { reason = "multiple scissors"; return false; }
				const auto& sc = saved.scissors[0];
				f.left = std::max(f.left, sc.left), f.top = std::max(f.top, sc.top);
				f.right = std::min(f.right, sc.right), f.bottom = std::min(f.bottom, sc.bottom);
			}
			if (f.right <= f.left || f.bottom <= f.top) { reason = "empty preview scissor"; return false; }
			const glm::mat3 rotation = Matrix(node.world.rotate);
			if (g_baselineGeneration != generation || g_baselineNode != native.spModel.get()) {
				g_baselineGeneration = generation, g_baselineNode = native.spModel.get(), g_initialRotation = rotation;
			}
			if (!std::isfinite(glm::determinant(rotation)) || std::abs(glm::determinant(rotation)) < 0.01f ||
				std::abs(glm::determinant(g_initialRotation)) < 0.01f) { reason = "invalid native rotation"; return false; }
			const glm::mat4 delta(rotation * glm::inverse(g_initialRotation));
			// A weapon lies across the view as Skyrim shows one: its length along the screen, tilted 30 degrees up.
			const bool weapon = !asset.item->armorOnly && asset.inventoryType != 1 && asset.inventoryType != 3 && asset.inventoryType != 14;
			glm::mat4  initial(1.0f);
			if (asset.item->armorOnly) {
				// M2 characters face +X; Model_OnLoad in 1.12 UIParent.lua starts DressUpModel at 0.61 radians.
				const glm::vec3 toEye = Point(camera->world.translate) - centre;
				initial = glm::rotate(initial, std::atan2(toEye.y, toEye.x) + 0.61f, glm::vec3(0, 0, 1));
			} else if (weapon) {
				const glm::vec3 forward = -glm::normalize(Point(camera->world.translate) - centre);
				glm::vec3       right = glm::cross(forward, glm::vec3(0, 0, 1));
				right = glm::length(right) > 1e-3f ? glm::normalize(right) : Matrix(camera->world.rotate)[2];
				const glm::vec3 up = glm::cross(right, forward);
				const glm::vec3 along = glm::normalize(right * 0.866f + up * 0.5f);
				const glm::vec3 axis = glm::cross(asset.longAxis, along);
				const float     turn = std::acos(std::clamp(glm::dot(asset.longAxis, along), -1.0f, 1.0f));
				if (glm::length(axis) > 1e-4f) {
					initial = glm::rotate(glm::mat4(1.0f), turn, glm::normalize(axis));
				} else if (turn > 1.0f) {
					initial = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), up);
				}
			}
			const float scale = radius * 0.95f / asset.radius;  // native sphere already includes zoom: apply it once
			const glm::mat4 world = glm::translate(glm::mat4(1.0f), centre) * delta * initial *
				glm::scale(glm::mat4(1.0f), glm::vec3(scale)) * glm::translate(glm::mat4(1.0f), -asset.centre);
			if (!Finite(world)) { reason = "invalid item transform"; return false; }
			const glm::vec3 toEye = Point(camera->world.translate) - centre;
			if (!std::isfinite(glm::length(toEye)) || glm::length(toEye) < 1e-4f) { reason = "invalid menu light basis"; return false; }
			const glm::mat3 cameraRotation = Matrix(camera->world.rotate);
			const glm::vec3 light = glm::normalize(glm::normalize(toEye) + cameraRotation[1] * 0.5f + cameraRotation[2] * 0.3f);
			if (!std::isfinite(light.x) || !std::isfinite(light.y) || !std::isfinite(light.z)) { reason = "invalid menu light direction"; return false; }
			std::memcpy(f.lightDir, glm::value_ptr(light), 12);
			Store(f.world, world), Store(f.viewProj, projection);
			return true;
		}

		void Record(bool wow, std::uint64_t generation, std::uint32_t batches, std::string_view reason,
			RE::Inventory3DManager* manager, RE::NiAVObject* node, bool armorOnly = false)
		{
			std::lock_guard lock(g_lock);
			++g_info.frames;
			if (wow) { ++g_info.wowFrames; } else { ++g_info.nativeFrames; }
			g_info.armorOnly = wow && armorOnly;
			g_info.wow = wow, g_info.batches = batches, g_info.menu = g_menuName;
			g_info.zoom = manager ? manager->GetRuntimeData().zoomProgress : 0;
			if (node) { std::memcpy(g_info.rotation, node->world.rotate.entry, sizeof(g_info.rotation)); }
			if (g_info.reason != reason || g_info.frames % 120 == 1) {
				logger::info("item preview: drawn={} menu={} movie={:X} depth={} form={:08X} entry={} generation={} batches={} zoom={} reason={}",
					wow ? "WoW" : "Skyrim", g_menuName, g_owner ? reinterpret_cast<std::uintptr_t>(g_owner->uiMovie.get()) : 0,
					g_owner ? static_cast<int>(g_owner->depthPriority) : -1, g_info.form, g_info.entry, generation, batches, g_info.zoom, reason);
			}
			g_info.reason = reason;
		}

		struct DrawHook
		{
			// Audited AE 52727: RCX scene, EDX scheme, R8 optional NiCamera, R9b flag; no incoming stack arguments.
			static void thunk(RE::UI3DSceneManager* scene, RE::INTERFACE_LIGHT_SCHEME scheme, RE::NiCamera* optionalCamera, bool flag)
			{
				auto nativeDraw = [&] { func(scene, scheme, optionalCamera, flag); };
				if (!Integration::Active() || !g_owner || !g_owner->uiMovie || !Config::Get().itemPreview) { nativeDraw(); return; }
				auto* manager = RE::Inventory3DManager::GetSingleton();
				std::uint64_t generation;
				RE::TESBoundObject* selected;
				std::shared_ptr<const Asset> asset;
				std::string reason;
				{
					std::lock_guard lock(g_lock);
					generation = g_generation, selected = g_selected;
					if (g_routeFailed) { reason = "native target route disabled for session"; }
					else if (const auto it = g_assets.find(g_entry); it != g_assets.end()) { asset = it->second; }
					else { reason = g_entry ? "asset pending or unsupported" : "ordinary Skyrim item"; }
				}
				RE::NiPointer<RE::NiAVObject> node;
				const RE::LoadedInventoryModel* loaded = nullptr;
				if (manager && !manager->GetRuntimeData().loadTask && !manager->GetRuntimeData().loadedModels.empty()) {
					loaded = &manager->GetRuntimeData().loadedModels.back();
					if (loaded->itemBase == selected && loaded->lightScheme == scheme) { node = loaded->spModel; }
				}
				auto* camera = optionalCamera ? optionalCamera : scene ? scene->camera.get() : nullptr;
				auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
				auto* device = renderer ? reinterpret_cast<ID3D11Device*>(renderer->GetRuntimeData().forwarder) : nullptr;
				auto* context = renderer ? reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context) : nullptr;
				Diagnostics(manager, loaded, camera, context, generation);
				if (!asset || !reason.empty() || !node || node->GetAppCulled() || !camera || !device || !context) {
					if (reason.empty()) { reason = "native model pending, hidden, mismatched or no camera/device"; }
					nativeDraw(); Record(false, generation, 0, reason, manager, node.get()); return;
				}
				RenderState::StateBackup entry;
				entry.Save(context);
				WowModels::ItemFrame frame;
				std::uint32_t batches = 0;
				bool ready = false;
				try {
					ready = Prepare(manager, *loaded, camera, *asset, generation, device, context, entry, frame, reason);
					if (ready) { ready = WowModels::DrawItem(frame, asset->item, batches); }
					if (!ready && reason.empty()) { reason = "GPU upload or draw failed"; }
				} catch (const std::exception& e) { reason = std::format("draw exception: {}", e.what()); }
				// Retain the destination separately: Restore releases StateBackup's COM references.
				auto* destination = entry.rtv[0];
				if (destination) { destination->AddRef(); }
				entry.Restore(context);
				{
					std::lock_guard lock(g_lock);
					if (generation != g_generation || selected != g_selected) { ready = false; reason = "selection changed"; }
				}
				if (!ready) {
					RenderState::Release(destination);
					nativeDraw(); Record(false, generation, 0, reason, manager, node.get()); return;
				}

				{
					struct CullScope
					{
						RE::NiPointer<RE::NiAVObject> node;
						bool original;
						~CullScope() { node->SetAppCulled(original); }
					} cull{ node, node->GetAppCulled() };
					node->SetAppCulled(true);
					nativeDraw();
				}  // visible again before the engine's next rotation/input update
				RenderState::StateBackup after;
				after.Save(context);
				// The initial implementation requires native UI3D to return to this movie's exact RTV.
				bool composited = false;
				if (after.rtv[0] == destination) {
					composited = WowModels::CompositeItem(frame, destination);
				} else {
					logger::warn("item preview: native destination changed from {:X} to {:X}",
						reinterpret_cast<std::uintptr_t>(destination), reinterpret_cast<std::uintptr_t>(after.rtv[0]));
				}
				after.Restore(context);
				RenderState::Release(destination);
				if (!composited) {
					logger::warn("item preview: native/composite route failed; restoring visible Skyrim preview");
					{ std::lock_guard lock(g_lock); g_routeFailed = true; }
					nativeDraw();  // fail closed: redraw with the selected node visible
					Record(false, generation, 0, "composite/target route failed; native redraw", manager, node.get());
				} else { Record(true, generation, batches, "ready", manager, node.get(), asset->item->armorOnly); }
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		bool Prefix(std::uintptr_t address, std::string_view bytes, const char* name)
		{
			if (std::memcmp(reinterpret_cast<const void*>(address), bytes.data(), bytes.size()) != 0) {
				logger::warn("item preview: {} code mismatch at {:X}; Skyrim preview retained", name, address); return false;
			}
			return true;
		}
		// Each stolen prefix consists of complete, relocation-free instructions, audited with tools/addrlib.py.
		std::uintptr_t Gateway(std::uintptr_t address, std::size_t size)
		{
			auto* code = static_cast<std::uint8_t*>(SKSE::GetTrampoline().allocate(size + 14));
			std::memcpy(code, reinterpret_cast<const void*>(address), size);
			const std::uint8_t jump[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
			std::memcpy(code + size, jump, 6);
			const auto resume = address + size;
			std::memcpy(code + size + 6, &resume, 8);
			FlushInstructionCache(GetCurrentProcess(), code, size + 14);
			return reinterpret_cast<std::uintptr_t>(code);
		}
	}

	bool Install()
	{
		g_info.enabled = Config::Get().itemPreview;
		if (!g_info.enabled) { logger::info("item preview: itemPreview=0; native hooks untouched"); return true; }
		if (REL::Module::get().version() != REL::Version{ 1, 7, 104, 0 }) {
			logger::warn("item preview: unaudited runtime; Skyrim preview retained"); return false;
		}
		const auto load = REL::Relocation<std::uintptr_t>{ REL::ID(51758) }.address();
		const auto unload = REL::Relocation<std::uintptr_t>{ REL::ID(51759) }.address();
		const auto end = REL::Relocation<std::uintptr_t>{ REL::ID(51756) }.address();
		const auto site = REL::Relocation<std::uintptr_t>{ REL::ID(51755) }.address() + 0xB0;
		const auto draw = REL::Relocation<std::uintptr_t>{ REL::ID(52727) }.address();
		const auto* jump = reinterpret_cast<const std::uint8_t*>(site);
		if (jump[0] != 0xE9 || site + 5 + *reinterpret_cast<const std::int32_t*>(jump + 1) != draw) {
			logger::warn("item preview: Render tail jump mismatch; Skyrim preview retained"); return false;
		}
		const std::string_view loadBytes("\x4C\x8B\xDC\x57\x41\x54", 6);
		const std::string_view unloadBytes("\x40\x57\x48\x83\xEC\x30", 6);
		const std::string_view endBytes("\x40\x53\x55\x56\x57", 5);
		if (!Prefix(load, loadBytes, "LoadInventoryItem") || !Prefix(unload, unloadBytes, "UnloadInventoryItem") ||
			!Prefix(end, endBytes, "End3D") || !Prefix(draw, std::string_view("\x48\x8B\xC4\x44\x88\x48\x20", 7), "UI3D draw")) { return false; }
		REL::Relocation<std::uintptr_t> inventory{ RE::VTABLE_InventoryMenu[0] }, barter{ RE::VTABLE_BarterMenu[0] }, container{ RE::VTABLE_ContainerMenu[0] };
		for (const auto& [vtable, id] : { std::pair{inventory.address(), 51849}, {barter.address(), 50946}, {container.address(), 51127} }) {
			if (*reinterpret_cast<const std::uintptr_t*>(vtable + 6 * 8) != REL::Relocation<std::uintptr_t>{ REL::ID(id) }.address()) {
				logger::warn("item preview: PostDisplay vtable mismatch; Skyrim preview retained"); return false;
			}
		}
		LoadHook::func = Gateway(load, loadBytes.size());
		UnloadHook::func = Gateway(unload, unloadBytes.size());
		EndHook::func = Gateway(end, endBytes.size());
		DrawHook::func = draw;
		SKSE::GetTrampoline().write_branch<5>(load, LoadHook::thunk);
		SKSE::GetTrampoline().write_branch<5>(unload, UnloadHook::thunk);
		SKSE::GetTrampoline().write_branch<5>(end, EndHook::thunk);
		MovieHook<0>::func = inventory.write_vfunc(0x06, MovieHook<0>::thunk);
		MovieHook<1>::func = barter.write_vfunc(0x06, MovieHook<1>::thunk);
		MovieHook<2>::func = container.write_vfunc(0x06, MovieHook<2>::thunk);
		SKSE::GetTrampoline().write_branch<5>(site, DrawHook::thunk);
		g_info.installed = true;
		logger::info("item preview: verified AE load/unload/end, three PostDisplay slots and Render tail jump installed");
		return true;
	}

	namespace
	{
		void Preload(bool all)
		{
			const auto looks = Character::Looks();
			std::uint64_t appearanceGeneration;
			{
				std::lock_guard lock(g_lock);
				if (!all && looks.race == g_appearance.race && looks.sex == g_appearance.sex) { return; }
				g_appearance = looks;
				appearanceGeneration = ++g_appearanceGeneration;
				std::erase_if(g_assets, [](const auto& entry) {
					return entry.second->item->armorOnly || entry.second->inventoryType == 1;  // helmets are race/sex variants too
				});
				++g_generation;  // any in-flight draw must not hide the native node for the previous appearance
				g_info.generation = g_generation;
				g_info.wow = g_info.armorOnly = false;
			}
			wowcore::ItemDisplayCatalog catalog;
			for (const auto& [entry, item] : Character::Gear()) { catalog[entry] = { item.displayId, item.inventoryType }; }
			const bool hasCharacter = !Character::Get().name.empty();
			const auto root = Config::ClientData();
			std::thread([catalog = std::move(catalog), looks, hasCharacter, root, appearanceGeneration, all] {
				std::lock_guard loading(g_loading);
				try {
					const wowcore::Mpq mpq(root);
					// Staff, then chestguard, then the catalog. No archive access on the render thread.
					std::vector<std::uint32_t> entries;
					for (const auto entry : { 873u, 16897u }) { if (catalog.contains(entry)) { entries.push_back(entry); } }
					for (const auto& [entry, display] : catalog) { if (entry != 873 && entry != 16897) { entries.push_back(entry); } }
					for (const auto entry : entries) {
						const auto type = catalog.at(entry).inventoryType;
						const bool armorOnly = wowcore::characterPreviewSlot(type).has_value();
						const bool appearanceDependent = armorOnly || type == 1;
						if (!all && !appearanceDependent) { continue; }
						if (appearanceDependent) {
							std::lock_guard lock(g_lock);
							if (!hasCharacter || appearanceGeneration != g_appearanceGeneration) { continue; }
						}
						try {
							const DisplayKey key{ catalog.at(entry).displayId, type,
								appearanceDependent ? looks.race : 0, appearanceDependent ? looks.sex : 0 };
							{
								std::lock_guard lock(g_lock);
								if (const auto it = g_displays.find(key); it != g_displays.end()) {
									if (!appearanceDependent || appearanceGeneration == g_appearanceGeneration) { g_assets[entry] = it->second; }
									continue;
								}
							}
							auto item = std::make_shared<wowcore::ItemPreview>(armorOnly ?
								wowcore::loadArmorItemPreview(mpq, entry, catalog, looks) : wowcore::loadItemPreview(mpq, entry, catalog, looks));
							logger::info("item preview: preload entry={} display={} status={} detail={} parts={} armorOnly={} appearanceGeneration={}", entry, item->displayId,
								wowcore::itemPreviewStatusName(item->status), item->detail, item->parts.size(), item->armorOnly, appearanceGeneration);
							if (item->status != wowcore::ItemPreviewStatus::Ready) { continue; }
							auto asset = std::make_shared<Asset>();
							asset->item = std::move(item);
							asset->inventoryType = type;
							if (!Bounds(*asset)) { logger::warn("item preview: entry={} unsupported material/bones/bounds; Skyrim retained", entry); continue; }
							logger::info("item preview: ready entry={} centre=({},{},{}) radius={}", entry, asset->centre.x, asset->centre.y, asset->centre.z, asset->radius);
							std::lock_guard lock(g_lock);
							g_displays[key] = asset;
							if (!appearanceDependent || appearanceGeneration == g_appearanceGeneration) { g_assets[entry] = std::move(asset); }
						} catch (const std::exception& e) { logger::warn("item preview: entry={} preload failed: {}; Skyrim retained", entry, e.what()); }
					}
				} catch (const std::exception& e) { logger::error("item preview: preload failed: {}; Skyrim retained", e.what()); }
			}).detach();
		}
	}

	void Init()
	{
		// Ground assets work even when the optional menu hooks/setting are unavailable.
		if (g_info.installed && Config::Get().itemPreview) { RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&g_close); }
		{ std::lock_guard lock(g_lock); g_started = true; }
		Preload(true);
	}

	void Redress()
	{
		{ std::lock_guard lock(g_lock); if (!g_started) { return; } }
		Preload(false);
	}

	void Reset() { Select(nullptr, "reset"); }
	std::shared_ptr<const Asset> AssetFor(std::uint32_t a_entry)
	{
		std::lock_guard lock(g_lock);
		const auto it = g_assets.find(a_entry);
		return it == g_assets.end() ? nullptr : it->second;
	}
	std::string ModelFor(std::uint32_t a_entry)
	{
		std::lock_guard lock(g_lock);
		const auto it = g_assets.find(a_entry);
		return it != g_assets.end() && !it->second->item->parts.empty() ? it->second->item->parts.front().model.name : std::string();
	}

	Info GetInfo() { std::lock_guard lock(g_lock); return g_info; }
}
