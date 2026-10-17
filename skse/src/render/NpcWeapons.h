#pragma once

#include "wowcore/item_preview.hpp"
#include <nlohmann/json.hpp>

// Shared, numeric review evidence. Missing measurements stay null; flags never decide fixture pass/fail.
namespace FitEvidence
{
	struct Bounds
	{
		glm::vec3 lower{ 0 }, upper{ 0 };
		bool valid = false;
		void Add(const glm::vec3& a_point);
	};
	Bounds ModelBounds(const wowcore::Model& a_model, const glm::mat4& a_world = glm::mat4(1));
	Bounds TransformBounds(const Bounds& a_bounds, const glm::mat4& a_world);
	nlohmann::json Vector(const glm::vec3& a_value);
	nlohmann::json Bound(const Bounds& a_bounds);
	nlohmann::json Sphere(const RE::NiBound& a_bound);
	nlohmann::json Pixels(const Bounds& a_bounds);
	nlohmann::json Pixels(const RE::NiBound& a_bound);
	nlohmann::json Range(std::optional<float> a_value, float a_min, float a_max);
	nlohmann::json Below(std::optional<float> a_value, float a_limit);
	float Angle(const glm::vec3& a_axis, const glm::vec3& a_reference);
	nlohmann::json Weapon(const Bounds& a_bounds, const glm::vec3& a_axis, const glm::vec3& a_normal,
		const glm::mat4& a_attachment, const glm::vec3& a_forward, const glm::vec3& a_outward, const glm::vec3& a_forearm,
		bool a_shield, bool a_drawn, bool a_back, std::optional<float> a_shoulder, std::optional<float> a_hip,
		const RE::NiBound& a_native);
}

// Main-thread engine access/culling; the world pass owns only assets and copied transforms.
namespace NpcWeapons
{
	constexpr float kRadius = 2048.0f;  // 32 yards
	constexpr std::size_t kMaxActors = 128;
	enum class Type { Dagger, Sword, Mace, Axe, Greatsword, Battleaxe, Hammer, Staff, Bow, Crossbow, Shield, Count };
	const char* TypeName(Type a_type);
	struct Instance
	{
		RE::FormID ref = 0;
		std::uint32_t entry = 0, display = 0;
		std::uint64_t gather = 0;
		Type type = Type::Sword;
		bool female = false, drawn = false, hidden = false;
		std::string node;
		glm::mat4 nodeWorld{ 1 }, world{ 1 };
		RE::NiBound nativeBound{};
		glm::vec3 outward{ 0 }, forearm{ 0 };
		std::optional<float> shoulder, hip;
		std::shared_ptr<const wowcore::ItemPreview> asset;
	};
	struct DrawInfo
	{
		Instance instance;
		std::uint32_t batches = 0;
		std::uint64_t frame = 0;
		std::chrono::steady_clock::time_point at;
	};
	void Tick();
	void Reset();
	void SetEnabled(bool a_enabled);
	bool Enabled();
	bool SelectType(std::string_view a_type);
	Type SelectedType();
	std::array<float, 4> SetFit(bool a_female, const char* a_which, float a_value);
	std::vector<Instance> Current();
	void RecordReady(const std::shared_ptr<const wowcore::ItemPreview>& a_asset, bool a_ready);
	void RecordDraws(std::vector<DrawInfo> a_draws);
	std::vector<DrawInfo> Draws();
	// Why an actor's hands do or do not carry a WoW model: equipped forms, Armory looks, Skyrim kinds, mesh and socket.
	nlohmann::json Describe(RE::FormID a_actor);
	nlohmann::json Evidence(const Instance& a_instance);
}
