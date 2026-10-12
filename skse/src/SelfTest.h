#pragma once

// In-game test harness: enabled by Data/SKSE/Plugins/WoWBridge.ini [Test] autorun=1.
namespace SelfTest
{
	void OnDataLoaded();
	void BarterMenuFrame(); // test-only observer; native barter pauses player polls
	void ItemMenuFrame(RE::InventoryMenu* a_menu);  // only advances an active item-preview test, while Skyrim is paused
}
