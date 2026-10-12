#pragma once

// 2D pass: Dear ImGui (FreeType) drawn just before Skyrim's own UI, so Skyrim menus stay on top.
// The WoW FrameXML engine rasterizes through this (WoWee's widget renderer uses ImGui draw lists).
namespace UiPass
{
	bool Install();

	// WoW UI coordinates: 768-unit virtual height (UIParent), origin top-left here.
	float Scale();

	void SetDebugOverlay(bool a_on);  // M1 gate: test quad + FRIZQT text

	// Frame capture (what is on the bound target after our UI): <SKSE log dir>/capture_<name>.png
	void RequestCapture(const std::string& a_name);
	struct Capture
	{
		bool                      done = false;
		int                       width = 0, height = 0;
		std::vector<std::uint8_t> rgba;  // 8-bit RGBA
	};
	Capture TakeCapture(const std::string& a_name);  // done=false until written

	struct Info
	{
		bool          drawing = false;
		bool          fontLoaded = false;
		bool          boundIsFramebuffer = false;
		int           width = 0, height = 0, format = 0;
		std::uint64_t frames = 0;
	};
	Info GetInfo();
}
