#include "render/PlayerWeapons.h"

#include "Armory.h"
#include "Avatar.h"
#include "Character.h"
#include "Gear.h"
#include "Surrogate.h"
#include "render/WowModels.h"
#include "render/NpcWeapons.h"
#include "wowcore/character.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <set>
#include <sstream>

namespace PlayerWeapons
{
	namespace
	{
		enum class Type { Dagger, Sword, Mace, Axe, Greatsword, Battleaxe, Hammer, Staff, Bow, Crossbow, Shield };
		constexpr const char* kNames[] = { "dagger", "sword", "mace", "axe", "greatsword", "battleaxe", "hammer", "staff", "bow", "crossbow", "shield" };
		struct WeaponStyle { std::uint32_t itemClass, subclass, inventoryType; };
		constexpr WeaponStyle kStyles[] = {
			{ 2, 15, 13 }, { 2, 7, 13 }, { 2, 4, 13 }, { 2, 0, 13 }, { 2, 8, 17 },
			{ 2, 1, 17 }, { 2, 5, 17 }, { 2, 10, 17 }, { 2, 2, 15 }, { 2, 18, 26 }, { 4, 6, 14 }
		};
		static_assert(std::size(kStyles) == std::size(kNames));
		struct Calibration { float lift, out, scale, roll, pitch, drop = 0; };   // pitch: degrees about the attachment's Y; drop: Skyrim units straight down (world)
		// Skyrim's authored grip, not its bounds' centre. Native meshes already use Skyrim units.
		// Initial fits need in-game visual review; each kind can be tuned without affecting WoW items.
		constexpr Calibration kFit[] = {
			{ 0, 0, 1, 0, -15 }, { 0, 0, 1, 0, -15 }, { 0, 0, 1, 0, -15 }, { 0, 0, 1, 0, -15 },   // one-handers pitched down: the sword came up out of the hand (the user, Oct 17)
			{ 0, 0, 1, 0, 0 }, { 0, 0, 1, 0, 0 }, { 0, 0, 1, 0, 0 }, { 0, 0, 1, 0, 0 },
			{ 0, 0, 1, 0, 0, 12 }, { 0, 0, 1, 0, 0 }, { 0, 0, 1, 0, 0 }   // bow: 12 units down (it sat high; 'out' along the attachment did not move it, October 17)
		};
		static_assert(std::size(kFit) == std::size(kNames));
		std::array<Calibration, std::size(kFit)> g_fit = [] {
			std::array<Calibration, std::size(kFit)> fit;
			std::ranges::copy(kFit, fit.begin()); return fit;
		}();
		std::size_t g_selected = 1;
		bool g_enabled = false;   // opt in with wb playerweapons on; visual acceptance is run separately
		bool g_glow = true, g_stance = true, g_frontView = true;
		struct Slot
		{
			RE::NiPointer<RE::NiAVObject> mesh;
			RE::NiPointer<RE::NiNode> parent;
			RE::NiTransform local;
			RE::NiBound nativeBound{};
			RE::FormID form = 0;
			Type type = Type::Sword;
			std::uint32_t point = 0;
			bool drawn = false;
			std::uint64_t frame = 0;
			std::vector<RE::NiPointer<RE::BSGeometry>> geometry;
			glm::mat4 attachment{ 1 };
			float tickYaw = 0;   // our node's yaw when the local was derived (evidence against the engine's later update)
			RE::NiPointer<RE::ShaderReferenceEffect> effect;
			std::chrono::steady_clock::time_point glowAttempt{};
		};
		std::array<Slot, 2> g_slots;
		nlohmann::json g_referenceReview = nlohmann::json::object();
		RE::NiPointer<RE::NiAVObject> g_root;
		std::set<const RE::BSGeometry*> g_visible;
		RE::NiPointer<RE::NiNode> g_node;

		bool Finite(const glm::mat4& m)
		{
			for (int col = 0; col < 4; ++col) for (int row = 0; row < 4; ++row)
				if (!std::isfinite(m[col][row])) return false;
			return true;
		}
		glm::mat4 Matrix(const RE::NiTransform& t)
		{
			glm::mat4 m(1);
			for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) m[col][row] = t.rotate.entry[row][col] * t.scale;
			m[3] = glm::vec4(t.translate.x, t.translate.y, t.translate.z, 1);
			return m;
		}
		RE::NiTransform Transform(const glm::mat4& m)
		{
			RE::NiTransform t;
			t.scale = glm::length(glm::vec3(m[0]));
			for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) t.rotate.entry[row][col] = m[col][row] / t.scale;
			t.translate = { m[3].x, m[3].y, m[3].z };
			return t;
		}
		glm::mat4 Fit(Type type)
		{
			const auto& f = g_fit[static_cast<std::size_t>(type)];
			// Inverse of NpcWeapons' grip basis: Skyrim +Y long / +Z thin -> WoW +X long / +Y thin.
			// Shields: native +Y top -> attachment +Z. Both bases preserve handedness.
			glm::mat4 basis(1);
			// Weapons: the NIF's blade axis (+Y, grip at the origin) runs along the attachment's -X: with +X the
			// sheathed greatsword stood blade-up over their shoulder and the hip sword pointed up into the player (October 17);
			// Skyrim's +X goes to -Z to keep the handedness. Shields keep their basis.
			// Shields: turned half a turn about their top axis - they sat under the inner forearm facing the body
			// (the user, October 17); now on the outer forearm, face away.
			basis[0] = type == Type::Shield ? glm::vec4(1, 0, 0, 0) : glm::vec4(0, 0, -1, 0);
			basis[1] = type == Type::Shield ? glm::vec4(0, 0, 1, 0) : glm::vec4(-1, 0, 0, 0);
			basis[2] = type == Type::Shield ? glm::vec4(0, -1, 0, 0) : glm::vec4(0, 1, 0, 0);
			return glm::translate(glm::mat4(1), glm::vec3(f.out, 0, f.lift) / Avatar::kUnitsPerYard) *
				glm::rotate(glm::mat4(1), glm::radians(f.pitch), glm::vec3(0, 1, 0)) *
				glm::rotate(glm::mat4(1), glm::radians(f.roll), glm::vec3(1, 0, 0)) * basis *
				glm::scale(glm::mat4(1), glm::vec3(f.scale / Avatar::kUnitsPerYard));
		}
		void StopGlow(Slot& slot)
		{
			// The same stop signal as ProcessLists::StopAllMagicEffects, restricted to our own effect.
			// The engine detaches its shader data/particles on the next temp-effect update.
			if (slot.effect) slot.effect->finished = true;
			slot.effect.reset();
		}
		void Release(Slot& slot)
		{
			StopGlow(slot);
			if (!slot.mesh) return;
			for (auto& geo : slot.geometry) {
				g_visible.erase(geo.get()); geo->SetAppCulled(Surrogate::BodyHidden());
			}
			if (slot.mesh->parent) slot.mesh->parent->DetachChild(slot.mesh.get());
			if (slot.parent) slot.parent->AttachChild(slot.mesh.get(), true);
			slot.mesh->local = slot.local;
			RE::NiUpdateData update{}; slot.mesh->Update(update);
			slot = {};
		}
		bool WowItem(RE::TESBoundObject* form)
		{
			if (const auto* item = Armory::Find(form); item && item->look.displayId) return true;
			const auto it = Character::Gear().find(Gear::EntryOf(form));
			return it != Character::Gear().end() && it->second.displayId;
		}
		std::optional<Type> Kind(RE::TESBoundObject* form)
		{
			if (const auto* armor = form->As<RE::TESObjectARMO>(); armor && armor->IsShield()) return Type::Shield;
			const auto* weapon = form->As<RE::TESObjectWEAP>();
			if (!weapon) return {};
			switch (weapon->GetWeaponType()) {
			case RE::WEAPON_TYPE::kOneHandDagger: return Type::Dagger;
			case RE::WEAPON_TYPE::kOneHandSword: return Type::Sword;
			case RE::WEAPON_TYPE::kOneHandMace: return Type::Mace;
			case RE::WEAPON_TYPE::kOneHandAxe: return Type::Axe;
			case RE::WEAPON_TYPE::kTwoHandSword: return Type::Greatsword;
			case RE::WEAPON_TYPE::kTwoHandAxe: return weapon->HasKeywordString("WeapTypeWarhammer") ? Type::Hammer : Type::Battleaxe;
			case RE::WEAPON_TYPE::kStaff: return Type::Staff;
			case RE::WEAPON_TYPE::kBow: return Type::Bow;
			case RE::WEAPON_TYPE::kCrossbow: return Type::Crossbow;
			default: return Type::Sword;  // custom/unarmed WEAPs still use their authored weapon-node grip
			}
		}
		RE::TESEffectShader* EnchantmentShader(RE::PlayerCharacter* player, RE::TESBoundObject* form, bool left)
		{
			auto* weapon = form->As<RE::TESObjectWEAP>();
			if (!weapon) return nullptr;
			auto* entry = player->GetEquippedEntryData(left);
			auto* enchantment = entry && entry->object == form ? entry->GetEnchantment() : weapon->formEnchanting;
			if (!enchantment) enchantment = weapon->formEnchanting;
			if (!enchantment) return nullptr;
			// enchantShader is the weapon visual; effectShader is the magic effect's hit shader in CommonLib.
			for (const auto* effect : enchantment->effects) if (effect && effect->baseEffect) {
				const auto& data = effect->baseEffect->data;
				if (data.enchantShader) return data.enchantShader;
			}
			for (const auto* effect : enchantment->effects) if (effect && effect->baseEffect && effect->baseEffect->data.effectShader)
				return effect->baseEffect->data.effectShader;
			return nullptr;
		}
		void Glow(Slot& slot, RE::PlayerCharacter* player, RE::TESBoundObject* form, bool left)
		{
			auto* shader = g_glow && slot.drawn ? EnchantmentShader(player, form, left) : nullptr;
			if (slot.effect && (slot.effect->finished || slot.effect->effectData != shader)) StopGlow(slot);
			const auto now = std::chrono::steady_clock::now();
			if (!shader || slot.effect || now - slot.glowAttempt < std::chrono::seconds(1)) return;
			slot.glowAttempt = now;
			// Apply after re-parenting: the explicit attach node confines the engine effect to this clone. On 1.7.104
			// the engine returns a bool here, not the ShaderReferenceEffect* CommonLib declares (it returned 1 and
			// NiPointer::reset dereferenced it, October 17): the effect is found in ProcessLists instead.
			std::set<const RE::ShaderReferenceEffect*> before;
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) return;
			lists->ForEachShaderEffect([&](RE::ShaderReferenceEffect* e) { before.insert(e); return RE::BSContainer::ForEachResult::kContinue; });
			(void)player->ApplyEffectShader(shader, -1.f, nullptr, false, false, slot.mesh.get());
			const auto handle = player->GetHandle();
			lists->ForEachShaderEffect([&](RE::ShaderReferenceEffect* e) {
				if (e && !before.contains(e) && e->effectData == shader && e->target == handle && !e->finished) { slot.effect.reset(e); return RE::BSContainer::ForEachResult::kStop; }
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		RE::NiAVObject* Socket(RE::NiAVObject* mesh, RE::NiAVObject* root, bool left, bool drawn)
		{
			RE::NiAVObject* socket = nullptr;
			for (auto* node = mesh->parent; node; node = node->parent) {
				if (node == root) return socket;
				const std::string_view name = node->name.c_str();
				if (drawn && ((left && name == "NPC R Hand [RHnd]") || (!left && name == "NPC L Hand [LHnd]"))) return nullptr;
				if (!socket && (name == "WEAPON" || name == "SHIELD" || name == "WeaponSword" || name == "WeaponDagger" ||
					name == "WeaponAxe" || name == "WeaponMace" || name == "WeaponBack" || name == "WeaponBow" ||
					name == "WeaponStaff" || name == "WeaponCrossbow")) socket = node;
				if (drawn && name == (left ? "NPC L Hand [LHnd]" : "NPC R Hand [RHnd]")) socket = node;
			}
			return nullptr;  // detached/root-buffered clones must not become ghost weapons
		}
		RE::NiAVObject* Mesh(RE::PlayerCharacter* player, RE::TESBoundObject* form, bool left, bool drawn,
			RE::NiAVObject* other)
		{
			const auto& biped = player->GetBiped(false);
			if (biped) for (const auto& object : biped->objects)
				if (object.item == form && object.partClone && object.partClone.get() != other &&
					(object.partClone->parent == g_node.get() || Socket(object.partClone.get(), g_root.get(), left, drawn))) return object.partClone.get();
			return nullptr;
		}
	}

	bool KeepsVisible(const RE::BSGeometry* geometry) { return g_visible.contains(geometry); }
	bool Enabled() { return g_enabled; }
	bool FrontViewEnabled() { return g_frontView; }
	std::optional<std::uint32_t> ReadyStance()
	{
		if (!g_enabled || !g_stance) return {};
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) return {};
		// The main hand decides, as for WoW gear. A shield stays in the off hand; it has no melee stance.
		for (const bool left : { false, true }) {
			auto* equipped = player->GetEquippedObject(left);
			auto* form = equipped ? equipped->As<RE::TESBoundObject>() : nullptr;
			if (!form) continue;
			if (WowItem(form)) return {};
			const auto type = Kind(form);
			if (!type || *type == Type::Shield) continue;
			const auto& style = kStyles[static_cast<std::size_t>(*type)];
			return wowcore::meleeAnims(style.itemClass, style.subclass, style.inventoryType).ready;
		}
		return {};
	}
	std::uint32_t NativeSlots()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) return 0;
		std::uint32_t slots = 0;
		for (const bool left : { false, true }) {
			auto* equipped = player->GetEquippedObject(left);
			auto* form = equipped ? equipped->As<RE::TESBoundObject>() : nullptr;
			if (left && (!form || !form->IsWeapon())) form = player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kShield);
			const auto type = form && !WowItem(form) ? Kind(form) : std::nullopt;
			if (!type) continue;
			slots |= 1u << (left ? 16 : 15);
			if (*type == Type::Bow || *type == Type::Crossbow) slots |= 1u << 17;
		}
		return slots;
	}
	void Reset()
	{
		for (auto& slot : g_slots) Release(slot);
		g_visible.clear();
		if (g_node && g_node->parent) g_node->parent->DetachChild(g_node.get());
		g_node.reset(); g_root.reset();
	}
	void Tick()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* root = player ? player->Get3D(false) : nullptr;
		auto* ui = RE::UI::GetSingleton();
		const auto pose = WowModels::LastAttachments();
		if (!g_enabled || !root || !Surrogate::BodyHidden() || !Avatar::Current().visible || !pose.body ||
			pose.body != Avatar::Data() || !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME) ||
			std::chrono::steady_clock::now() - pose.at > std::chrono::milliseconds(250)) { Reset(); return; }
		if (g_root.get() != root) {
			Reset();
			auto* parent = root->AsNode();
			if (!parent) return;
			g_root.reset(root); g_node.reset(RE::NiNode::Create(2));
			g_node->name = "WBPlayerWeapons";
			parent->AttachChild(g_node.get(), true);
		}
		RE::NiUpdateData update{};  // flags 0: transforms/bounds without advancing controllers
		// Our node's world as the engine's last full update left it (a frame old): forcing it here, inside
		// PlayerCharacter::Update, read the root mid-update and placed the clone mirrored through the player (October 16).
		if (!std::isfinite(g_node->world.scale) || std::abs(g_node->world.scale) < .00001f) return;   // not updated yet
		for (std::size_t side = 0; side < g_slots.size(); ++side) {
			auto& slot = g_slots[side];
			auto* equipped = player->GetEquippedObject(side == 1);
			auto* form = equipped ? equipped->As<RE::TESBoundObject>() : nullptr;
			if (side == 1 && (!form || !form->IsWeapon()))
				form = player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kShield);
			const auto type = form && !WowItem(form) ? Kind(form) : std::nullopt;
			if (!type) { Release(slot); continue; }
			const bool ranged = *type == Type::Bow || *type == Type::Crossbow;
			if (side == 1 && form == player->GetEquippedObject(false) &&
				(ranged || *type == Type::Greatsword || *type == Type::Battleaxe || *type == Type::Hammer)) { Release(slot); continue; }
			const bool left = side == 1 || ranged;
			// Same slot/sheath rule as dressCharacter/placeAttached: 26 back, 27 staff, 28 shield, 32/33 hips.
			const auto sheath = *type == Type::Shield ? 28u : *type == Type::Staff ? 27u :
				ranged || *type == Type::Greatsword || *type == Type::Battleaxe || *type == Type::Hammer ? 26u : side == 0 ? 32u : 33u;
			const auto point = pose.drawn ? left ? 2u : 1u : sheath;
			auto* mesh = Mesh(player, form, left, player->AsActorState()->IsWeaponDrawn(), side ? g_slots[0].mesh.get() : nullptr);
			if (!mesh || !mesh->parent || !pose.points[point]) { Release(slot); continue; }
			// The root as the engine last left it is what it composes the clone with; the local is derived against it.
			if (!g_root || !std::isfinite(g_root->world.scale) || std::abs(g_root->world.scale) < .00001f) { Release(slot); continue; }
			// This frame's placement of the last drawn pose's attachment (Avatar::Tick ran before us): a heading change
			// since the draw cancels out; only the animation is a frame old.
			const auto& now = Avatar::Current();
			glm::mat4 at = pose.local[point] ? WowModels::PlacementAbsolute(now.position, now.facing) * *pose.local[point] : *pose.points[point];
			if (!pose.drawn) at *= glm::rotate(glm::mat4(1), glm::radians(180.f), glm::vec3(0, 0, 1));
			// The drop moves the sheath point (a bow high on the back), never the hand: part of the target, so Status
			// measures the clone against the dropped point.
			if (!pose.drawn) at = glm::translate(glm::mat4(1), glm::vec3(0, 0, -g_fit[static_cast<std::size_t>(*type)].drop)) * at;
			const auto world = at * Fit(*type);
			if (!Finite(world) || glm::length(glm::vec3(world[0])) < .00001f ||
				glm::length(glm::vec3(world[3]) - glm::vec3(player->GetPosition().x, player->GetPosition().y, player->GetPosition().z)) > 512.f) {
				Release(slot); continue;
			}
			if (slot.mesh.get() != mesh) {
				if (g_slots[1 - side].mesh.get() == mesh) Release(g_slots[1 - side]);
				Release(slot); slot.mesh.reset(mesh); slot.parent.reset(mesh->parent); slot.local = mesh->local; slot.nativeBound = mesh->worldBound;
				RE::BSVisit::TraverseScenegraphGeometries(mesh, [&](RE::BSGeometry* geo) {
					slot.geometry.emplace_back(geo); return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
			if (slot.geometry.empty()) { Release(slot); continue; }
			if (mesh->parent != g_node.get()) {
				// Keep the latest engine socket/local if draw or equip re-parented this same clone.
				slot.parent.reset(mesh->parent); slot.local = mesh->local;
				mesh->parent->DetachChild(mesh);
				g_node->AttachChild(mesh, true);
			}
			slot.form = form->GetFormID(); slot.type = *type; slot.point = point; slot.drawn = pose.drawn;
			slot.attachment = at; slot.frame = pose.frame;
			// No animated hand between their root and the clone; the last rendered pose is one frame old.
			// In glm: NiTransform's compose/invert placed the clone mirrored through their root (October 16).
			mesh->local = Transform(glm::inverse(Matrix(g_root->world)) * world);   // our node's local is identity
			slot.tickYaw = std::atan2(g_node->world.rotate.entry[1][0], g_node->world.rotate.entry[0][0]);
			static int traced = 0;   // the root's yaw frame by frame against their heading (it flipped by pi between reads)
			if (traced < 40) { ++traced; logger::info("playerweapons: frame {} node yaw {:.3f} root yaw {:.3f} heading {:.3f}", pose.frame, slot.tickYaw,
				std::atan2(g_root->world.rotate.entry[1][0], g_root->world.rotate.entry[0][0]), player->GetHeading(false)); }
			mesh->Update(update);
			Glow(slot, player, form, side == 1);
			// Our node was created bare: without a world bound Skyrim's culling drops its whole subtree (the sword at
			// their hand drew nothing, October 16). It carries the clone's bound, grown so a swing never leaves it.
			g_node->worldBound.center = mesh->worldBound.center;
			g_node->worldBound.radius = std::max(mesh->worldBound.radius * 2.0f, 120.0f);
			for (auto& geo : slot.geometry) { g_visible.insert(geo.get()); geo->SetAppCulled(false); }
		}
	}

	// Recover the current body placement from an exported attachment. Shoulders/hips are not exported by
	// the renderer; sample those landmarks from the body pose and identify that approximation in Status.
	std::optional<float> Landmark(const WowModels::AttachmentPose& pose, std::uint32_t point)
	{
		if (!pose.body || pose.body != Avatar::Data()) return {};
		const auto& model = pose.body->body.model;
		std::vector<glm::mat4> bones;
		const auto& current = Avatar::Current();
		wowcore::poseBones(model, wowcore::sequenceWithFallback(model, current.animation), current.timeMs, current.timeMs, bones);
		const auto landmark = wowcore::attachmentMatrix(model, bones, point);
		const auto anchor = wowcore::attachmentMatrix(model, bones, 26);
		if (!landmark || !anchor || !pose.points[26]) return {};
		return (*pose.points[26] * glm::inverse(*anchor) * *landmark)[3].z;
	}
	nlohmann::json WowItems()
	{
		nlohmann::json items = nlohmann::json::array();
		const auto pose = WowModels::LastAttachments();
		if (!pose.body || pose.body != Avatar::Data() || std::chrono::steady_clock::now() - pose.at > std::chrono::milliseconds(250)) return items;
		for (const auto& part : pose.body->attached) {
			if (!part.attached || !part.attached->sheathed) continue;
			const auto& item = *part.attached;
			const auto point = pose.drawn ? item.slot == 16 || item.slot == 17 ? 2u : 1u : item.point;
			if (point >= pose.points.size() || !pose.points[point]) continue;
			auto world = *pose.points[point];
			if (!pose.drawn) world *= glm::rotate(glm::mat4(1), glm::radians(180.f), glm::vec3(0, 0, 1));
			const auto bounds = FitEvidence::TransformBounds(FitEvidence::ModelBounds(part.model), world);
			const auto centre = (bounds.lower + bounds.upper) * .5f;
			items.push_back({ { "slot", item.slot }, { "point", point }, { "drawn", pose.drawn }, { "frame", pose.frame },
				{ "body", pose.body->body.model.name }, { "race", pose.body->appearance.race }, { "sex", pose.body->appearance.sex },
				{ "model", part.model.name }, { "item_bound", FitEvidence::Bound(bounds) }, { "item_px", FitEvidence::Pixels(bounds) },
				{ "bound_centre_height", centre.z - RE::PlayerCharacter::GetSingleton()->GetPosition().z },
				{ "bound_top_height", bounds.upper.z - RE::PlayerCharacter::GetSingleton()->GetPosition().z },
				{ "long_axis", FitEvidence::Vector(glm::normalize(glm::vec3(world[0]))) } });
		}
		return items;
	}
	void RecordReferenceReview(std::string_view kind, bool drawn, const nlohmann::json& native, const nlohmann::json& wow)
	{
		g_referenceReview[std::string(kind)][drawn ? "drawn" : "sheathed"] = {
			{ "body", wow["body"] }, { "race", wow["race"] }, { "sex", wow["sex"] },
			{ "native_bound", native["clone"]["metrics"]["item_bound"] }, { "wow_bound", wow["item_bound"] },
			{ "native_centre_height", native["clone"]["bound_centre_height"] }, { "wow_centre_height", wow["bound_centre_height"] },
			{ "native_top_height", native["clone"]["bound_top_height"] }, { "wow_top_height", wow["bound_top_height"] },
			{ "native_axis", native["clone"]["long_axis"] }, { "wow_axis", wow["long_axis"] },
			{ "axis_angle_deg", native["reference_axis_angle_deg"] }, { "centre_height_difference", native["reference_centre_height_difference"] } };
	}
	nlohmann::json Status()
	{
		nlohmann::json items = nlohmann::json::array();
		for (const auto& slot : g_slots) if (slot.mesh) {
			const auto p = slot.mesh->world.translate;
			const auto a = slot.attachment[3];
			const auto& style = kStyles[static_cast<std::size_t>(slot.type)];
			FitEvidence::Bounds bounds;
			const auto& b = slot.mesh->worldBound;
			if (b.radius > 0) { bounds.Add(glm::vec3(b.center.x, b.center.y, b.center.z) - glm::vec3(b.radius)); bounds.Add(glm::vec3(b.center.x, b.center.y, b.center.z) + glm::vec3(b.radius)); }
			const auto world = Matrix(slot.mesh->world);
			const auto pose = WowModels::LastAttachments();
			const bool shield = slot.type == Type::Shield;
			const bool left = slot.point == 33 || slot.point == 2 || shield || slot.type == Type::Bow || slot.type == Type::Crossbow;
			const auto shoulder = Landmark(pose, left ? 6 : 5);
			const auto& hip = pose.points[left ? 33 : 32];
			auto metrics = FitEvidence::Weapon(bounds, glm::vec3(world[1]), glm::vec3(world[2]), slot.attachment,
				glm::vec3(slot.attachment[0]), glm::vec3(slot.attachment[1]), glm::vec3(slot.attachment[3]), shield, slot.drawn,
				slot.point == 26 || slot.point == 27 || slot.point == 28, shoulder, hip ? std::optional((*hip)[3].z) : std::nullopt, slot.nativeBound);
			metrics["item_bound"] = FitEvidence::Sphere(b);
			metrics["item_px"] = FitEvidence::Pixels(b);
			metrics["scale_ratio"] = slot.nativeBound.radius > 0 ? nlohmann::json(b.radius / slot.nativeBound.radius) : nlohmann::json();
			metrics["shoulder_source"] = "sampled attachment 5/6, aligned to rendered attachment 26";
			metrics["hip_source"] = left ? "rendered attachment 33" : "rendered attachment 32";
			const auto effects = std::ranges::count_if(slot.geometry, [](const auto& g) {
				const auto& shader = g->GetGeometryRuntimeData().shaderProperty;
				return shader && shader->effectData;
			});
			items.push_back({ { "metrics", metrics }, { "item_px", FitEvidence::Pixels(b) }, { "long_axis", FitEvidence::Vector(glm::normalize(glm::vec3(world[1]))) }, { "form", slot.form }, { "type", kNames[static_cast<std::size_t>(slot.type)] },
				{ "point", slot.point }, { "drawn", slot.drawn }, { "frame", slot.frame }, { "node", slot.mesh->name.c_str() },
				{ "world", { p.x, p.y, p.z } }, { "attachment", { a.x, a.y, a.z } },
				{ "distance", glm::length(glm::vec3(p.x, p.y, p.z) - glm::vec3(a)) },
				{ "visible", std::ranges::all_of(slot.geometry, [](const auto& g) { return !g->GetAppCulled(); }) },
				{ "geometry", slot.geometry.size() }, { "effect_geometry", effects },
				{ "glow_shader", slot.effect && slot.effect->effectData ? slot.effect->effectData->GetFormID() : 0u },
				{ "glow_finished", slot.effect && slot.effect->finished },
				{ "weapon_style", { style.itemClass, style.subclass, style.inventoryType } },
				{ "ready_animation", wowcore::meleeAnims(style.itemClass, style.subclass, style.inventoryType).ready },
				{ "parent", slot.mesh->parent ? slot.mesh->parent->name.c_str() : "none" },
				{ "scale", slot.mesh->world.scale }, { "collision", slot.mesh->collisionObject != nullptr },
				// Heights above the player's feet: the item's bound centre and top (a sheathed bow "high on the back").
				{ "bound_centre_height", slot.mesh->worldBound.center.z - RE::PlayerCharacter::GetSingleton()->GetPosition().z },
				{ "bound_top_height", slot.mesh->worldBound.center.z + slot.mesh->worldBound.radius - RE::PlayerCharacter::GetSingleton()->GetPosition().z },
				{ "node_culled", slot.mesh->GetAppCulled() }, { "node_flags", static_cast<std::uint32_t>(slot.mesh->GetFlags().underlying()) },
				{ "ancestors_culled", [&] { std::string out; for (auto* n = slot.mesh->parent; n; n = n->parent) if (n->GetAppCulled()) { out += n->name.c_str(); out += ' '; } return out; }() },
				{ "geometry_bounds", [&] { nlohmann::json b = nlohmann::json::array(); for (const auto& g : slot.geometry) b.push_back({ g->worldBound.center.x, g->worldBound.center.y, g->worldBound.center.z, g->worldBound.radius, g->GetAppCulled(), static_cast<std::uint32_t>(g->GetFlags().underlying()) }); return b; }() },
				{ "blade", { slot.mesh->world.rotate.entry[0][1], slot.mesh->world.rotate.entry[1][1], slot.mesh->world.rotate.entry[2][1] } },   // the NIF's +Y, Skyrim's blade axis
				{ "hand_x", { slot.attachment[0].x / glm::length(glm::vec3(slot.attachment[0])), slot.attachment[0].y / glm::length(glm::vec3(slot.attachment[0])), slot.attachment[0].z / glm::length(glm::vec3(slot.attachment[0])) } },
				{ "hand_z", { slot.attachment[2].x / glm::length(glm::vec3(slot.attachment[2])), slot.attachment[2].y / glm::length(glm::vec3(slot.attachment[2])), slot.attachment[2].z / glm::length(glm::vec3(slot.attachment[2])) } },
				{ "tick_yaw", slot.tickYaw }, { "node_yaw", g_node ? std::atan2(g_node->world.rotate.entry[1][0], g_node->world.rotate.entry[0][0]) : 0.f },
				{ "expected", g_node ? nlohmann::json{ (g_node->world * slot.mesh->local).translate.x, (g_node->world * slot.mesh->local).translate.y, (g_node->world * slot.mesh->local).translate.z } : nlohmann::json() },
				{ "local", { slot.mesh->local.translate.x, slot.mesh->local.translate.y, slot.mesh->local.translate.z } },
				{ "node_world", g_node ? nlohmann::json{ g_node->world.translate.x, g_node->world.translate.y, g_node->world.translate.z } : nlohmann::json() } });
		}
		const auto& f = g_fit[g_selected];
		const auto stance = ReadyStance();
		return { { "enabled", g_enabled }, { "glow", g_glow }, { "stance", g_stance }, { "frontview", g_frontView },
			{ "ready_animation", stance ? nlohmann::json(*stance) : nlohmann::json() }, { "animation", Avatar::Current().animation },
			{ "body_hidden", Surrogate::BodyHidden() }, { "items", items }, { "wow_items", WowItems() }, { "last_fixture_reference_review", g_referenceReview },
			{ "type", kNames[g_selected] }, { "fit", { f.lift, f.out, f.scale, f.roll, f.pitch, f.drop } } };
	}
	bool Command(std::string_view arg, std::string_view value, std::string& message)
	{
		if (arg == "glow" || arg == "stance" || arg == "frontview") {
			if (value != "on" && value != "off") { message = "playerweapons glow|stance|frontview on|off"; return false; }
			if (arg == "glow") {
				g_glow = value == "on";
				if (!g_glow) for (auto& slot : g_slots) StopGlow(slot);
			} else if (arg == "stance") g_stance = value == "on";
			else g_frontView = value == "on";
		} else if (arg == "type") {
			const auto it = std::ranges::find(kNames, value);
			if (it == std::end(kNames)) { message = "playerweapons type dagger|sword|mace|axe|greatsword|battleaxe|hammer|staff|bow|crossbow|shield"; return false; }
			g_selected = static_cast<std::size_t>(it - std::begin(kNames));
		} else if (arg == "lift" || arg == "out" || arg == "scale" || arg == "roll" || arg == "pitch" || arg == "drop") {
			float v = 0; std::istringstream input{ std::string(value) };
			if (!(input >> v) || !std::isfinite(v) || (input >> std::ws, !input.eof()) || (arg == "scale" && v <= 0)) {
				message = "playerweapons lift|out|scale|roll|pitch N (finite value, positive scale)"; return false;
			}
			auto& f = g_fit[g_selected];
			if (arg == "lift") f.lift = v; else if (arg == "out") f.out = v; else if (arg == "scale") f.scale = v; else if (arg == "roll") f.roll = v; else if (arg == "pitch") f.pitch = v; else f.drop = v;
		} else if (value.empty() && (arg == "on" || arg == "off" || arg == "status")) {
			if (arg != "status") { g_enabled = arg == "on"; Reset(); }
		} else { message = "playerweapons on|off|status; glow|stance|frontview on|off; type NAME; lift|out|scale|roll N"; return false; }
		message = Status().dump(); return true;
	}
}
