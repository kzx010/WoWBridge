#pragma once

// Data/SKSE/Plugins/WoWBridge.ini, read once at kDataLoaded.
namespace Config
{
	struct Values
	{
		bool        autorun = false;
		std::string testCell = "Riverwood";
		std::string testOnly;   // the suite's steps to run (comma-separated), after its setup; empty: all
		std::string wowRoot = "WoW";   // legacy parent containing client/Data
		std::string wowClient;   // existing client folder containing WoW.exe and Data
		bool        wowUi = true;   // draw the WoW 1.12 interface
		bool        itemPreview = true;  // standalone WoW models in Skyrim's item menus
		bool        skyrimGearLooks = true;  // dress her WoW body in mapped Skyrim equipment
		bool        cursor = true;  // WoW cursor control in play (RMB steers); test runs drive it themselves
	};
	std::string ClientData();
	void Load();
	const Values& Get();
}
