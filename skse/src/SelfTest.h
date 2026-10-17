#pragma once

// In-game test harness: enabled by Data/SKSE/Plugins/WoWBridge.ini [Test] autorun=1.
namespace SelfTest
{
	void OnDataLoaded();
	bool WantFrontView();  // only the active player-weapons fixture may suppress the drawn camera's heading fold
	// A pausing Skyrim menu someone opens on purpose during a suite (the wb levelup helper: Stats, LevelUp); the
	// stray-menu guard leaves it alone while claimed. Paired claim/release; counted.
	void ClaimMenu(std::string_view a_menu, bool a_claim);
	void BarterMenuFrame(); // test-only observer; native barter pauses player polls
	void ItemMenuFrame(RE::InventoryMenu* a_menu);  // only advances an active item-preview test, while Skyrim is paused
}
