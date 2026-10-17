#include "render/GroundItems.h"

#include "Frame.h"
#include "Gear.h"
#include "render/WowModels.h"

#include <glm/gtc/matrix_transform.hpp>

namespace GroundItems
{
	namespace
	{
		struct Geometry
		{
			RE::NiPointer<RE::BSGeometry> node;
			bool culled = false;
		};
		struct Tracked
		{
			RE::ObjectRefHandle handle;
			RE::NiPointer<RE::NiAVObject> root, physics;
			std::vector<Geometry> geometry;
			std::shared_ptr<const ItemPreview::Asset> asset;
			glm::mat4 fit{ 1.0f };
			bool hidden = false;
		};
		std::map<RE::FormID, Tracked> g_tracked;  // main thread only; handles do not pin references/cells
		double g_nextScan = 0;
		RE::TESObjectCELL* g_cell = nullptr;
		std::mutex g_lock;
		std::vector<Instance> g_instances;
		std::vector<DrawInfo> g_draws;

		glm::vec3 Point(const RE::NiPoint3& p) { return { p.x, p.y, p.z }; }
		glm::mat4 Transform(const RE::NiTransform& t)
		{
			glm::mat4 m(1.0f);
			for (int row = 0; row < 3; ++row) {
				for (int col = 0; col < 3; ++col) { m[col][row] = t.rotate.entry[row][col] * t.scale; }
			}
			m[3] = glm::vec4(Point(t.translate), 1.0f);
			return m;
		}
		bool Finite(const glm::mat4& m)
		{
			for (int col = 0; col < 4; ++col) {
				for (int row = 0; row < 4; ++row) { if (!std::isfinite(m[col][row])) { return false; } }
			}
			return true;
		}
		glm::mat3 Axes(const glm::vec3& size)
		{
			std::array<int, 3> order{ 0, 1, 2 };
			std::ranges::stable_sort(order, [&](int a, int b) { return size[a] > size[b]; });
			glm::vec3 longest(0), thinnest(0);
			longest[order[0]] = 1; thinnest[order[2]] = 1;
			return glm::mat3(longest, thinnest, glm::cross(longest, thinnest));
		}
		void Restore(Tracked& t)
		{
			if (t.hidden) { for (auto& g : t.geometry) { g.node->SetAppCulled(g.culled); } }
			t.hidden = false;
		}
		RE::NiAVObject* PhysicsOwner(RE::NiAVObject* root)
		{
			RE::NiAVObject* owner = root;
			RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* node) {
				if (node->GetCollisionObject()) { owner = node; return RE::BSVisit::BSVisitControl::kStop; }
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return owner;
		}
		bool Bind(Tracked& t, RE::NiAVObject* root, RE::TESBoundObject* base,
			const std::shared_ptr<const ItemPreview::Asset>& asset)
		{
			Restore(t);
			t.root.reset(root);
			t.physics.reset();
			t.geometry.clear();
			t.asset = asset;
			// A rigid body's owner may be below the reference root; Havok moves that node, while REFR position
			// and angles can lag. Leave the entire collision tree attached and sample the owner's world transform.
			t.physics.reset(PhysicsOwner(root));  // root fallback: placed refs with collision disabled by their author
			RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* node) {
				t.geometry.push_back({ RE::NiPointer<RE::BSGeometry>(node), node->GetAppCulled() });
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			const auto native = Transform(t.physics->world);
			const float scale = std::abs(t.physics->world.scale);
			const float radius = root->worldBound.radius;
			if (t.geometry.empty() || !Finite(native) || scale < 1e-5f || !std::isfinite(radius) || radius <= 0) { return false; }
			// Keep the visible item's centre and size inside the retained pickup/collision envelope. This is a
			// uniform fit, not a new collider. OBND supplies the placeholder's principal axis in reference space.
			const auto& b = base->boundData;
			const glm::vec3 extent(b.boundMax.x - b.boundMin.x, b.boundMax.y - b.boundMin.y, b.boundMax.z - b.boundMin.z);
			const glm::mat3 toPhysics = glm::mat3(glm::inverse(native) * Transform(root->world));
			glm::mat3 nativeAxes = toPhysics * Axes(extent);
			for (int i = 0; i < 3; ++i) { nativeAxes[i] = glm::normalize(nativeAxes[i]); }
			const glm::vec3 centre = glm::vec3(glm::inverse(native) * glm::vec4(Point(root->worldBound.center), 1));
			t.fit = glm::translate(glm::mat4(1.0f), centre) * glm::mat4(nativeAxes * glm::transpose(Axes(asset->size))) *
				glm::scale(glm::mat4(1.0f), glm::vec3(radius / scale / asset->radius)) *
				glm::translate(glm::mat4(1.0f), -asset->centre);
			return Finite(t.fit);
		}
		void Scan(RE::TESObjectCELL* playerCell, const RE::NiPoint3& at)
		{
			std::vector<std::pair<float, RE::ObjectRefHandle>> nearby;
			std::set<RE::FormID> seen;
			auto cell = [&](RE::TESObjectCELL* c) {
				if (!c || !c->IsAttached()) { return; }
				// The cell helper locks its references set. Never call TES::ForEachReference: its cell buffers
				// can be changing while loading. Only attached current/grid cells, on the main thread.
				c->ForEachReference([&](RE::TESObjectREFR* ref) {
					auto* base = ref->GetBaseObject();
					if (!base || (!base->IsArmor() && !base->IsWeapon()) || !Gear::EntryOf(base) ||
						!ref->Get3D() || ref->IsDisabled() || ref->IsDeleted() || ref->IsMarkedForDeletion() ||
						!seen.insert(ref->GetFormID()).second) { return RE::BSContainer::ForEachResult::kContinue; }
					const auto tracked = g_tracked.find(ref->GetFormID());
					const auto* owner = tracked != g_tracked.end() && tracked->second.root.get() == ref->Get3D() && tracked->second.physics ?
						tracked->second.physics.get() : PhysicsOwner(ref->Get3D());
					const float d = owner->world.translate.GetSquaredDistance(at);
					if (d <= kRadius * kRadius) { nearby.emplace_back(d, ref->GetHandle()); }
					return RE::BSContainer::ForEachResult::kContinue;
				});
			};
			cell(playerCell);
			auto* tes = RE::TES::GetSingleton();
			if (tes && !playerCell->IsInteriorCell() && tes->gridCells && tes->gridCells->cells) {
				const auto* grid = tes->gridCells;
				for (std::uint32_t x = 0; x < grid->length; ++x) {
					for (std::uint32_t y = 0; y < grid->length; ++y) { if (grid->GetCell(x, y) != playerCell) { cell(grid->GetCell(x, y)); } }
				}
			}
			if (!playerCell->IsInteriorCell()) {
				if (const auto* world = RE::PlayerCharacter::GetSingleton()->GetWorldspace()) {
					cell(world->persistentCell);
					cell(world->skyCell);
				}
			}
			std::ranges::sort(nearby, [](const auto& a, const auto& b) { return a.first < b.first; });
			if (nearby.size() > kMaxInstances) { nearby.resize(kMaxInstances); }
			std::set<RE::FormID> keep;
			for (const auto& [distance, handle] : nearby) {
				if (auto ref = handle.get()) {
					const auto id = ref->GetFormID();
					keep.insert(id);
					g_tracked[id].handle = handle;
				}
			}
			std::erase_if(g_tracked, [&](auto& row) {
				if (keep.contains(row.first)) { return false; }
				Restore(row.second); return true;
			});
		}
	}

	void Reset()
	{
		for (auto& [id, t] : g_tracked) { Restore(t); }
		g_tracked.clear(); g_cell = nullptr; g_nextScan = 0;
		std::lock_guard lock(g_lock);
		g_instances.clear(); g_draws.clear();
	}
	void Tick()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* ui = RE::UI::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		if (!ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME) ||
			!player || !player->Is3DLoaded() || !cell || !cell->IsAttached()) { Reset(); return; }
		if (cell != g_cell) { Reset(); g_cell = cell; }
		if (Frame::Now() >= g_nextScan) { Scan(cell, player->GetPosition()); g_nextScan = Frame::Now() + 0.5; }
		std::vector<Instance> instances;
		std::erase_if(g_tracked, [&](auto& row) {
			auto& t = row.second;
			auto ref = t.handle.get();
			auto* root = ref ? ref->Get3D() : nullptr;
			auto* parent = ref ? ref->GetParentCell() : nullptr;
			if (!root || !parent || !parent->IsAttached() || ref->IsDisabled() || ref->IsDeleted() || ref->IsMarkedForDeletion()) {
				Restore(t); return true;
			}
			const auto entry = Gear::EntryOf(ref->GetBaseObject());
			const auto asset = ItemPreview::AssetFor(entry);
			if (!asset) {
				ItemPreview::Want(entry);   // an Armory item's, the first time one lies near the player
				Restore(t); return false;
			}
			if (t.root.get() != root || t.asset != asset || t.geometry.empty()) {
				if (!Bind(t, root, ref->GetBaseObject(), asset)) { t.geometry.clear(); return false; }
			}
			const glm::mat4 world = Transform(t.physics->world) * t.fit;
			if (!Finite(world)) { Restore(t); return false; }
			const auto centre = world * glm::vec4(asset->centre, 1);
			const auto delta = glm::vec3(centre) - Point(player->GetPosition());
			if (glm::dot(delta, delta) > kRadius * kRadius) { Restore(t); return false; }
			if (WowModels::GroundReady(asset->item)) {
				for (auto& g : t.geometry) { g.node->SetAppCulled(true); }
				t.hidden = true;
			} else { Restore(t); }
			instances.push_back({ row.first, entry, asset, world, t.hidden });
			return false;
		});
		std::lock_guard lock(g_lock);
		g_instances = std::move(instances);
	}
	std::vector<Instance> Current() { std::lock_guard lock(g_lock); return g_instances; }
	void RecordDraws(std::vector<DrawInfo> a_draws) { std::lock_guard lock(g_lock); g_draws = std::move(a_draws); }
	std::vector<DrawInfo> Draws() { std::lock_guard lock(g_lock); return g_draws; }
}
