#pragma once

// Our D3D11 pass inside Skyrim's frame, right after Main::RenderWorld (before post-processing/TAA):
// draws into Skyrim's HDR scene target + motion vectors, depth-tested against Skyrim's main depth.
// M1: debug boxes and a pixel read-back oracle; WoW models/effects build on the same pass later.
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace WorldPass
{
	bool Install();

	// The WoW models (the player's WoW body), drawn after Skyrim's tonemapping into the bound target, depth-tested
	// against the scene with RenderWorld's view: their colours reach the screen as the client's do, whatever exposure
	// Skyrim's eye adaptation has chosen (UI pass, before the interface).
	void DrawLate(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

	struct Box
	{
		RE::NiPoint3 center;  // world position
		float        half;    // half extent (Skyrim units)
		float        rgba[4];
	};
	void SetBoxes(std::vector<Box> a_boxes);

	// Pixel oracle: read back Skyrim's scene colour where a world point projects, after our draw.
	struct Sample
	{
		bool  done = false;
		bool  onScreen = false;
		int   x = 0, y = 0;
		float rgba[4]{};
	};
	int    RequestSample(const RE::NiPoint3& a_world);  // returns a ticket
	Sample GetSample(int a_ticket);

	// The exact view the last frame was rendered with (camera-relative world->clip, plus the camera
	// position it is relative to). NiCamera::worldToCam read outside rendering is NOT this matrix.
	struct View
	{
		bool         valid = false;
		double       viewProj[4][4]{};
		RE::NiPoint3 cam;
		int          width = 0, height = 0;
	};
	View LastView();

	struct Info
	{
		bool  installed = false;
		bool  drawing = false;  // pass ran at least once with valid targets
		bool  reversedZ = false;
		int   width = 0, height = 0;
		int   sceneFormat = 0, depthFormat = 0;
		std::uint64_t frames = 0;
	};
	Info GetInfo();
}
