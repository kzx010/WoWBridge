// WoWBridge compat: WoWee's UI clock. The host drives it (the plugin's paused-aware clock; tests advance it).
#pragma once
namespace wowee::core {
double appTimeSeconds();
void advanceTestClock(double seconds);
void setAppTimeSeconds(double seconds);   // WoWBridge: host-owned time (Skyrim pauses; WoW's UI clock must too)
}  // namespace wowee::core
