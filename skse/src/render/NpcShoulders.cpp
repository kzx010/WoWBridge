#include "render/NpcShoulders.h"
#include "render/NpcWeapons.h"

#include "Armory.h"
#include "Config.h"
#include "Frame.h"
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace NpcShoulders
{
	namespace
	{
		std::map<RE::FormID, RE::ActorHandle> g_tracked;  // main thread; does not pin a cell or its 3D
		RE::TESObjectCELL* g_cell = nullptr;
		double g_nextScan = 0;
		std::atomic<bool> g_enabled = true;
		std::uint64_t g_gather = 0;
		std::mutex g_lock;
		std::vector<Instance> g_instances;
		std::vector<DrawInfo> g_draws;
		// One background loader, cached by display (including failed requests). No archive access in either frame loop.
		std::map<std::uint32_t, std::shared_ptr<const Asset>> g_assets;
		std::set<std::uint32_t> g_wanted;
		std::deque<std::pair<std::uint32_t, std::uint32_t>> g_queue;  // entry, display
		bool g_working = false;

		struct Calibration { glm::vec3 offset; float scale; };
		// The sex calibration table; offsets are in the neutral Skyrim skeleton frame (x out along the clavicle, z up),
		// scales multiply 64 units/yard. Lifted 6 units and pushed 1.5 out after the user saw the first fit's inner
		// edges sink into a leather cuirass (October 16); wb npcshoulders lift|out|scale N tunes them live.
		//                       male                 female
		Calibration g_fit[] = { { { 1.5f, 0, 6.0f }, 0.90f }, { { 1.0f, 0, 5.0f }, 0.82f } };
		// Neutral model-space rotations measured from the shipped SSE skeleton.nif/skeleton_female.nif (BS 100).
		// Their shoulder bases agree within 1e-5. Rows, left/right: clavicle then UpperArm. A yaw alone is wrong:
		// the clavicle's +Z points out toward the shoulder joint, 14.044 units from its origin near the neck.
		constexpr float kBind[2][2][3][3] = {
			{ { { -0.449009f, -0.392398f, -0.802755f }, { 0.305089f, 0.777091f, -0.550500f }, { 0.839828f, -0.492091f, -0.229204f } },
			  { { -0.449007f, 0.392393f, 0.802758f }, { -0.305093f, 0.777093f, -0.550495f }, { -0.839828f, -0.492092f, -0.229204f } } },
			{ { { -0.907319f, -0.077140f, -0.413307f }, { -0.087384f, 0.996157f, 0.005906f }, { 0.411263f, 0.041475f, -0.910573f } },
			  { { -0.907319f, 0.077140f, 0.413305f }, { 0.087384f, 0.996157f, 0.005907f }, { -0.411261f, 0.041475f, -0.910573f } } }
		};
		// Retain animated node rotation/scale, undo its neutral bone basis, then turn WoW +X forward/+Y left to
		// Skyrim +Y forward/-X left. The UpperArm origin is already at the joint, so it has no clavicle-length shift.
		glm::mat4 Fit(bool female, std::size_t side, bool upperArm)
		{
			const auto& fit = g_fit[female ? 1 : 0];
			glm::mat4 inverseBind(1);
			for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) { inverseBind[col][row] = kBind[upperArm ? 1 : 0][side][col][row]; }
			return glm::translate(glm::mat4(1), glm::vec3(0, 0, upperArm ? 0.0f : 14.04396f)) * inverseBind *
				glm::scale(glm::rotate(glm::translate(glm::mat4(1), fit.offset), glm::radians(90.0f), glm::vec3(0, 0, 1)), glm::vec3(64.0f * fit.scale));
		}
		glm::mat4 Transform(const RE::NiTransform& t)
		{
			glm::mat4 m(1);
			for (int row = 0; row < 3; ++row) {
				for (int col = 0; col < 3; ++col) { m[col][row] = t.rotate.entry[row][col] * t.scale; }
			}
			m[3] = glm::vec4(t.translate.x, t.translate.y, t.translate.z, 1);
			return m;
		}
		bool Finite(const glm::mat4& m)
		{
			for (int col = 0; col < 4; ++col) {
				for (int row = 0; row < 4; ++row) { if (!std::isfinite(m[col][row])) { return false; } }
			}
			return true;
		}
		const Armory::Item* Worn(RE::Actor* actor)
		{
			const auto* item = Armory::Find(actor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kModShoulder));  // slot 57
			return item && item->look.inventoryType == 3 && item->look.displayId ? item : nullptr;
		}
		std::shared_ptr<const Asset> Want(std::uint32_t entry, std::uint32_t display)
		{
			{
				std::lock_guard lock(g_lock);
				if (const auto it = g_assets.find(display); it != g_assets.end()) { return it->second; }
				if (!g_wanted.insert(display).second) { return nullptr; }
				g_queue.emplace_back(entry, display);
				if (g_working) { return nullptr; }
				g_working = true;
			}
			std::thread([root = Config::ClientData()] {
				std::optional<wowcore::Mpq> mpq;
				std::optional<wowcore::Dbc> displays;
				for (;;) {
					std::pair<std::uint32_t, std::uint32_t> next;
					{
						std::lock_guard lock(g_lock);
						if (g_queue.empty()) { g_working = false; return; }
						next = g_queue.front(); g_queue.pop_front();
					}
					try {
						if (!mpq) { mpq.emplace(root); displays = wowcore::Dbc::load(*mpq, "DBFilesClient\\ItemDisplayInfo.dbc"); }
						auto item = std::make_shared<wowcore::ItemPreview>(wowcore::loadItemPreview(*mpq, next.first,
							{ { next.first, { next.second, 3 } } }));
						auto asset = std::make_shared<Asset>();
						asset->item = item;
						// DBC columns are LShoulder then RShoulder (e.g. display 18971). ItemPreview omits empty
						// columns, so keep their side identity for asymmetric/one-sided items too.
						const auto row = displays ? displays->rowOf(next.second) : std::nullopt;
						if (row) for (std::size_t column = 0; column < 2; ++column) {
							if (*displays->str(*row, 1 + static_cast<std::uint32_t>(column))) { asset->sides.push_back(column); }
						}
						if (item->status != wowcore::ItemPreviewStatus::Ready || asset->sides.empty() || asset->sides.size() != item->parts.size()) {
							logger::warn("NPC shoulders: display={} status={} detail={}", next.second, wowcore::itemPreviewStatusName(item->status), item->detail);
							continue;
						}
						logger::info("NPC shoulders: display={} loaded {} models", next.second, item->parts.size());
						std::lock_guard lock(g_lock);
						g_assets[next.second] = std::move(asset);
					} catch (const std::exception& e) { logger::warn("NPC shoulders: display={} load failed: {}", next.second, e.what()); }
				}
			}).detach();
			return nullptr;
		}
		void Scan(RE::TESObjectCELL* playerCell, const RE::NiPoint3& at)
		{
			std::vector<std::pair<float, RE::ActorHandle>> nearby;
			std::set<RE::FormID> seen;
			const auto cell = [&](RE::TESObjectCELL* c) {
				if (!c || !c->IsAttached()) { return; }
				c->ForEachReference([&](RE::TESObjectREFR* ref) {
					auto* actor = ref->As<RE::Actor>();
					if (!actor || actor->IsPlayerRef() || !actor->Get3D(false) || actor->IsDisabled() || actor->IsDeleted() ||
						actor->IsMarkedForDeletion() || !seen.insert(actor->GetFormID()).second) { return RE::BSContainer::ForEachResult::kContinue; }
					const float distance = actor->GetPosition().GetSquaredDistance(at);
					if (distance <= kRadius * kRadius && Worn(actor)) { nearby.emplace_back(distance, actor->GetHandle()); }
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
				if (const auto* world = RE::PlayerCharacter::GetSingleton()->GetWorldspace()) { cell(world->persistentCell); cell(world->skyCell); }
			}
			std::ranges::sort(nearby, [](const auto& a, const auto& b) { return a.first < b.first; });
			if (nearby.size() > kMaxInstances) { nearby.resize(kMaxInstances); }
			g_tracked.clear();
			for (const auto& [distance, handle] : nearby) { if (auto actor = handle.get()) { g_tracked[actor->GetFormID()] = handle; } }
		}
	}

	void Reset()
	{
		g_tracked.clear(); g_cell = nullptr; g_nextScan = 0;
		std::lock_guard lock(g_lock);
		g_instances.clear(); g_draws.clear();
	}
	void SetEnabled(bool a_enabled) { g_enabled = a_enabled; Reset(); }
	std::array<float, 3> SetFit(bool a_female, const char* a_which, float a_value)
	{
		auto& fit = g_fit[a_female ? 1 : 0];
		if (a_which && std::string_view(a_which) == "lift") fit.offset.z = a_value;
		else if (a_which && std::string_view(a_which) == "out") fit.offset.x = a_value;
		else if (a_which && std::string_view(a_which) == "scale" && a_value > 0) fit.scale = a_value;
		return { fit.offset.z, fit.offset.x, fit.scale };
	}
	bool Enabled() { return g_enabled; }
	void Tick()
	{
		if (!g_enabled || !Armory::Ready()) { return; }
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* ui = RE::UI::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		if (!ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME) ||
			!player || !player->Is3DLoaded() || !cell || !cell->IsAttached()) { Reset(); return; }
		if (cell != g_cell) { Reset(); g_cell = cell; }
		if (Frame::Now() >= g_nextScan) { Scan(cell, player->GetPosition()); g_nextScan = Frame::Now() + 0.5; }
		std::vector<Instance> instances;
		const auto gather = ++g_gather;
		std::erase_if(g_tracked, [&](auto& row) {
			auto actor = row.second.get();
			auto* root = actor ? actor->Get3D(false) : nullptr;
			auto* parent = actor ? actor->GetParentCell() : nullptr;
			if (!root || !parent || !parent->IsAttached() || actor->IsDisabled() || actor->IsDeleted() || actor->IsMarkedForDeletion() ||
				actor->GetPosition().GetSquaredDistance(player->GetPosition()) > kRadius * kRadius) { return true; }
			const auto* item = Worn(actor.get());
			if (!item) { return true; }
			Instance instance;
			instance.ref = row.first; instance.entry = item->entry; instance.display = item->look.displayId; instance.gather = gather;
			const auto* base = actor->GetActorBase();
			instance.female = base && base->GetSex() == RE::SEX::kFemale;
			const char* const clavicles[] = { "NPC L Clavicle [LClv]", "NPC R Clavicle [RClv]" };
			const char* const arms[] = { "NPC L UpperArm [LUar]", "NPC R UpperArm [RUar]" };
			for (std::size_t side = 0; side < 2; ++side) {
				auto* node = root->GetObjectByName(clavicles[side]);
				if (!node) { node = root->GetObjectByName(arms[side]); instance.upperArm[side] = true; }
				if (!node) { return false; }  // no fabricated position-only fallback for a non-humanoid skeleton
				instance.bodyBounds[side] = node->worldBound;
				instance.nodes[side] = Transform(node->world);
				instance.world[side] = instance.nodes[side] * Fit(instance.female, side, instance.upperArm[side]);
				if (!Finite(instance.world[side])) { return false; }
			}
			if (const auto& biped = actor->GetBiped(false)) for (const auto& object : biped->objects)
				if (object.item == item->form && object.partClone) { instance.nativeBound = object.partClone->worldBound; break; }
			instance.asset = Want(instance.entry, instance.display);
			instances.push_back(std::move(instance));  // gather remains observable while its assets load
			return false;
		});
		std::lock_guard lock(g_lock);
		g_instances = std::move(instances);
	}
	nlohmann::json Evidence(const Instance& i)
	{
		nlohmann::json sides = nlohmann::json::array();
		FitEvidence::Bounds combined;
		if (i.asset) for (std::size_t part = 0; part < i.asset->item->parts.size(); ++part) {
			const auto side = i.asset->sides[part];
			const auto bounds = FitEvidence::TransformBounds(FitEvidence::ModelBounds(i.asset->item->parts[part].model), i.world[side]);
			if (bounds.valid) { combined.Add(bounds.lower); combined.Add(bounds.upper); }
			const auto centre = (bounds.lower + bounds.upper) * .5f;
			sides.push_back({ { "side", side ? "right" : "left" }, { "item_bound", FitEvidence::Bound(bounds) }, { "item_px", FitEvidence::Pixels(bounds) },
				{ "attachment", FitEvidence::Vector(glm::vec3(i.nodes[side][3])) }, { "body_bound", FitEvidence::Sphere(i.bodyBounds[side]) },
				{ "centre_offset", FitEvidence::Vector(centre - glm::vec3(i.nodes[side][3])) },
				{ "centre_distance", FitEvidence::Range(bounds.valid ? std::optional(glm::length(centre - glm::vec3(i.nodes[side][3]))) : std::nullopt, 0, 12) } });
		}
		return { { "sides", sides }, { "item_bound", FitEvidence::Bound(combined) }, { "item_px", FitEvidence::Pixels(combined) }, { "native_bound", FitEvidence::Sphere(i.nativeBound) },
			{ "scale_ratio", i.nativeBound.radius > 0 && combined.valid ? nlohmann::json(glm::length(combined.upper - combined.lower) * .5f / i.nativeBound.radius) : nlohmann::json() } };
	}
	std::vector<Instance> Current() { std::lock_guard lock(g_lock); return g_instances; }
	void RecordDraws(std::vector<DrawInfo> a_draws) { std::lock_guard lock(g_lock); g_draws = std::move(a_draws); }
	std::vector<DrawInfo> Draws() { std::lock_guard lock(g_lock); return g_draws; }
}
