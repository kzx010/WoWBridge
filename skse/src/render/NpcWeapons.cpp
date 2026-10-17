#include "render/NpcWeapons.h"

#include "Armory.h"
#include "Config.h"
#include "Frame.h"
#include "Gear.h"
#include "Picker.h"
#include "wowcore/mpq.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace FitEvidence
{
	void Bounds::Add(const glm::vec3& p)
	{
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return;
		if (!valid) { lower = upper = p; valid = true; }
		else { lower = glm::min(lower, p); upper = glm::max(upper, p); }
	}
	Bounds ModelBounds(const wowcore::Model& model, const glm::mat4& world)
	{
		Bounds b;
		for (const auto& v : model.vertices) b.Add(glm::vec3(world * glm::vec4(v.position, 1)));
		return b;
	}
	Bounds TransformBounds(const Bounds& b, const glm::mat4& world)
	{
		Bounds result;
		if (b.valid) for (int mask = 0; mask < 8; ++mask)
			result.Add(glm::vec3(world * glm::vec4(mask & 1 ? b.upper.x : b.lower.x,
				mask & 2 ? b.upper.y : b.lower.y, mask & 4 ? b.upper.z : b.lower.z, 1)));
		return result;
	}
	nlohmann::json Vector(const glm::vec3& v) { return { v.x, v.y, v.z }; }
	nlohmann::json Bound(const Bounds& b)
	{
		if (!b.valid) return nullptr;
		return { { "min", Vector(b.lower) }, { "max", Vector(b.upper) },
			{ "centre", Vector((b.lower + b.upper) * .5f) }, { "top", b.upper.z },
			{ "radius", glm::length(b.upper - b.lower) * .5f } };
	}
	nlohmann::json Sphere(const RE::NiBound& b)
	{
		if (!(b.radius > 0)) return nullptr;
		return { { "centre", { b.center.x, b.center.y, b.center.z } }, { "radius", b.radius }, { "top", b.center.z + b.radius } };
	}
	nlohmann::json Pixels(const RE::NiBound& b)
	{
		if (!(b.radius > 0)) return nullptr;
		float x = 0, y = 0, rx = 0, ry = 0;
		if (!Picker::WorldToScreen(b.center, x, y) || !Picker::WorldToScreen(b.center + RE::NiPoint3{ 0, 0, b.radius }, rx, ry)) return nullptr;
		return { x, y, std::hypot(rx - x, ry - y) };
	}
	nlohmann::json Pixels(const Bounds& b)
	{
		if (!b.valid) return nullptr;
		const auto c = (b.lower + b.upper) * .5f;
		RE::NiBound sphere; sphere.center = { c.x, c.y, c.z }; sphere.radius = glm::length(b.upper - b.lower) * .5f;
		return Pixels(sphere);
	}
	nlohmann::json Range(std::optional<float> value, float low, float high)
	{
		return { { "value", value ? nlohmann::json(*value) : nlohmann::json() }, { "expected", { low, high } },
			{ "ok", value && std::isfinite(*value) && *value >= low && *value <= high } };
	}
	nlohmann::json Below(std::optional<float> value, float limit)
	{
		auto result = Range(value, 0, limit);
		result["expected"] = { { "lt", limit } };
		result["ok"] = value && std::isfinite(*value) && *value >= 0 && *value < limit;
		return result;
	}
	float Angle(const glm::vec3& axis, const glm::vec3& reference)
	{
		const float lengths = glm::length(axis) * glm::length(reference);
		return lengths > .00001f ? glm::degrees(std::acos(std::clamp(glm::dot(axis, reference) / lengths, -1.f, 1.f))) : std::numeric_limits<float>::quiet_NaN();
	}
	nlohmann::json Weapon(const Bounds& b, const glm::vec3& axis, const glm::vec3& normal,
		const glm::mat4& at, const glm::vec3& forward, const glm::vec3& outward, const glm::vec3& forearm,
		bool shield, bool drawn, bool back, std::optional<float> shoulder, std::optional<float> hip, const RE::NiBound& native)
	{
		using nlohmann::json;
		const auto centre = (b.lower + b.upper) * .5f;
		const float sign = glm::length(outward) > .00001f ? glm::dot(centre - forearm, glm::normalize(outward)) : std::numeric_limits<float>::quiet_NaN();
		const float radius = glm::length(b.upper - b.lower) * .5f;
		json result{ { "item_bound", Bound(b) }, { "item_px", Pixels(b) }, { "long_axis", Vector(glm::normalize(axis)) },
			{ "native_bound", Sphere(native) }, { "scale_ratio", native.radius > 0 && b.valid ? json(radius / native.radius) : json() },
			{ "axis_angle_deg", Below(b.valid ? std::optional(Angle(axis, forward)) : std::nullopt, 15) },
			{ "sheath_above_shoulder", shoulder ? json(at[3].z - *shoulder) : json() },
			{ "sheath_above_hip", hip ? json(at[3].z - *hip) : json() },
			{ "sheath_landmark", back ? "shoulder" : "hip" } };
		const auto landmark = back ? shoulder : hip;
		result["sheath_height"] = Range(!drawn && landmark ? std::optional(at[3].z - *landmark) : std::nullopt, -12, 12);
		result["sheath_height"]["applicable"] = !drawn;
		if (shield) {
			result["shield_normal_deg"] = Below(b.valid ? std::optional(Angle(normal, outward)) : std::nullopt, 25);
			result["shield_outer_side"] = { { "value", std::isfinite(sign) && b.valid ? json(sign) : json() }, { "expected", { { "gt", 0 } } }, { "ok", b.valid && sign > 0 } };
		}
		return result;
	}
}

namespace NpcWeapons
{
	namespace
	{
		struct Geometry { RE::NiPointer<RE::BSGeometry> node; bool culled; };
		struct Slot
		{
			RE::NiPointer<RE::NiAVObject> mesh;
			std::vector<Geometry> geometry;
			bool hidden = false;
		};
		struct Tracked { RE::ActorHandle handle; std::array<Slot, 2> slots; };
		std::map<RE::FormID, Tracked> g_tracked;
		RE::TESObjectCELL* g_cell = nullptr;
		double g_nextScan = 0;
		std::atomic<bool> g_enabled = true;
		std::uint64_t g_gather = 0;
		std::mutex g_lock;
		std::vector<Instance> g_instances;
		std::vector<DrawInfo> g_draws;
		using Asset = std::shared_ptr<const wowcore::ItemPreview>;
		std::map<std::uint32_t, Asset> g_assets;
		std::set<std::uint32_t> g_wanted;  // failed displays are cached too
		struct Request { std::uint32_t entry, display, inventoryType; };
		std::deque<Request> g_queue;
		bool g_working = false;
		std::map<Asset, std::chrono::steady_clock::time_point, std::owner_less<Asset>> g_ready;
		Type g_selected = Type::Sword;
		struct Calibration { float lift, out, scale, roll; };
		// Attachment-1 frame: keep the authored grip at the origin, never centre the model by its bounds.
		// Offsets are Skyrim node-local units; roll is degrees around the authored grip's long (+X) axis.
		// Initial fits need visual review in Skyrim. Each sex has an independently tunable copy.
		constexpr Calibration kFit[] = {
			{ 0, 0, .75f, 0 },  // dagger
			{ 0, 0, .85f, 0 },  // sword
			{ 0, 0, .85f, 0 },  // mace
			{ 0, 0, .85f, 0 },  // axe
			{ 0, 0, .90f, 0 },  // greatsword
			{ 0, 0, .90f, 0 },  // battleaxe
			{ 0, 0, .90f, 0 },  // hammer
			{ 0, 0, .90f, 0 },  // staff
			{ 0, 0, .85f, 0 },  // bow: rigid model, follows the engine bow node (no string deformation)
			{ 0, 0, .85f, 0 },  // crossbow
			{ 0, 0, .85f, 0 }   // shield
		};
		static_assert(std::size(kFit) == static_cast<std::size_t>(Type::Count));
		std::array<std::array<Calibration, std::size(kFit)>, 2> g_fit = [] {
			std::array<std::array<Calibration, std::size(kFit)>, 2> result;
			for (auto& sex : result) std::ranges::copy(kFit, sex.begin());
			return result;
		}();
		glm::mat4 Fit(Type type, bool female)
		{
			const auto& f = g_fit[female ? 1 : 0][static_cast<std::size_t>(type)];
			// Actual client M2 vertices: swords/staves/bows extend along X, their thin axis is Y. The
			// installed SSE longsword/staff extend along Y, thin axis Z. This cyclic basis maps X/Y/Z
			// to Y/Z/X without reflecting the model. Shields keep their authored top (+Z) on native +Y.
			glm::mat4 basis(1);
			basis[0] = type == Type::Shield ? glm::vec4(-1, 0, 0, 0) : glm::vec4(0, 1, 0, 0);
			basis[1] = glm::vec4(0, 0, 1, 0);
			basis[2] = type == Type::Shield ? glm::vec4(0, 1, 0, 0) : glm::vec4(1, 0, 0, 0);
			return glm::translate(glm::mat4(1), glm::vec3(f.out, 0, f.lift)) *
				basis * glm::rotate(glm::mat4(1), glm::radians(f.roll), glm::vec3(1, 0, 0)) *
				glm::scale(glm::mat4(1), glm::vec3(64.f * f.scale));
		}
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
		struct Equipped { RE::TESBoundObject* form = nullptr; const Character::Item* look = nullptr; Type type = Type::Sword; };
		Equipped Worn(RE::Actor* actor, bool shield)
		{
			auto* equipped = actor->GetEquippedObject(shield);
			auto* form = equipped ? equipped->As<RE::TESBoundObject>() : nullptr;
			// Some engine shield equips are represented only by the worn biped slot (the biped's own row: GetWornArmor
			// walks the inventory every call, too dear per actor per frame).
			if (shield && (!form || !form->IsArmor())) {
				const auto& biped = actor->GetBiped(false);
				auto* item = biped ? biped->objects[static_cast<std::size_t>(RE::BIPED_OBJECT::kShield)].item : nullptr;
				form = item ? item->As<RE::TESBoundObject>() : nullptr;
			}
			if (!form || (shield ? !form->IsArmor() : !form->IsWeapon())) return {};
			const Character::Item* look = nullptr;
			if (const auto* item = Armory::Find(form)) look = &item->look;
			else if (const auto it = Character::Gear().find(Gear::EntryOf(form)); it != Character::Gear().end()) look = &it->second;
			if (!look || !look->displayId) return {};
			if (shield) return look->inventoryType == 14 ? Equipped{ form, look, Type::Shield } : Equipped{};
			switch (look->inventoryType) {
			case 13: case 15: case 17: case 21: case 22: case 25: case 26: break;
			default: return {};
			}
			// Use Skyrim's actual weapon kind for the attachment fit (WoW staves are subclass 10, invtype 17).
			const auto kind = form->As<RE::TESObjectWEAP>()->GetWeaponType();
			Type type;
			switch (kind) {
			case RE::WEAPON_TYPE::kOneHandDagger: type = Type::Dagger; break;
			case RE::WEAPON_TYPE::kOneHandSword: type = Type::Sword; break;
			case RE::WEAPON_TYPE::kOneHandAxe: type = Type::Axe; break;
			case RE::WEAPON_TYPE::kOneHandMace: type = Type::Mace; break;
			case RE::WEAPON_TYPE::kTwoHandSword: type = Type::Greatsword; break;
			// The Armory builds WoW staves (subclass 10) as two-handed axes: Skyrim's staff kind is a caster's item.
			case RE::WEAPON_TYPE::kTwoHandAxe: type = look->subclass == 10 ? Type::Staff : look->subclass == 5 ? Type::Hammer : Type::Battleaxe; break;
			case RE::WEAPON_TYPE::kStaff: type = Type::Staff; break;
			case RE::WEAPON_TYPE::kBow: type = Type::Bow; break;
			case RE::WEAPON_TYPE::kCrossbow: type = Type::Crossbow; break;
			default: return {};
			}
			return { form, look, type };
		}
		void Restore(Slot& slot)
		{
			if (slot.hidden) for (auto& g : slot.geometry) g.node->SetAppCulled(g.culled);
			slot.hidden = false;
		}
		void Unbind(Slot& slot) { Restore(slot); slot.geometry.clear(); slot.mesh.reset(); }
		RE::NiAVObject* Socket(RE::NiAVObject* mesh, RE::NiAVObject* root, bool drawn, bool shield);
		// Every clone the biped keeps for the item: the engine can hold one at the skeleton root (a staff built as a
		// two-hander, October 16) beside the one in the hand, so the gather takes the clone that has a socket.
		std::vector<RE::NiAVObject*> Clones(RE::Actor* actor, RE::TESBoundObject* form)
		{
			std::vector<RE::NiAVObject*> clones;
			const auto& biped = actor->GetBiped(false);
			if (biped) for (const auto& object : biped->objects) if (object.item == form && object.partClone) clones.push_back(object.partClone.get());
			return clones;
		}
		RE::NiAVObject* Mesh(RE::Actor* actor, RE::TESBoundObject* form, RE::NiAVObject* root, bool drawn, bool shield)
		{
			RE::NiAVObject* first = nullptr;
			for (auto* clone : Clones(actor, form)) {
				if (!first) first = clone;
				if (root && Socket(clone, root, drawn, shield)) return clone;
			}
			return first;
		}
		RE::NiAVObject* Socket(RE::NiAVObject* mesh, RE::NiAVObject* root, bool drawn, bool shield)
		{
			// The clone's actual ancestry resolves duplicate names and draw/sheath transition frames.
			// Bows normally move to the left hand while drawn; never force them onto the right-hand socket.
			RE::NiAVObject* socket = nullptr;
			for (RE::NiAVObject* node = mesh->parent; node; node = node->parent) {
				if (node == root) return socket;  // buffered/detached biped clones must not produce ghost weapons
				if (socket) continue;
				const std::string_view name = node->name.c_str();
				if (name == "WEAPON" || name == "SHIELD" || name == "WeaponSword" || name == "WeaponDagger" ||
					name == "WeaponAxe" || name == "WeaponMace" || name == "WeaponBack" || name == "WeaponBow" ||
					name == "WeaponStaff" || name == "WeaponCrossbow") socket = node;
				if (drawn && (name == (shield ? "NPC L Forearm [LLar]" : "NPC R Hand [RHnd]") ||
					name == "NPC L Hand [LHnd]")) socket = node;
			}
			return nullptr;  // no invented position fallback on unsupported skeletons
		}
		// The engine parents a clone by its NIF's Prn node; a name the skeleton lacks leaves the clone at the root (the
		// Armory's staves: a warhammer wearing Staff01.nif, October 16). The WoW model then takes the engine's own
		// socket for the kind, where the weapon belongs.
		RE::NiAVObject* NamedSocket(RE::NiAVObject* root, Type type, bool drawn, bool shield)
		{
			const char* name = shield ? "SHIELD" : drawn ? "WEAPON" :
				type == Type::Dagger ? "WeaponDagger" : type == Type::Sword ? "WeaponSword" : type == Type::Axe ? "WeaponAxe" :
				type == Type::Mace ? "WeaponMace" : type == Type::Bow || type == Type::Crossbow ? "WeaponBow" : "WeaponBack";
			return root ? root->GetObjectByName(name) : nullptr;
		}
		Asset Want(const Character::Item& look)
		{
			{
				std::lock_guard lock(g_lock);
				if (const auto it = g_assets.find(look.displayId); it != g_assets.end()) return it->second;
				if (!g_wanted.insert(look.displayId).second) return nullptr;
				g_queue.push_back({ look.entry, look.displayId, look.inventoryType });
				if (g_working) return nullptr;
				g_working = true;
			}
			std::thread([root = Config::ClientData()] {
				std::optional<wowcore::Mpq> mpq;
				for (;;) {
					Request next;
					{
						std::lock_guard lock(g_lock);
						if (g_queue.empty()) { g_working = false; return; }
						next = g_queue.front(); g_queue.pop_front();
					}
					try {
						if (!mpq) mpq.emplace(root);
						auto item = std::make_shared<wowcore::ItemPreview>(wowcore::loadItemPreview(*mpq, next.entry,
							{ { next.entry, { next.display, next.inventoryType } } }));
						if (item->status != wowcore::ItemPreviewStatus::Ready || item->parts.empty()) {
							logger::warn("NPC weapons: display={} status={} detail={}", next.display, wowcore::itemPreviewStatusName(item->status), item->detail);
							continue;
						}
						logger::info("NPC weapons: display={} loaded {} models", next.display, item->parts.size());
						std::lock_guard lock(g_lock); g_assets[next.display] = std::move(item);
					} catch (const std::exception& e) { logger::warn("NPC weapons: display={} load failed: {}", next.display, e.what()); }
				}
			}).detach();
			return nullptr;
		}
		bool Ready(const Asset& asset)
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
					if (distance <= kRadius * kRadius && (Worn(actor, false).look || Worn(actor, true).look)) { nearby.emplace_back(distance, actor->GetHandle()); }
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
				for (auto& slot : row.second.slots) Restore(slot);
				return true;
			});
		}
	}

	const char* TypeName(Type type)
	{
		constexpr const char* names[] = { "dagger", "sword", "mace", "axe", "greatsword", "battleaxe", "hammer", "staff", "bow", "crossbow", "shield" };
		return names[static_cast<std::size_t>(type)];
	}
	bool SelectType(std::string_view type)
	{
		for (std::size_t i = 0; i < std::size(kFit); ++i) if (type == TypeName(static_cast<Type>(i))) { g_selected = static_cast<Type>(i); return true; }
		return false;
	}
	Type SelectedType() { return g_selected; }
	std::array<float, 4> SetFit(bool female, const char* which, float value)
	{
		auto& f = g_fit[female ? 1 : 0][static_cast<std::size_t>(g_selected)];
		if (std::isfinite(value) && which) {
			if (std::string_view(which) == "lift") f.lift = value;
			else if (std::string_view(which) == "out") f.out = value;
			else if (std::string_view(which) == "scale" && value > 0) f.scale = value;
			else if (std::string_view(which) == "roll") f.roll = value;
		}
		return { f.lift, f.out, f.scale, f.roll };
	}
	void Reset()
	{
		for (auto& [id, actor] : g_tracked) for (auto& slot : actor.slots) Unbind(slot);
		g_tracked.clear(); g_cell = nullptr; g_nextScan = 0;
		std::lock_guard lock(g_lock);
		g_instances.clear(); g_draws.clear(); g_ready.clear();
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
		if (cell != g_cell) { g_cell = cell; g_nextScan = 0; }   // rescan now; the rows evict themselves (a reset would pop the native meshes for a frame)
		if (Frame::Now() >= g_nextScan) { Scan(cell, player->GetPosition()); g_nextScan = Frame::Now() + .5; }
		std::vector<Instance> instances;
		const auto gather = ++g_gather;
		std::erase_if(g_tracked, [&](auto& row) {
			auto& tracked = row.second;
			auto actor = tracked.handle.get();
			auto* root = actor ? actor->Get3D(false) : nullptr;
			auto* parent = actor ? actor->GetParentCell() : nullptr;
			if (!root || !parent || !parent->IsAttached() || actor->IsDisabled() || actor->IsDeleted() || actor->IsMarkedForDeletion() ||
				actor->GetPosition().GetSquaredDistance(player->GetPosition()) > kRadius * kRadius) {
				for (auto& slot : tracked.slots) Unbind(slot);
				return true;
			}
			const bool drawn = actor->AsActorState()->IsWeaponDrawn();
			for (std::size_t side = 0; side < 2; ++side) {
				auto& slot = tracked.slots[side];
				const auto worn = Worn(actor.get(), side == 1);
				auto* mesh = worn.look ? Mesh(actor.get(), worn.form, root, drawn, side == 1) : nullptr;
				auto* node = mesh ? Socket(mesh, root, drawn, side == 1) : nullptr;
				const bool named = mesh && !node && mesh->parent == root;
				if (named) node = NamedSocket(root, worn.type, drawn, side == 1);
				if (!node) { Unbind(slot); continue; }
				if (slot.mesh.get() != mesh) {
					Unbind(slot); slot.mesh.reset(mesh);
					RE::BSVisit::TraverseScenegraphGeometries(mesh, [&](RE::BSGeometry* geo) {
						slot.geometry.push_back({ RE::NiPointer<RE::BSGeometry>(geo), geo->GetAppCulled() });
						return RE::BSVisit::BSVisitControl::kContinue;
					});
				}
				if (slot.geometry.empty()) { Unbind(slot); continue; }
				Instance instance;
				instance.ref = row.first; instance.entry = worn.look->entry; instance.display = worn.look->displayId;
				instance.gather = gather; instance.type = worn.type; instance.drawn = drawn; instance.node = node->name.c_str();
				if (named) instance.node += " (named)";
				const auto* base = actor->GetActorBase();
				instance.female = base && base->GetSex() == RE::SEX::kFemale;
				instance.nativeBound = mesh->worldBound;
				const auto* arm = root->GetObjectByName(side ? "NPC L Forearm [LLar]" : "NPC R Forearm [RLar]");
				if (arm) { instance.outward = glm::vec3(Transform(arm->world)[0]); instance.forearm = glm::vec3(Transform(arm->world)[3]); }
				if (const auto* n = root->GetObjectByName("NPC R Clavicle [RClv]")) instance.shoulder = n->world.translate.z;
				if (const auto* n = root->GetObjectByName("NPC Pelvis [Pelv]")) instance.hip = n->world.translate.z;
				instance.nodeWorld = Transform(node->world); instance.world = instance.nodeWorld * Fit(instance.type, instance.female);
				if (!Finite(instance.world)) { Restore(slot); continue; }
				instance.asset = Want(*worn.look);
				if (instance.asset && Ready(instance.asset)) {
					for (auto& geo : slot.geometry) geo.node->SetAppCulled(true);
					slot.hidden = true;
				} else Restore(slot);
				instance.hidden = slot.hidden;
				instances.push_back(std::move(instance));
			}
			return false;
		});
		std::lock_guard lock(g_lock); g_instances = std::move(instances);
	}
	std::vector<Instance> Current() { std::lock_guard lock(g_lock); return g_instances; }
	void RecordReady(const Asset& asset, bool ready)
	{
		std::lock_guard lock(g_lock);
		if (ready && g_enabled) g_ready[asset] = std::chrono::steady_clock::now();
		else g_ready.erase(asset);
	}
	void RecordDraws(std::vector<DrawInfo> draws) { std::lock_guard lock(g_lock); g_draws = std::move(draws); }
	std::vector<DrawInfo> Draws() { std::lock_guard lock(g_lock); return g_draws; }
	nlohmann::json Evidence(const Instance& i)
	{
		FitEvidence::Bounds bounds;
		if (i.asset) for (const auto& part : i.asset->parts) {
			const auto b = FitEvidence::TransformBounds(FitEvidence::ModelBounds(part.model), i.world);
			if (b.valid) { bounds.Add(b.lower); bounds.Add(b.upper); }
		}
		return FitEvidence::Weapon(bounds, glm::vec3(i.world[0]), glm::vec3(i.world[1]), i.nodeWorld,
			glm::vec3(i.nodeWorld[1]), i.outward, i.forearm, i.type == Type::Shield, i.drawn,
			i.type == Type::Greatsword || i.type == Type::Battleaxe || i.type == Type::Hammer || i.type == Type::Staff || i.type == Type::Bow,
			i.shoulder, i.hip, i.nativeBound);
	}
	nlohmann::json Describe(RE::FormID a_actor)
	{
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor);
		if (!actor) return { { "actor", "missing" } };
		nlohmann::json out{ { "drawn", actor->AsActorState()->IsWeaponDrawn() }, { "tracked", g_tracked.contains(a_actor) }, { "enabled", g_enabled.load() } };
		auto* root = actor->Get3D(false);
		for (int side = 0; side < 2; ++side) {
			nlohmann::json row;
			auto* equipped = actor->GetEquippedObject(side == 1);
			auto* form = equipped ? equipped->As<RE::TESBoundObject>() : nullptr;
			row["equipped"] = form ? std::format("{:08X}", form->GetFormID()) : "none";
			if (form) {
				row["weapon"] = form->IsWeapon(); row["armor"] = form->IsArmor();
				if (const auto* weapon = form->As<RE::TESObjectWEAP>()) row["kind"] = static_cast<int>(weapon->GetWeaponType());
				if (const auto* item = Armory::Find(form)) row["armory"] = { { "entry", item->look.entry }, { "display", item->look.displayId }, { "type", item->look.inventoryType }, { "subclass", item->look.subclass } };
				else row["armory"] = "none";
				const auto worn = Worn(actor, side == 1);
				row["worn"] = worn.look ? TypeName(worn.type) : "rejected";
				const bool drawn = actor->AsActorState()->IsWeaponDrawn();
				auto* mesh = worn.look ? Mesh(actor, worn.form, root, drawn, side == 1) : nullptr;
				row["mesh"] = mesh != nullptr;
				if (worn.look) {
					nlohmann::json clones = nlohmann::json::array();
					for (auto* clone : Clones(actor, worn.form)) {
						std::string chain;
						for (auto* node = clone->parent; node && chain.size() < 160; node = node->parent) { chain += node->name.c_str(); chain += '/'; if (node == root) break; }
						auto* socket = root ? Socket(clone, root, drawn, side == 1) : nullptr;
						clones.push_back({ { "parents", chain }, { "socket", socket ? socket->name.c_str() : "none" }, { "chosen", clone == mesh } });
					}
					row["clones"] = clones;
				}
			}
			out[side ? "left" : "right"] = row;
		}
		out["items"] = nlohmann::json::array();
		for (const auto& i : Current()) if (i.ref == a_actor) out["items"].push_back({ { "entry", i.entry }, { "drawn", i.drawn }, { "metrics", Evidence(i) } });
		return out;
	}
}
