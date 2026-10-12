// WoWBridge compat: where WoW's per-account files (SavedVariables, bindings) live; set by the host.
#pragma once
#include <string>
namespace wowee::core {
std::string getConfigRoot();
void setConfigRoot(const std::string& dir);
}  // namespace wowee::core
