#include "Portrait.h"

#include <glm/gtc/packing.hpp>

namespace Portrait
{
	namespace
	{
		std::mutex                  g_lock;
		std::array<std::shared_ptr<const Head>, 5> g_heads;
		std::array<RE::ActorHandle, 5> g_pending;   // selected before its head was there
		std::uint32_t               g_serial = 0;

		glm::mat4 Matrix(const RE::NiTransform& a_t)
		{
			glm::mat4 m(1.0f);
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 3; ++c) {
					m[c][r] = a_t.rotate.entry[r][c] * a_t.scale;
				}
			}
			m[3] = glm::vec4(a_t.translate.x, a_t.translate.y, a_t.translate.z, 1.0f);
			return m;
		}

		// One part: its positions (a face's morphed dynamic data, else the partitions' own vertex data - a creature's
		// body) skinned by each partition's bones, its UVs from the partitions' vertex data, its triangles (global vertex
		// indices), and what it is drawn with.
		std::optional<Part> Skin(RE::BSGeometry* a_shape, const glm::mat4& a_toHead)
		{
			auto&       g = a_shape->GetGeometryRuntimeData();
			auto*       dynamicShape = netimmerse_cast<RE::BSDynamicTriShape*>(a_shape);
			auto*       skin = g.skinInstance.get();
			auto*       data = skin ? skin->skinData.get() : nullptr;
			auto*       partitions = skin ? skin->skinPartition.get() : nullptr;
			const auto* property = netimmerse_cast<RE::BSLightingShaderProperty*>(g.shaderProperty.get());
			auto*       material = property ? static_cast<RE::BSLightingShaderMaterialBase*>(property->material) : nullptr;
			if (!data || !partitions || !material || !skin->bones || (dynamicShape && !dynamicShape->GetDynamicTrishapeRuntimeData().dynamicData)) {
				return std::nullopt;
			}
			std::size_t count = partitions->vertexCount;
			if (dynamicShape) {
				count = dynamicShape->GetDynamicTrishapeRuntimeData().dataSize / 16;   // a float4 a vertex
			}
			Part              part;
			part.positions.resize(count);
			part.uvs.resize(count);
			std::vector<bool> done(count);
			{
				std::optional<RE::BSSpinLockGuard> guard;
				if (dynamicShape) {
					guard.emplace(dynamicShape->GetDynamicTrishapeRuntimeData().lock);
				}
				const auto* morphed = dynamicShape ? static_cast<const float*>(dynamicShape->GetDynamicTrishapeRuntimeData().dynamicData) : nullptr;
				for (std::uint32_t p = 0; p < partitions->numPartitions; ++p) {
					const auto& pt = partitions->partitions[p];
					if (!pt.triList || !pt.bones || !pt.buffData || !pt.buffData->rawVertexData) {
						continue;
					}
					std::vector<glm::mat4> palette(pt.numBones);
					for (std::uint16_t b = 0; b < pt.numBones; ++b) {
						const auto bone = pt.bones[b];
						auto*      node = bone < skin->numMatrices ? skin->bones[bone] : nullptr;
						palette[b] = node ? Matrix(node->world) * Matrix(data->GetBoneDataSkinToBone(bone)) : glm::mat4(1.0f);
					}
					const auto  desc = pt.vertexDesc;
					const auto  stride = static_cast<std::size_t>(std::bit_cast<std::uint64_t>(desc) & 0xF) * 4;
					const auto  uvAt = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
					const auto  skinAt = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
					const auto* raw = pt.buffData->rawVertexData;
					for (std::uint32_t k = 0; k < pt.triangles * 3u; ++k) {
						const auto v = pt.triList[k];
						if (v >= count) {
							continue;
						}
						part.indices.push_back(v);
						if (done[v]) {
							continue;
						}
						done[v] = true;
						const auto* vertex = raw + v * stride;
						glm::vec4   at(0.0f, 0.0f, 0.0f, 1.0f);
						if (morphed) {
							at = glm::vec4(morphed[v * 4], morphed[v * 4 + 1], morphed[v * 4 + 2], 1.0f);
						} else if (uvAt >= 16) {   // full floats before the UVs (a skin partition's, whatever the flags say)
							std::memcpy(&at, vertex, 12);
						} else {
							std::uint16_t half[3];
							std::memcpy(half, vertex, 6);
							at = glm::vec4(glm::unpackHalf1x16(half[0]), glm::unpackHalf1x16(half[1]), glm::unpackHalf1x16(half[2]), 1.0f);
						}
						std::uint16_t   uv[2], weights[4];
						std::memcpy(uv, vertex + uvAt, 4);
						std::memcpy(weights, vertex + skinAt, 8);
						const auto*     bones = vertex + skinAt + 8;
						glm::vec4       skinned(0.0f);
						float           total = 0;
						for (int i = 0; i < 4; ++i) {
							const float w = glm::unpackHalf1x16(weights[i]);
							if (w > 0 && bones[i] < palette.size()) {
								skinned += w * (palette[bones[i]] * at);
								total += w;
							}
						}
						// A creature's body stands in its bind pose, placed by its shape (its skin space is not a face's).
						part.positions[v] = glm::vec3(a_toHead * (morphed ? (total > 0 ? skinned / total : at) : Matrix(a_shape->world) * at));
						part.uvs[v] = glm::vec2(glm::unpackHalf1x16(uv[0]), glm::unpackHalf1x16(uv[1]));
					}
				}
			}
			if (part.indices.empty()) {
				return std::nullopt;
			}
			// The head carries no normals (its lighting is a model-space normal map): smooth ones from the triangles.
			part.normals.assign(count, glm::vec3(0.0f));
			for (std::size_t i = 0; i + 2 < part.indices.size(); i += 3) {
				const auto a = part.indices[i], b = part.indices[i + 1], c = part.indices[i + 2];
				const auto n = glm::cross(part.positions[b] - part.positions[a], part.positions[c] - part.positions[a]);
				part.normals[a] += n, part.normals[b] += n, part.normals[c] += n;
			}
			for (auto& n : part.normals) {
				n = glm::length(n) > 0 ? glm::normalize(n) : glm::vec3(0, 1, 0);
			}
			part.diffuse = material->diffuseTexture;
			using Feature = RE::BSShaderMaterial::Feature;
			switch (material->GetFeature()) {
			case Feature::kFaceGen:
				part.tint = static_cast<RE::BSLightingShaderMaterialFacegen*>(material)->tintTexture;
				break;
			case Feature::kHairTint:
				{
					const auto& c = static_cast<RE::BSLightingShaderMaterialHairTint*>(material)->tintColor;
					part.color = glm::vec3(c.red, c.green, c.blue);
					part.alphaKey = true;
					break;
				}
			default:
				break;
			}
			if (const auto* alpha = g.alphaProperty.get(); alpha && alpha->GetAlphaTesting()) {
				part.alphaKey = true;   // a creature's fur cards, a mane
			}
			part.blended = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kDecal,
				RE::BSShaderProperty::EShaderPropertyFlag::kDynamicDecal);   // brows and the like lie on the head
			return part;
		}

		// A creature's head bone: the first node named for a head (Canine_Head, Horse_Head, NPC Head [Head] ...).
		RE::NiAVObject* HeadBone(RE::NiAVObject* a_root)
		{
			RE::NiAVObject* found = nullptr;
			RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
				const std::string_view name = a_object->name.c_str();
				if (name.find("Head") != std::string_view::npos && name.find("Track") == std::string_view::npos && !netimmerse_cast<RE::BSGeometry*>(a_object)) {
					found = a_object;
					return RE::BSVisit::BSVisitControl::kStop;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return found;
		}

		bool Take(RE::Actor* a_actor, std::size_t a_slot)
		{
			auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
			auto* face = root ? root->GetObjectByName("BSFaceGenNiNodeSkinned") : nullptr;
			auto* headNode = root ? root->GetObjectByName("NPC Head [Head]") : nullptr;
			const bool creature = root && !face;   // no face-gen: its body's meshes about its head bone
			if (creature) {
				face = root;
				headNode = HeadBone(root);
			}
			if (!face || !headNode) {
				return false;
			}
			// Head space: at the head, facing where the actor faces.
			const float     h = a_actor->GetHeading(false);
			const glm::vec3 forward(std::sin(h), std::cos(h), 0.0f), up(0, 0, 1), right = glm::cross(forward, up);
			const auto&     at = headNode->world.translate;
			glm::mat4       fromHead(1.0f);
			fromHead[0] = glm::vec4(right, 0), fromHead[1] = glm::vec4(forward, 0), fromHead[2] = glm::vec4(up, 0);
			fromHead[3] = glm::vec4(at.x, at.y, at.z, 1);
			const glm::mat4 toHead = glm::inverse(fromHead);
			auto            head = std::make_shared<Head>();
			RE::BSVisit::TraverseScenegraphGeometries(face, [&](RE::BSGeometry* a_geo) {
				if ((creature || netimmerse_cast<RE::BSDynamicTriShape*>(a_geo)) && !a_geo->GetAppCulled()) {
					if (auto part = Skin(a_geo, toHead)) {
						head->parts.push_back(std::move(*part));
					}
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			if (creature) {   // its head: the triangles within a quarter of its size of the head bone
				glm::vec3 lo(1e9f), hi(-1e9f);
				for (const auto& p : head->parts) {
					for (const auto i : p.indices) {
						lo = glm::min(lo, p.positions[i]), hi = glm::max(hi, p.positions[i]);
					}
				}
				const float reach = 0.25f * glm::length(hi - lo);
				for (auto& p : head->parts) {
					std::vector<std::uint16_t> kept;
					for (std::size_t t = 0; t + 2 < p.indices.size(); t += 3) {
						if (glm::length(p.positions[p.indices[t]]) < reach && glm::length(p.positions[p.indices[t + 1]]) < reach &&
							glm::length(p.positions[p.indices[t + 2]]) < reach) {
							kept.insert(kept.end(), p.indices.begin() + t, p.indices.begin() + t + 3);
						}
					}
					p.indices = std::move(kept);
				}
				std::erase_if(head->parts, [](const Part& a_part) { return a_part.indices.empty(); });
			}
			if (head->parts.empty()) {
				return false;
			}
			glm::vec3   lo(1e9f), hi(-1e9f);
			for (const auto& p : head->parts) {
				for (const auto i : p.indices) {
					lo = glm::min(lo, p.positions[i]), hi = glm::max(hi, p.positions[i]);
				}
			}
			head->centre = (lo + hi) * 0.5f;
			head->radius = glm::length(hi - lo) * 0.5f;
			head->serial = ++g_serial;
			head->actor = a_actor->GetFormID();
			std::lock_guard lock(g_lock);
			g_heads[a_slot] = std::move(head);
			return true;
		}
	}

	std::optional<RE::NiPoint3> HeadPosition(RE::Actor* a_actor)
	{
		auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
		if (!root) return std::nullopt;
		auto* head = root->GetObjectByName("NPC Head [Head]");
		if (!head) head = HeadBone(root);
		return head ? std::optional(head->world.translate) : std::nullopt;
	}

	void Capture(RE::Actor* a_actor, std::size_t a_slot)
	{
		if (a_slot >= g_heads.size()) return;
		g_pending[a_slot] = {};
		{
			std::lock_guard lock(g_lock);
			g_heads[a_slot].reset();
		}
		if (a_actor && !a_actor->IsPlayerRef() && !Take(a_actor, a_slot)) {
			g_pending[a_slot] = a_actor->CreateRefHandle();
		}
	}

	void Update()
	{
		for (std::size_t slot = 0; slot < g_pending.size(); ++slot) {
			if (auto actor = g_pending[slot].get(); actor && Take(actor.get(), slot)) g_pending[slot] = {};
		}
	}

	std::shared_ptr<const Head> Current(std::size_t a_slot)
	{
		std::lock_guard lock(g_lock);
		return a_slot < g_heads.size() ? g_heads[a_slot] : nullptr;
	}
}
