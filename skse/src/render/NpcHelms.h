#pragma once

#include "wowcore/item_preview.hpp"
#include "render/NpcWeapons.h"
#include <nlohmann/json.hpp>

// Engine objects/cull flags belong to the main thread. The renderer consumes owned assets and copied transforms.
namespace NpcHelms
{
	constexpr float kRadius = 2048.f;  // 32 yards
	constexpr std::size_t kMaxActors = 128;
	struct Asset
	{
		std::shared_ptr<const wowcore::ItemPreview> item;
		std::uint32_t visibility = 0;
		bool hideHair = false, hideBeard = false, hideEars = false;
		FitEvidence::Bounds bounds;  // M2 model vertices, before the attachment transform
		bool circlet = false, openFace = false;
	};
	struct Instance
	{
		RE::FormID ref = 0;
		std::uint32_t entry = 0, display = 0;
		std::uint64_t gather = 0;
		bool female = false, elf = false, hidden = false, hairHidden = false, beardHidden = false;
		std::uint32_t hairGeometry = 0, beardGeometry = 0;
		std::vector<std::string> culledNames;   // the hair/beard geometries taken, with their parents (evidence)
		glm::mat4 nodeWorld{ 1 }, world{ 1 };
		FitEvidence::Bounds head, hair, outer, interior;
		float forehead = 0, foreheadRadius = 0, computedScale = 0, scale = 0;
		glm::vec3 computedOffset{ 0 }, offset{ 0 };
		RE::NiBound faceBound{}, headBound{}, nativeBound{};
		std::uint32_t headVertices = 0;
		bool browMeasured = false;
		bool autoFitAttempted = false, autoFit = false;
		std::string fitRejection;
		nlohmann::json headVerification;  // same-part CPU/worldBound samples, taken together on the main thread
		std::shared_ptr<const Asset> asset;
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
	std::array<float, 4> SetFit(bool a_female, const char* a_which, float a_value);
	std::vector<Instance> Current();
	void RecordReady(const std::shared_ptr<const Asset>& a_asset, bool a_ready);
	void RecordDraws(std::vector<DrawInfo> a_draws);
	std::vector<DrawInfo> Draws();
	nlohmann::json Describe(RE::FormID a_actor);
	nlohmann::json Evidence(const Instance& a_instance);
}
