#pragma once

#include <glm/mat4x4.hpp>

namespace Avatar { struct Loaded; }

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Buffer;
struct ID3D11DepthStencilState;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;
namespace wowcore { struct ItemPreview; }

// WoW models in Skyrim's frame (M8): the player's WoW body and what hangs on it, drawn by the world pass after Skyrim's
// world (depth-tested against it, motion vectors for its TAA). Render thread.
namespace WowModels
{
	struct Frame
	{
		ID3D11Device*            device = nullptr;
		ID3D11DeviceContext*     context = nullptr;
		ID3D11Buffer*            frameCb = nullptr;      // the world pass's view (camera-relative)
		ID3D11DepthStencilState* depthWrite = nullptr;   // the world pass's depth test, writing
		bool                     reversedZ = false;
		RE::NiPoint3             cam;
		ID3D11ShaderResourceView* depth = nullptr;  // Skyrim's scene depth (the circle is laid on it)
		float                    viewProj[16]{};    // the frame's, row-major (camera-relative Skyrim units -> clip)
	};
	void Draw(const Frame& a_frame);
	// Last body actually drawn, after blending, upper-body animation and the mount's saddle.
	// Absolute Skyrim world matrices (not the pass's camera-relative matrices). Copied across threads.
	struct AttachmentPose
	{
		const Avatar::Loaded* body = nullptr;
		bool drawn = false;
		std::uint64_t frame = 0;
		std::chrono::steady_clock::time_point at;
		std::array<std::optional<glm::mat4>, 34> points;
		// The same points in their own frame (saddle, size and the attachment; no placement): a consumer places them
		// with this frame's position and facing, so a turn between the draw and its Tick cannot mirror or lag them.
		std::array<std::optional<glm::mat4>, 34> local;
	};
	AttachmentPose LastAttachments();
	// Their placement in absolute Skyrim world (the pass's Placement without the camera re-basing).
	glm::mat4 PlacementAbsolute(const RE::NiPoint3& a_position, float a_facing);
	bool GroundReady(const std::shared_ptr<const wowcore::ItemPreview>& a_item);  // main thread: uploaded and recent world draw

	// Their face for the interface's portraits (PlayerPortrait): drawn once their model is up, through the model's own
	// portrait camera (M2 camera 0). nullptr until then.
	ID3D11ShaderResourceView* Portrait();
	// The target's face (a Skyrim actor's head, Portrait::Current) for TargetPortrait. nullptr: none drawn for it.
	ID3D11ShaderResourceView* TargetPortrait();
	// Slot 0: target, 1-4: party. Optional actor ID prevents showing a previous roster's face.
	ID3D11ShaderResourceView* UnitPortrait(std::size_t a_slot, RE::FormID a_actor = 0);

	struct Stats
	{
		std::uint64_t frames = 0;   // frames the avatar was drawn
		std::uint32_t batches = 0;  // in the last of them
		std::uint32_t mountDisplay = 0, mountBatches = 0;
		std::uint64_t circles = 0;  // frames a selection circle was drawn
	};
	Stats GetStats();

	// Independent menu resources; never evicts the player's body/portraits. Caller saves/restores D3D bindings.
	struct ItemFrame
	{
		ID3D11Device* device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		std::uint32_t width = 0, height = 0;
		float viewProj[16]{}, world[16]{};  // same row-major convention as Frame
		float lightDir[3]{};  // fixed key relative to the observed UI camera
		long left = 0, top = 0, right = 0, bottom = 0;
	};
	bool DrawItem(const ItemFrame& a_frame, const std::shared_ptr<const wowcore::ItemPreview>& a_item, std::uint32_t& a_batches);
	bool CompositeItem(const ItemFrame& a_frame, ID3D11RenderTargetView* a_destination);
}
