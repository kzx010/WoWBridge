#include "render/NpcHelms.h"

#include "Armory.h"
#include "Config.h"
#include "Frame.h"
#include "Gear.h"
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <cstring>

namespace NpcHelms
{
	namespace
	{
		struct Geometry { RE::NiPointer<RE::BSGeometry> node; bool culled; };
		struct Group { std::vector<Geometry> geometry; bool hidden = false; };
		struct HeadCache
		{
			RE::NiPointer<RE::NiAVObject> root, face;
			std::vector<RE::NiPointer<RE::BSGeometry>> parts;
			FitEvidence::Bounds head, hair, foreheadSlice;
			float forehead = 0, radius = 0;
			bool complete = false, hairMeasured = false, verified = false;
			nlohmann::json verification;
			std::uint32_t vertices = 0;
			double retryAt = 0;
			bool browMeasured = false;
		};
		struct Tracked { RE::ActorHandle handle; Group helmet, hair, beard; RE::NiPointer<RE::NiAVObject> faceShown; HeadCache head; std::string fitReport; };   // faceShown: a face node we un-culled
		std::map<RE::FormID, Tracked> g_tracked;
		RE::TESObjectCELL* g_cell = nullptr;
		double g_nextScan = 0;
		std::atomic<bool> g_enabled = true;
		std::uint64_t g_gather = 0;
		std::mutex g_lock;
		std::vector<Instance> g_instances;
		std::vector<DrawInfo> g_draws;
		using OwnedAsset = std::shared_ptr<const Asset>;
		std::map<std::uint32_t, std::array<OwnedAsset, 2>> g_assets;
		std::set<std::uint32_t> g_wanted;  // failures are cached too
		std::deque<std::pair<std::uint32_t, std::uint32_t>> g_queue;  // entry, display
		bool g_working = false;
		std::map<OwnedAsset, std::chrono::steady_clock::time_point, std::owner_less<OwnedAsset>> g_ready;
		struct Calibration { float lift, out, scale, pitch; };
		// Console values are additive trims over the measured fit, independently for each sex.
		std::array<Calibration, 2> g_fit{};
		// Preserve the reviewed fit from 8141691 when automatic fitting cannot be trusted.
		constexpr Calibration kFit[] = { { 11.5f, 2.0f, 1.12f, 0 }, { 11.0f, 6.0f, 1.20f, 0 } };
		struct Adjust { const char* model; float lift, scaleMale, scaleFemale; };
		constexpr Adjust kAdjust[] = {
			{ "Helm_Plate_A_02", 2.0f, 1.12f, 1.04f },
			{ "Helm_Mail_A_02", 1.5f, 1.00f, 1.00f },
			{ "Helm_Cloth_A_01", 1.5f, 1.00f, 1.00f },
			{ "Helm_Circlet", 1.5f, 0.98f, 1.08f },
			{ "Helm_Plate_B_01", 2.f, 1.00f, 1.00f },
		};
		constexpr float kElfEars = 1.12f;
		constexpr float kMargin = 1.5f, kThickness = 2.f;
		constexpr float kMinScale = .85f, kMaxScale = 1.45f, kMaxOffset = 10.f, kBoundTolerance = 3.f;
		glm::mat4 Transform(const RE::NiTransform& t)
		{
			glm::mat4 m(1);
			for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) m[col][row] = t.rotate.entry[row][col] * t.scale;
			m[3] = glm::vec4(t.translate.x, t.translate.y, t.translate.z, 1);
			return m;
		}
		bool Finite(const glm::mat4& m)
		{
			for (int col = 0; col < 4; ++col) for (int row = 0; row < 4; ++row) if (!std::isfinite(m[col][row])) return false;
			return true;
		}
		struct Equipped { RE::TESBoundObject* form = nullptr; const Character::Item* look = nullptr; };
		Equipped Worn(RE::Actor* actor)
		{
			const auto& biped = actor->GetBiped(false);
			if (biped) for (const auto slot : { RE::BIPED_OBJECT::kHair, RE::BIPED_OBJECT::kCirclet }) {
				// Read the biped rows, never walk GetWornArmor's inventory per frame.
				auto* item = biped->objects[static_cast<std::size_t>(slot)].item;
				auto* form = item ? item->As<RE::TESBoundObject>() : nullptr;
				if (!form || !form->IsArmor()) continue;
				const Character::Item* look = nullptr;
				if (const auto* armory = Armory::Find(form)) look = &armory->look;
				else if (const auto it = Character::Gear().find(Gear::EntryOf(form)); it != Character::Gear().end()) look = &it->second;
				if (look && look->inventoryType == 1 && look->displayId) return { form, look };
			}
			return {};
		}
		void Restore(Group& group)
		{
			if (group.hidden) for (auto& g : group.geometry) g.node->SetAppCulled(g.culled);
			group.hidden = false;
		}
		void Unbind(Tracked& tracked)
		{
			for (auto* group : { &tracked.helmet, &tracked.hair, &tracked.beard }) { Restore(*group); group->geometry.clear(); }
			if (tracked.faceShown) { tracked.faceShown->SetAppCulled(true); tracked.faceShown.reset(); }
		}
		// Head-covering Skyrim armour makes the engine cull the FaceGen node itself (the plate helm, coif and horned
		// helm templates: the background showed through the face opening, October 17); while the WoW helm stands in
		// for the culled Skyrim mesh the face must show again, restored on release.
		void ShowFace(Tracked& tracked, RE::Actor* actor)
		{
			auto* face = actor->GetFaceNodeSkinned();
			if (!face) return;
			// Every Tick: the engine re-culls the face node each frame while the head-covering armour is worn.
			for (auto* n = static_cast<RE::NiAVObject*>(face); n; n = n->parent) {
				if (n == actor->Get3D(false)) break;
				if (n->GetAppCulled() || tracked.faceShown.get() == n) {
					if (!tracked.faceShown) tracked.faceShown.reset(n);
					n->SetAppCulled(false);
				}
			}
		}
		void Collect(RE::NiAVObject* node, std::vector<RE::BSGeometry*>& out)
		{
			if (node) RE::BSVisit::TraverseScenegraphGeometries(node, [&](RE::BSGeometry* geo) {
				if (std::ranges::find(out, geo) == out.end()) out.push_back(geo);
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
		void Bind(Group& group, const std::vector<RE::BSGeometry*>& geometry)
		{
			if (group.geometry.size() == geometry.size() && std::ranges::equal(group.geometry, geometry,
				[](const auto& a, auto* b) { return a.node.get() == b; })) return;
			Restore(group); group.geometry.clear();
			for (auto* geo : geometry) group.geometry.push_back({ RE::NiPointer<RE::BSGeometry>(geo), geo->GetAppCulled() });
		}
		void Hide(Group& group, bool hide)
		{
			if (!hide) { Restore(group); return; }
			// Capture the flag when taking ownership, not while a model is still loading.
			for (auto& geo : group.geometry) {
				if (!group.hidden) geo.culled = geo.node->GetAppCulled();
				geo.node->SetAppCulled(true);
			}
			group.hidden = !group.geometry.empty();
		}
		std::vector<RE::BSGeometry*> HeadParts(RE::Actor* actor, RE::BGSHeadPart::HeadPartType type)
		{
			std::vector<RE::BSGeometry*> geometry;
			auto* face = actor->GetFaceNodeSkinned();
			auto* base = actor->GetActorBase();
			if (!face || !base) return geometry;
			// FaceGen names each part by its BGSHeadPart EDID (Actor::GetHeadPartObject does this lookup).
			// Include extra parts (hairlines/strands) even when their own type is Misc. Never cull the face/eyes.
			std::set<RE::BGSHeadPart*> seen;
			const auto part = [&](auto&& self, RE::BGSHeadPart* p) -> void {
				if (!p || !seen.insert(p).second) return;
				if (!p->formEditorID.empty()) Collect(face->GetObjectByName(p->formEditorID), geometry);
				for (auto* extra : p->extraParts) self(self, extra);
			};
			part(part, base->GetCurrentHeadPartByType(type));
			return geometry;
		}
		struct PartSample
		{
			std::vector<glm::vec3> points;  // head-bone space, after skinning into world space
			nlohmann::json verification;
			bool verified = false;
		};
		// Dynamic float4 and static float/half positions are MESH space. Do not interpret them as head
		// coordinates or apply geometry.world to already skinned world positions (that transforms twice).
		PartSample Vertices(RE::BSGeometry* geo, RE::NiAVObject* head)
		{
			PartSample sample;
			sample.verification = { { "geometry", geo->name.c_str() }, { "engine_world_bound", FitEvidence::Sphere(geo->worldBound) },
				{ "skinned_world_aabb", nullptr }, { "ok", false }, { "reason", "CPU positions unavailable" } };
			auto* shape = geo->AsTriShape();
			const auto& data = geo->GetGeometryRuntimeData();
			const auto count = shape && shape->GetTrishapeRuntimeData().vertexCount ? shape->GetTrishapeRuntimeData().vertexCount :
				data.skinInstance && data.skinInstance->skinPartition ? data.skinInstance->skinPartition->vertexCount : 0;
			if (!count) return sample;
			std::vector<glm::vec3> mesh;
			if (auto* dynamic = geo->AsDynamicTriShape()) {
				auto& stream = dynamic->GetDynamicTrishapeRuntimeData();
				RE::BSSpinLockGuard guard(stream.lock);
				if (stream.dynamicData && stream.dataSize >= count * sizeof(glm::vec4)) {
					mesh.resize(count);
					for (std::uint32_t v = 0; v < count; ++v)
						std::memcpy(&mesh[v], static_cast<const std::byte*>(stream.dynamicData) + v * sizeof(glm::vec4), sizeof(glm::vec3));
				}
			}
			const auto read = [&](const RE::BSGraphics::TriShape* buffer) {
				if (!mesh.empty() || !buffer || !buffer->rawVertexData || !buffer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX)) return;
				std::uint64_t desc = 0; std::memcpy(&desc, &buffer->vertexDesc, sizeof(desc));
				const std::size_t stride = (desc & 15) * 4;
				const bool full = buffer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC);
				if (stride < (full ? 12u : 6u) || stride > 256) return;
				mesh.resize(count);
				for (std::uint32_t v = 0; v < count; ++v) {
					const auto* row = buffer->rawVertexData + v * stride;
					if (full) std::memcpy(&mesh[v], row, sizeof(glm::vec3));
					else { std::uint16_t xyz[3]; std::memcpy(xyz, row, sizeof(xyz)); for (int axis = 0; axis < 3; ++axis) mesh[v][axis] = glm::unpackHalf1x16(xyz[axis]); }
				}
			};
			read(data.rendererData);
			if (mesh.empty() && data.skinInstance && data.skinInstance->skinPartition) {
				const auto& partition = data.skinInstance->skinPartition;
				for (std::uint32_t n = 0; n < partition->numPartitions && mesh.empty(); ++n) read(partition->partitions[n].buffData);
			}
			if (mesh.empty()) return sample;
			std::vector<glm::vec3> world(count, glm::vec3(0));
			std::vector<float> weights(count, 0);
			std::string_view frame = "geometry.world";
			bool mapped = true;
			const auto& skin = data.skinInstance;
			if (skin) {
				mapped = false;
				if (skin->skinData) {
					std::optional<glm::mat4> headToWorld;
					for (std::uint32_t bone = 0; bone < skin->skinData->GetBoneCount(); ++bone) {
						const auto* transform = skin->boneWorldTransforms ? skin->boneWorldTransforms[bone] : nullptr;
						if (!transform && skin->bones && skin->bones[bone]) transform = &skin->bones[bone]->world;
						if (!transform) continue;
						const auto matrix = Transform(*transform) * Transform(skin->skinData->GetBoneDataSkinToBone(bone));
						if (!Finite(matrix)) continue;
						if (skin->bones && skin->bones[bone] == head) headToWorld = matrix;
						const auto* influences = skin->skinData->GetBoneDataBoneVertData(bone);
						if (influences) for (std::uint32_t n = 0; n < skin->skinData->GetBoneDataVerts(bone); ++n) {
							const auto& influence = influences[n];
							if (influence.vert >= count || !(influence.weight > 0) || !std::isfinite(influence.weight)) continue;
							world[influence.vert] += glm::vec3(matrix * glm::vec4(mesh[influence.vert], 1)) * influence.weight;
							weights[influence.vert] += influence.weight;
						}
					}
					if (std::ranges::all_of(weights, [](float w) { return std::abs(w - 1.f) < .02f; })) {
						for (std::uint32_t v = 0; v < count; ++v) world[v] /= weights[v];
						mapped = true; frame = "sum(boneWorld * skinToBone * mesh * weight)";
					} else if (headToWorld) {
						// SSE can discard NiSkinData's CPU weights. FaceGen is dominated by the head bone;
						// this approximation is usable ONLY if it agrees with this geometry's engine bound.
						for (std::uint32_t v = 0; v < count; ++v) world[v] = glm::vec3(*headToWorld * glm::vec4(mesh[v], 1));
						mapped = true; frame = "headBoneWorld * skinToBone (dominant head, bound checked)";
					} else if (skin->rootParent) {
						const auto matrix = Transform(skin->rootParent->world) * glm::inverse(Transform(skin->skinData->rootParentToSkin));
						for (std::uint32_t v = 0; v < count; ++v) world[v] = glm::vec3(matrix * glm::vec4(mesh[v], 1));
						mapped = Finite(matrix); frame = "skin root world * inverse(rootParentToSkin) (bound checked)";
					}
				}
			} else for (std::uint32_t v = 0; v < count; ++v) world[v] = glm::vec3(Transform(geo->world) * glm::vec4(mesh[v], 1));
			FitEvidence::Bounds worldBounds, geometryBounds;
			const auto intoHead = glm::inverse(Transform(head->world));
			const auto& bound = geo->worldBound;
			const glm::vec3 boundCentre(bound.center.x, bound.center.y, bound.center.z);
			float farthest = 0;
			for (std::uint32_t v = 0; v < count; ++v) {
				geometryBounds.Add(glm::vec3(Transform(geo->world) * glm::vec4(mesh[v], 1)));
				if (!mapped || !std::isfinite(world[v].x) || !std::isfinite(world[v].y) || !std::isfinite(world[v].z)) continue;
				worldBounds.Add(world[v]); sample.points.push_back(glm::vec3(intoHead * glm::vec4(world[v], 1)));
				farthest = std::max(farthest, glm::length(world[v] - boundCentre));
			}
			const float centreError = glm::length((worldBounds.lower + worldBounds.upper) * .5f - boundCentre);
			const float radiusError = std::abs(farthest - bound.radius);
			sample.verified = mapped && sample.points.size() == count && worldBounds.valid && bound.radius > 0 &&
				centreError <= kBoundTolerance && radiusError <= kBoundTolerance;
			sample.verification = { { "geometry", geo->name.c_str() }, { "vertex_frame", frame },
				{ "mesh_geometry_world_aabb", FitEvidence::Bound(geometryBounds) }, { "skinned_world_aabb", FitEvidence::Bound(worldBounds) },
				{ "engine_world_bound", FitEvidence::Sphere(bound) }, { "vertices", sample.points.size() },
				{ "centre_error", FitEvidence::Range(worldBounds.valid ? std::optional(centreError) : std::nullopt, 0, kBoundTolerance) },
				{ "radius_error", FitEvidence::Range(worldBounds.valid ? std::optional(radiusError) : std::nullopt, 0, kBoundTolerance) },
				{ "max_vertex_outside_sphere", worldBounds.valid ? nlohmann::json(std::max(0.f, farthest - bound.radius)) : nlohmann::json() }, { "ok", sample.verified } };
			return sample;
		}
		bool Measure(HeadCache& cache, RE::Actor* actor, RE::NiAVObject* head)
		{
			const auto face = HeadParts(actor, RE::BGSHeadPart::HeadPartType::kFace);
			const auto hair = HeadParts(actor, RE::BGSHeadPart::HeadPartType::kHair);
			const auto brows = HeadParts(actor, RE::BGSHeadPart::HeadPartType::kEyebrows);
			std::vector<RE::NiPointer<RE::BSGeometry>> parts;
			for (const auto* list : { &face, &hair, &brows }) for (auto* geo : *list) parts.emplace_back(geo);
			if (cache.root.get() == actor->Get3D(false) && cache.face.get() == actor->GetFaceNodeSkinned() && cache.parts == parts &&
				(cache.complete || Frame::Now() < cache.retryAt)) return cache.verified;
			cache = {}; cache.root.reset(actor->Get3D(false)); cache.face.reset(actor->GetFaceNodeSkinned()); cache.parts = std::move(parts); cache.retryAt = Frame::Now() + 5;
			const auto eyes = HeadParts(actor, RE::BGSHeadPart::HeadPartType::kEyes);
			std::vector<glm::vec3> headPoints;
			bool verified = !face.empty();
			cache.verification = { { "parts", nlohmann::json::array() } };
			// Only kFace: eyes, hair, beards and unrelated FaceGen children cannot enlarge the skull.
			for (auto* geo : face) if (std::ranges::find(eyes, geo) == eyes.end() && std::ranges::find(hair, geo) == hair.end()) {
				const auto sample = Vertices(geo, head);
				verified = verified && sample.verified;
				cache.verification["parts"].push_back(sample.verification);
				for (const auto& p : sample.points) { cache.head.Add(p); headPoints.push_back(p); ++cache.vertices; }
			}
			const auto size = cache.head.upper - cache.head.lower;
			// Broad adult limits allow morphs and ears, but reject mesh units, skeleton offsets and corrupt streams.
			const bool adult = cache.head.valid && size.x >= 10 && size.x <= 24 && size.y >= 14 && size.y <= 26 && size.z >= 18 && size.z <= 28;
			cache.verified = verified && adult;
			cache.verification["head_size"] = FitEvidence::Vector(size);
			cache.verification["adult_size_expected"] = { { "width", { 10, 24 } }, { "depth", { 14, 26 } }, { "height", { 18, 28 } } };
			cache.verification["adult_size_ok"] = adult;
			cache.verification["ok"] = cache.verified;
			logger::info("NPC helms: head extents ref={:08X} head_local={} verification={}", actor->GetFormID(),
				FitEvidence::Bound(cache.head).dump(), cache.verification.dump());
			FitEvidence::Bounds brow;
			for (auto* geo : brows) {
				const auto sample = Vertices(geo, head);
				if (sample.verified) for (const auto& p : sample.points) brow.Add(p);
			}
			cache.browMeasured = brow.valid;
			cache.forehead = brow.valid ? brow.upper.z + .5f : 0;
			// A forehead cross-section, not the whole skull, ears or a hairstyle. Use its own horizontal centre.
			if (cache.browMeasured) for (const auto& p : headPoints) if (std::abs(p.z - cache.forehead) <= 1.5f) cache.foreheadSlice.Add(p);
			if (cache.foreheadSlice.valid) {
				const auto centre = glm::vec2(cache.foreheadSlice.lower + cache.foreheadSlice.upper) * .5f;
				for (const auto& p : headPoints) if (std::abs(p.z - cache.forehead) <= 1.5f)
					cache.radius = std::max(cache.radius, glm::length(glm::vec2(p) - centre));
			}
			cache.hairMeasured = true;
			for (auto* geo : hair) {
				const auto sample = Vertices(geo, head);
				cache.hairMeasured = cache.hairMeasured && sample.verified;
				for (const auto& p : sample.points) cache.hair.Add(p);
			}
			cache.complete = cache.verified && (brows.empty() || cache.browMeasured) && cache.hairMeasured;
			return cache.verified;
		}
		bool Fit(Instance& i, const HeadCache& head)
		{
			if (!i.asset || !i.asset->bounds.valid) return false;
			i.head = head.head; i.hair = head.hair; i.headVertices = head.vertices; i.forehead = head.forehead;
			i.foreheadRadius = head.radius; i.browMeasured = head.browMeasured; i.headVerification = head.verification;
			const auto& trim = g_fit[i.female ? 1 : 0];
			const auto rotation = glm::rotate(glm::mat4(1), glm::radians(90.f), glm::vec3(0, 0, 1)) * glm::scale(glm::mat4(1), glm::vec3(64.f));
			i.outer = FitEvidence::ModelBounds(i.asset->item->parts[0].model, rotation);
			i.interior = i.outer;
			// A circlet has a thin band, not a two-unit shell. Hair is deliberately absent from its target.
			const auto thickness = i.asset->circlet ? glm::vec3(.5f, .5f, 0) : glm::vec3(kThickness);
			i.interior.lower += thickness; i.interior.upper -= thickness;
			const auto size = i.interior.upper - i.interior.lower;
			if (!head.verified) i.fitRejection = "head vertices unavailable or disagree with engine worldBound/adult size";
			else if (i.asset->circlet && (!head.browMeasured || !head.foreheadSlice.valid || head.radius < 7 || head.radius > 11))
				i.fitRejection = "forehead cross-section unavailable or radius outside 7..11 units";
			else if (size.x <= 0 || size.y <= 0 || (!i.asset->circlet && size.z <= 0)) i.fitRejection = "degenerate estimated interior";
			else {
				i.autoFitAttempted = true;
				const auto& target = i.asset->circlet ? head.foreheadSlice : head.head;
				const float margin = i.asset->circlet ? .5f : kMargin;
				const auto required = (target.upper - target.lower + glm::vec3(2 * margin)) / size;
				i.computedScale = std::max(required.x, required.y);
				if (!i.asset->circlet) i.computedScale = std::max(i.computedScale, required.z);
				else i.computedScale = std::max(i.computedScale, (head.radius + margin) / (std::max(size.x, size.y) * .5f));
				const auto centre = (target.lower + target.upper) * .5f;
				const auto interiorCentre = (i.interior.lower + i.interior.upper) * .5f;
				i.computedOffset = centre - interiorCentre * i.computedScale;
				i.computedOffset.z = i.asset->circlet ? head.forehead - (i.outer.lower.z + i.outer.upper.z) * .5f * i.computedScale :
					head.head.upper.z + kMargin - i.interior.upper.z * i.computedScale;
				if (i.asset->openFace) i.computedOffset.y = std::max(i.computedOffset.y, head.head.upper.y + kMargin - i.interior.upper.y * i.computedScale);
				i.scale = i.computedScale + trim.scale;
				i.offset = i.computedOffset + glm::vec3(0, trim.out, trim.lift);
				const auto sane = [](float scale, const glm::vec3& offset) {
					return std::isfinite(scale) && scale >= kMinScale && scale <= kMaxScale &&
						std::isfinite(glm::length(offset)) && glm::length(offset) <= kMaxOffset;
				};
				if (!sane(i.computedScale, i.computedOffset) || !sane(i.scale, i.offset)) i.fitRejection = "scale outside 0.85..1.45 or offset farther than 10 units from head bone";
			}
			// The reviewed hand fit (zoomed front/side/back reviews, October 17) is the reference; the computed fit only
			// refines it. The head-extent reading is not trusted alone: it accepted an Ironforge horned helm at 1.36
			// sitting over the nose with the whole skull above it.
			glm::vec3 handOffset; float handScale;
			{
				const auto& base = kFit[i.female ? 1 : 0];
				handScale = base.scale; handOffset = glm::vec3(0, base.out, base.lift);
				const std::string_view model = i.asset->item->parts[0].model.name;
				for (const auto& a : kAdjust) if (model.find(a.model) != std::string_view::npos) {
					handOffset.z += a.lift; handScale *= i.female ? a.scaleFemale : a.scaleMale;
				}
				if (i.elf && i.asset->hideEars) handScale *= kElfEars;
			}
			constexpr float kRefineScale = .12f, kRefineOffset = 3.f;
			if (i.fitRejection.empty() && (std::fabs(i.computedScale - handScale) > kRefineScale || glm::length(i.computedOffset - handOffset) > kRefineOffset))
				i.fitRejection = std::format("computed fit departs from the reviewed hand fit (scale {:.2f} vs {:.2f}, offset {:.1f} units)",
					i.computedScale, handScale, glm::length(i.computedOffset - handOffset));
			i.autoFit = i.fitRejection.empty();
			if (!i.autoFit) {
				i.scale = handScale + trim.scale; i.offset = handOffset + glm::vec3(0, trim.out, trim.lift);
			}
			if (!(i.scale > 0) || !std::isfinite(i.scale)) return false;
			i.world = i.nodeWorld * glm::translate(glm::mat4(1), i.offset) * glm::scale(glm::mat4(1), glm::vec3(i.scale)) * rotation * glm::rotate(glm::mat4(1), glm::radians(trim.pitch), glm::vec3(0, 1, 0));
			return Finite(i.world);
		}
		OwnedAsset Want(const Character::Item& look, bool female)
		{
			{
				std::lock_guard lock(g_lock);
				if (const auto it = g_assets.find(look.displayId); it != g_assets.end()) return it->second[female ? 1 : 0];
				if (!g_wanted.insert(look.displayId).second) return nullptr;
				g_queue.emplace_back(look.entry, look.displayId);
				if (g_working) return nullptr;
				g_working = true;
			}
			std::thread([root = Config::ClientData()] {
				std::optional<wowcore::Mpq> mpq;
				std::optional<wowcore::Dbc> displays, visibility;
				for (;;) {
					std::pair<std::uint32_t, std::uint32_t> next;
					{
						std::lock_guard lock(g_lock);
						if (g_queue.empty()) { g_working = false; return; }
						next = g_queue.front(); g_queue.pop_front();
					}
					try {
						if (!mpq) {
							mpq.emplace(root);
							displays = wowcore::Dbc::load(*mpq, "DBFilesClient\\ItemDisplayInfo.dbc");
							visibility = wowcore::Dbc::load(*mpq, "DBFilesClient\\HelmetGeosetVisData.dbc");
						}
						if (!displays || !visibility || displays->cols() < 14 || visibility->cols() != 6) {
							logger::warn("NPC helms: display={} missing/invalid visibility DBC", next.second); continue;
						}
						const auto row = displays->rowOf(next.second);
						if (!row) continue;
						std::array<OwnedAsset, 2> assets;
						for (std::uint32_t sex = 0; sex < 2; ++sex) {
							wowcore::Appearance who; who.race = 1; who.sex = sex;
							auto item = std::make_shared<wowcore::ItemPreview>(wowcore::loadItemPreview(*mpq, next.first,
								{ { next.first, { next.second, 1 } } }, who));  // same _HuM/_HuF path as their helm
							if (item->status != wowcore::ItemPreviewStatus::Ready || item->parts.size() != 1) {
								logger::warn("NPC helms: display={} sex={} status={} detail={}", next.second, sex, wowcore::itemPreviewStatusName(item->status), item->detail); continue;
							}
							auto asset = std::make_shared<Asset>(); asset->item = std::move(item);
							const auto vis = wowcore::helmetVisibility(*displays, *visibility, next.second, 1, sex);
							if (!vis) { logger::warn("NPC helms: display={} sex={} invalid visibility row", next.second, sex); continue; }
							asset->visibility = vis->id; asset->hideHair = vis->hair;
							asset->hideBeard = vis->facialHair; asset->hideEars = vis->ears;
							asset->bounds = FitEvidence::ModelBounds(asset->item->parts[0].model);
							// The model family describes the opening; it supplies no per-model calibration values.
							std::string name = asset->item->parts[0].model.name;
							std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
							asset->circlet = name.find("circlet") != std::string::npos;
							asset->openFace = !asset->circlet && (!asset->hideBeard || !asset->hideEars);
							logger::info("NPC helms: display={} sex={} model={} hide hair={} beard={}", next.second, sex,
								asset->item->parts[0].model.name, asset->hideHair, asset->hideBeard);
							assets[sex] = std::move(asset);
						}
						std::lock_guard lock(g_lock); g_assets[next.second] = std::move(assets);
					} catch (const std::exception& e) { logger::warn("NPC helms: display={} load failed: {}", next.second, e.what()); }
				}
			}).detach();
			return nullptr;
		}
		bool Ready(const OwnedAsset& asset)
		{
			std::lock_guard lock(g_lock);
			const auto it = g_ready.find(asset);
			return it != g_ready.end() && std::chrono::steady_clock::now() - it->second < std::chrono::milliseconds(500);
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
					if (distance <= kRadius * kRadius && Worn(actor).look) { nearby.emplace_back(distance, actor->GetHandle()); }
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
			if (nearby.size() > kMaxActors) { nearby.resize(kMaxActors); }
			std::set<RE::FormID> keep;
			for (const auto& [distance, handle] : nearby) if (auto actor = handle.get()) {
				keep.insert(actor->GetFormID()); g_tracked[actor->GetFormID()].handle = handle;
			}
			std::erase_if(g_tracked, [&](auto& row) {
				if (keep.contains(row.first)) return false;
				Unbind(row.second);
				return true;
			});
		}
	}

	std::array<float, 4> SetFit(bool female, const char* which, float value)
	{
		auto& f = g_fit[female ? 1 : 0];
		if (std::isfinite(value) && which) {
			if (std::string_view(which) == "lift") f.lift = value;
			else if (std::string_view(which) == "out") f.out = value;
			else if (std::string_view(which) == "scale") f.scale = value;
			else if (std::string_view(which) == "pitch") f.pitch = value;
		}
		return { f.lift, f.out, f.scale, f.pitch };
	}
	void Reset()
	{
		for (auto& [id, tracked] : g_tracked) Unbind(tracked);
		g_tracked.clear(); g_cell = nullptr; g_nextScan = 0;
		std::lock_guard lock(g_lock); g_instances.clear(); g_draws.clear(); g_ready.clear();
	}
	void SetEnabled(bool enabled) { g_enabled = enabled; Reset(); }
	bool Enabled() { return g_enabled; }
	void Tick()
	{
		if (!g_enabled) return;
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* ui = RE::UI::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		if (!ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME) ||
			!player || !player->Is3DLoaded() || !cell || !cell->IsAttached()) { Reset(); return; }
		if (cell != g_cell) { g_cell = cell; g_nextScan = 0; }
		if (Frame::Now() >= g_nextScan) { Scan(cell, player->GetPosition()); g_nextScan = Frame::Now() + .5; }
		std::vector<Instance> instances;
		const auto gather = ++g_gather;
		std::erase_if(g_tracked, [&](auto& row) {
			auto& tracked = row.second;
			auto actor = tracked.handle.get();
			auto* root = actor ? actor->Get3D(false) : nullptr;
			auto* parent = actor ? actor->GetParentCell() : nullptr;
			if (!root || !parent || !parent->IsAttached() || actor->IsDisabled() || actor->IsDeleted() || actor->IsMarkedForDeletion() ||
				actor->GetPosition().GetSquaredDistance(player->GetPosition()) > kRadius * kRadius) { Unbind(tracked); return true; }
			const auto worn = Worn(actor.get());
			auto* node = worn.look ? root->GetObjectByName("NPC Head [Head]") : nullptr;
			if (!node) { Unbind(tracked); return false; }
			std::vector<RE::BSGeometry*> geometry;
			const auto& biped = actor->GetBiped(false);
			if (biped) for (const auto& object : biped->objects) if (object.item == worn.form && object.partClone) {
				// Buffered clones must not hide the new head's geometry while 3D is being replaced.
				for (auto* p = object.partClone.get(); p; p = p->parent) if (p == root) { Collect(object.partClone.get(), geometry); break; }
			}
			Bind(tracked.helmet, geometry);
			Bind(tracked.hair, HeadParts(actor.get(), RE::BGSHeadPart::HeadPartType::kHair));
			ShowFace(tracked, actor.get());
			// Not the beard: culling the facial-hair part (HumanBeard01Shaved on the guard) took the whole head with it
			// (October 17: empty neck holes under the plate helm, coif and horned helm); beards may show through full
			// helms until the head/beard geometry relationship is understood.
			Bind(tracked.beard, {});
			Instance instance;
			instance.ref = row.first; instance.entry = worn.look->entry; instance.display = worn.look->displayId; instance.gather = gather;
			const auto* base = actor->GetActorBase(); instance.female = base && base->GetSex() == RE::SEX::kFemale;
			const auto* race = actor->GetRace();
			const std::string_view raceID = race ? race->GetFormEditorID() : "";
			instance.elf = raceID.find("Elf") != std::string_view::npos;
			instance.asset = Want(*worn.look, instance.female);
			instance.nodeWorld = Transform(node->world);
			Measure(tracked.head, actor.get(), node);
			if (!Fit(instance, tracked.head)) { Unbind(tracked); return false; }
			const auto report = std::format("{}:{}", instance.display, instance.fitRejection);
			if (tracked.fitReport != report) {
				tracked.fitReport = report;
				if (!instance.autoFit) logger::warn("helm auto-fit rejected: ref={:08X} display={} computed_scale={} computed_offset=({},{},{}) offset_distance={} reason={} fallback_scale={} fallback_offset=({},{},{})",
					instance.ref, instance.display, instance.computedScale, instance.computedOffset.x, instance.computedOffset.y, instance.computedOffset.z,
					glm::length(instance.computedOffset), instance.fitRejection, instance.scale, instance.offset.x, instance.offset.y, instance.offset.z);
			}
			if (auto* face = actor->GetFaceNodeSkinned()) instance.faceBound = face->worldBound;
			const auto faceParts = HeadParts(actor.get(), RE::BGSHeadPart::HeadPartType::kFace);
			if (!faceParts.empty()) instance.headBound = faceParts.front()->worldBound;
			if (biped) for (const auto& object : biped->objects) if (object.item == worn.form && object.partClone) {
				bool attached = false;
				for (auto* p = object.partClone.get(); p; p = p->parent) if (p == root) { attached = true; break; }
				if (attached && object.partClone->worldBound.radius > instance.nativeBound.radius) instance.nativeBound = object.partClone->worldBound;
			}
			const bool ready = instance.asset && !tracked.helmet.geometry.empty() && Ready(instance.asset);
			Hide(tracked.helmet, ready);
			Hide(tracked.hair, ready && instance.asset->hideHair);
			Hide(tracked.beard, ready && instance.asset->hideBeard);
			instance.hidden = tracked.helmet.hidden; instance.hairHidden = tracked.hair.hidden; instance.beardHidden = tracked.beard.hidden;
			instance.hairGeometry = static_cast<std::uint32_t>(tracked.hair.geometry.size());
			instance.beardGeometry = static_cast<std::uint32_t>(tracked.beard.geometry.size());
			// Evidence: which geometries the hair/beard hiding takes (a face vanished under beard-hiding helms, October 17).
			instance.culledNames.clear();
			for (const auto* group : { &tracked.hair, &tracked.beard }) for (const auto& g : group->geometry) {
				std::string name = g.node->name.c_str(); if (g.node->parent) { name += " < "; name += g.node->parent->name.c_str(); }
				instance.culledNames.push_back(std::move(name));
			}
			instances.push_back(std::move(instance));
			return false;
		});
		std::lock_guard lock(g_lock); g_instances = std::move(instances);
	}
	std::vector<Instance> Current() { std::lock_guard lock(g_lock); return g_instances; }
	void RecordReady(const OwnedAsset& asset, bool ready)
	{
		std::lock_guard lock(g_lock);
		if (ready && g_enabled) g_ready[asset] = std::chrono::steady_clock::now(); else g_ready.erase(asset);
	}
	void RecordDraws(std::vector<DrawInfo> draws) { std::lock_guard lock(g_lock); g_draws = std::move(draws); }
	std::vector<DrawInfo> Draws() { std::lock_guard lock(g_lock); return g_draws; }
	nlohmann::json Evidence(const Instance& i)
	{
		using namespace FitEvidence;
		using nlohmann::json;
		// Evaluate the FINAL transformed M2 vertices, not the transformed corners of the fit's model box.
		const auto bounds = i.asset ? ModelBounds(i.asset->item->parts[0].model, i.world) : Bounds{};
		const auto basis = glm::rotate(glm::mat4(1), glm::radians(90.f), glm::vec3(0, 0, 1)) * glm::scale(glm::mat4(1), glm::vec3(64.f));
		const auto interior = TransformBounds(i.interior, glm::inverse(i.nodeWorld) * i.world * glm::inverse(basis));
		const auto headWorld = TransformBounds(i.head, i.nodeWorld);
		const glm::vec3 headCentre(i.headBound.center.x, i.headBound.center.y, i.headBound.center.z);
		const auto centre = (bounds.lower + bounds.upper) * .5f;
		const auto delta = glm::vec3(glm::inverse(i.nodeWorld) * glm::vec4(centre - headCentre, 0));
		const float nodeScale = glm::length(glm::vec3(i.nodeWorld[0]));
		const float radius = i.headBound.radius;
		const bool measured = bounds.valid && radius > 0 && std::isfinite(radius);
		// Independent engine sphere versus actual helm vertex AABB. These are coverage proxies, not shell
		// collision tests; in particular a circlet is not meant to enclose the head sphere.
		const auto penetration = glm::max(glm::max(headCentre + radius - bounds.upper, bounds.lower - (headCentre - radius)), glm::vec3(0));
		const float ears = std::max({ 0.f, headCentre.x + .6f * radius - bounds.upper.x, bounds.lower.x - (headCentre.x - .6f * radius),
			headCentre.y + .6f * radius - bounds.upper.y, bounds.lower.y - (headCentre.y - .6f * radius) });
		const auto nativeRatio = bounds.valid && i.nativeBound.radius > 0 ?
			std::optional(glm::length(bounds.upper - bounds.lower) / (2 * i.nativeBound.radius)) : std::nullopt;
		json out{ { "head_extents", Bound(i.head) }, { "head_limits", { { "front", i.head.upper.y }, { "back", -i.head.lower.y }, { "top", i.head.upper.z }, { "left", -i.head.lower.x }, { "right", i.head.upper.x } } },
			{ "head_world", Bound(headWorld) }, { "head_vertices", i.headVertices }, { "head_frame_verification", i.headVerification },
			{ "forehead", i.forehead }, { "forehead_radius", i.foreheadRadius }, { "brow_measured", i.browMeasured }, { "hair_extents", Bound(i.hair) },
			{ "helm_outer", Bound(i.outer) }, { "helm_interior", Bound(interior) }, { "item_bound", Bound(bounds) }, { "item_px", Pixels(bounds) },
			{ "face_bound", Sphere(i.faceBound) }, { "head_bound", Sphere(i.headBound) }, { "native_bound", Sphere(i.nativeBound) },
			{ "metric_geometry", "kFace engine worldBound sphere / final M2 world-space vertex AABB (estimated interior unused)" },
			{ "kind", i.asset && i.asset->circlet ? "circlet" : i.asset && i.asset->openFace ? "open_face" : "covering" },
			{ "fit_source", i.autoFit ? "automatic" : "8141691 fallback" }, { "auto_fit_attempted", i.autoFitAttempted }, { "auto_fit_rejection", i.fitRejection },
			{ "computed_scale", i.autoFitAttempted ? json(i.computedScale) : json() }, { "scale", i.scale },
			{ "computed_scale_sanity", Range(i.autoFitAttempted ? std::optional(i.computedScale) : std::nullopt, kMinScale, kMaxScale) },
			{ "computed_offset_sanity", Range(i.autoFitAttempted ? std::optional(glm::length(i.computedOffset)) : std::nullopt, 0, kMaxOffset) },
			{ "computed_offset", i.autoFitAttempted ? Vector(i.computedOffset) : json() }, { "offset", Vector(i.offset) },
			{ "margin", i.asset && i.asset->circlet ? .5f : kMargin }, { "thickness", i.asset && i.asset->circlet ? .5f : kThickness },
			{ "penetration_xyz", measured ? Vector(penetration) : json() },
			{ "top_clearance", Range(measured ? std::optional(bounds.upper.z - (headCentre.z + radius)) : std::nullopt, 2, 10) },
			{ "horizontal_offset", Range(measured ? std::optional(glm::length(glm::vec2(delta)) * nodeScale) : std::nullopt, 0, 4) },
			{ "horizontal_forward_side", measured ? json{ delta.y * nodeScale, delta.x * nodeScale } : json() },
			{ "top_penetration", Range(measured ? std::optional(std::max(0.f, headCentre.z + radius - bounds.upper.z)) : std::nullopt, 0, 0) },
			{ "ear_penetration", Range(measured ? std::optional(ears) : std::nullopt, 0, 0) },
			{ "helm_centre_to_head_centre", Range(measured ? std::optional(glm::length(centre - headCentre)) : std::nullopt, 0, 10) },
			{ "helm_scale_vs_native", Range(nativeRatio, .8f, 1.4f) }, { "scale_ratio", nativeRatio ? json(*nativeRatio) : json() } };
		for (const auto* check : { "top_clearance", "top_penetration", "ear_penetration" }) out[check]["applicable"] = !(i.asset && i.asset->circlet);
		return out;
	}

	nlohmann::json Describe(RE::FormID id)
	{
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
		if (!actor) return { { "actor", "missing" } };
		nlohmann::json out{ { "enabled", g_enabled.load() }, { "tracked", g_tracked.contains(id) }, { "wow_race", "Human (all Skyrim races)" } };
		const auto worn = Worn(actor);
		out["form"] = worn.form ? std::format("{:08X}", worn.form->GetFormID()) : "none";
		out["head_node"] = actor->Get3D(false) && actor->Get3D(false)->GetObjectByName("NPC Head [Head]");
		if (const auto tracked = g_tracked.find(id); tracked != g_tracked.end()) {
			const auto& h = tracked->second.head;
			out["head_cpu"] = { { "vertices", h.vertices }, { "brow_measured", h.browMeasured }, { "hair_measured", h.hairMeasured }, { "verified", h.verified }, { "verification", h.verification } };
			if (!h.head.valid) out["fit_unavailable"] = "kFace CPU vertices unavailable (hand-tuned fallback)";
			else if (!h.browMeasured) out["circlet_fit_unavailable"] = "brow CPU vertices unavailable (hand-tuned fallback)";
		}
		if (worn.look) { out["entry"] = worn.look->entry; out["display"] = worn.look->displayId; }
		out["biped"] = nlohmann::json::array();
		if (const auto& biped = actor->GetBiped(false)) for (const auto slot : { RE::BIPED_OBJECT::kHair, RE::BIPED_OBJECT::kCirclet }) {
			const auto& object = biped->objects[static_cast<std::size_t>(slot)];
			out["biped"].push_back({ { "slot", static_cast<int>(slot) + 30 }, { "form", object.item ? object.item->GetFormID() : 0 }, { "clone", object.partClone != nullptr } });
		}
		for (const auto& i : Current()) if (i.ref == id) {
			out["computed_fit"] = i.autoFit; out["fit_source"] = i.autoFit ? "automatic" : "8141691 fallback"; out["metrics"] = Evidence(i);
			out["gather"] = i.gather; out["female"] = i.female; out["hidden"] = i.hidden;
			out["hair_hidden"] = i.hairHidden; out["beard_hidden"] = i.beardHidden;
			out["hair_geometry"] = i.hairGeometry; out["beard_geometry"] = i.beardGeometry; out["culled"] = i.culledNames;
			// Decisive for "is the head there": every geometry under the FaceGen node with its cull flag.
			if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(i.ref); actor && actor->GetFaceNodeSkinned()) {
				nlohmann::json face = nlohmann::json::array();
				RE::BSVisit::TraverseScenegraphGeometries(actor->GetFaceNodeSkinned(), [&](RE::BSGeometry* geo) {
					face.push_back({ { "name", geo->name.c_str() }, { "culled", geo->GetAppCulled() }, { "radius", geo->worldBound.radius } });
					return RE::BSVisit::BSVisitControl::kContinue;
				});
				out["face_geometry"] = face;
				std::string chain;
				for (auto* n = static_cast<RE::NiAVObject*>(actor->GetFaceNodeSkinned()); n; n = n->parent) { chain += n->name.c_str(); chain += n->GetAppCulled() ? "[culled] " : " "; }
				out["face_chain"] = chain;
			}
			out["asset"] = i.asset ? "ready" : "loading or failed (see log)";
			if (i.asset) {
				out["model"] = i.asset->item->parts[0].model.name; out["visibility"] = i.asset->visibility;
				out["hide_hair"] = i.asset->hideHair; out["hide_beard"] = i.asset->hideBeard; out["hide_ears"] = i.asset->hideEars;
			}
		}
		if (!out.contains("computed_fit")) {
			out["computed_fit"] = false;
			if (!out.contains("fit_unavailable")) out["fit_unavailable"] = "helm asset unavailable or final transform invalid (native retained)";
		}
		for (const auto& d : Draws()) if (d.instance.ref == id) {
			out["frame"] = d.frame; out["batches"] = d.batches;
			out["draw_age_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - d.at).count();
		}
		return out;
	}
}
