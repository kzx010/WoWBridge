#pragma once

#include <d3d11.h>
#include <nlohmann/json.hpp>

// Session switches and render-thread resources for M10. Engine references never cross from Tick to a draw.
namespace Shadows
{
	bool Install();
	void Tick();  // main thread: snapshot NPC feet alongside the shoulder gather
	bool Command(std::string_view a_route, std::string_view a_value, std::string& a_message);
	nlohmann::json Status();

	struct Frame
	{
		ID3D11Device* device;
		ID3D11DeviceContext* context;
		RE::NiPoint3 cam;
		const float* viewProj;  // row-major, camera-relative
		ID3D11ShaderResourceView* depth;
		UINT width, height;
	};
	bool Receiving();  // render thread: this frame's receiving constants were bound successfully
	void BeginLate(const Frame& a_frame);  // descriptors, probes, and b3/t2/t3/s1 for world models
	void DrawMask(const Frame& a_frame);   // explicit diagnostic, captured by UiPass at full resolution
	void DrawBlobs(const Frame& a_frame, ID3D11RenderTargetView* a_target);
	bool NpcFeet(RE::FormID a_actor, RE::NiPoint3& a_feet);
	void ClearBlobs();
	void AddBlob(const RE::NiPoint3& a_center, float a_radius, bool a_priority = false);
}
