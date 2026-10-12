#pragma once

#include "render/ItemPreview.h"

// Loaded WoW ARMO/WEAP references. All engine access and culling happen on the main thread;
// the render thread sees only copied transforms and owned, immutable assets.
namespace GroundItems
{
	constexpr float kRadius = 2048.0f;  // 32 yards
	constexpr std::size_t kMaxInstances = 128;
	struct Instance
	{
		RE::FormID ref = 0;
		std::uint32_t entry = 0;
		std::shared_ptr<const ItemPreview::Asset> asset;
		glm::mat4 world{ 1.0f };  // absolute Skyrim coordinates, including the fixed model-to-collision fit
		bool hidden = false;
	};
	struct DrawInfo
	{
		RE::FormID ref = 0;
		std::uint32_t entry = 0, display = 0, batches = 0;
		glm::mat4 world{ 1.0f };
	};
	void Tick();
	void Reset();
	std::vector<Instance> Current();
	void RecordDraws(std::vector<DrawInfo> a_draws);  // render thread, only successfully drawn instances
	std::vector<DrawInfo> Draws();                   // SelfTest
}
