#include "Config.h"

#include <SimpleIni.h>

namespace Config
{
	namespace
	{
		Values g_values;
	}

	std::string ClientData()
	{
		return g_values.wowClient.empty() ? g_values.wowRoot + R"(\client\Data)" : g_values.wowClient + R"(\Data)";
	}

	void Load()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		if (ini.LoadFile("Data/SKSE/Plugins/WoWBridge.ini") < 0) {
			logger::warn("config: WoWBridge.ini not found, using defaults");
			return;
		}
		g_values.autorun = ini.GetBoolValue("Test", "autorun", g_values.autorun);
		g_values.testCell = ini.GetValue("Test", "cell", g_values.testCell.c_str());
		g_values.testOnly = ini.GetValue("Test", "only", "");
		g_values.wowRoot = ini.GetValue("WoW", "root", g_values.wowRoot.c_str());
		g_values.wowClient = ini.GetValue("WoW", "client", "");
		g_values.wowUi = ini.GetBoolValue("WoW", "interface", g_values.wowUi);
		g_values.itemPreview = ini.GetBoolValue("WoW", "itemPreview", g_values.itemPreview);
		g_values.skyrimGearLooks = ini.GetBoolValue("WoW", "skyrimGearLooks", g_values.skyrimGearLooks);
		g_values.cursor = ini.GetBoolValue("WoW", "cursor", g_values.cursor);
		logger::info("config: autorun={} cell={} clientData={}", g_values.autorun, g_values.testCell, ClientData());
	}

	const Values& Get() { return g_values; }
}
