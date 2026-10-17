#pragma once

#include "wowcore/item_preview.hpp"
#include <nlohmann/json.hpp>

// Engine access stays on the main thread. The world pass receives owned assets and copied skeleton transforms,
// never actor/node pointers. The player's surrogate and menu previews do not use this path.
namespace NpcShoulders
{
	constexpr float kRadius = 2048.0f;  // 32 yards, 64 Skyrim units per yard
	constexpr std::size_t kMaxInstances = 128;
	struct Asset
	{
		std::shared_ptr<const wowcore::ItemPreview> item;
		std::vector<std::size_t> sides;  // per M2 part: 0 left, 1 right (Skyrim nodes, independent of WoW body IDs)
	};
	struct Instance
	{
		RE::FormID ref = 0;
		std::uint32_t entry = 0, display = 0;
		std::uint64_t gather = 0;
		bool female = false;
		RE::NiBound nativeBound{};
		std::array<RE::NiBound, 2> bodyBounds{};
		std::array<bool, 2> upperArm{};
		std::array<glm::mat4, 2> nodes{ glm::mat4(1), glm::mat4(1) };  // full node world transforms, left/right
		std::array<glm::mat4, 2> world{ glm::mat4(1), glm::mat4(1) };  // calibrated model yards -> Skyrim units
		std::shared_ptr<const Asset> asset;
	};
	struct DrawInfo
	{
		Instance instance;  // the exact gather snapshot consumed by this render frame
		std::array<std::uint32_t, 2> batches{};
		std::uint64_t frame = 0;
		std::chrono::steady_clock::time_point at;
	};
	nlohmann::json Evidence(const Instance& a_instance);
	void Tick();
	void Reset();
	void SetEnabled(bool a_enabled);  // main thread; session toggle, default on
	bool Enabled();
	// Tuning by eye (wb npcshoulders lift|out|scale N): the fit for their sex's NPCs; units along z (up), x (out), and
	// the model scale factor. Returns the three in force.
	std::array<float, 3> SetFit(bool a_female, const char* a_which, float a_value);
	std::vector<Instance> Current();
	void RecordDraws(std::vector<DrawInfo> a_draws);
	std::vector<DrawInfo> Draws();  // only successful draws from the last world frame
}
