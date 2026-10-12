// Definitions behind the compat headers (wowcore/compat/core/*.hpp).
#include "core/app_clock.hpp"
#include "core/config_paths.hpp"

#include <string>

namespace wowee::core {
namespace {
double g_time = 0.0;
std::string g_configRoot = ".";
}  // namespace
double appTimeSeconds() { return g_time; }
void advanceTestClock(double seconds) { g_time += seconds; }
void setAppTimeSeconds(double seconds) { g_time = seconds; }
std::string getConfigRoot() { return g_configRoot; }
void setConfigRoot(const std::string& dir) { g_configRoot = dir; }
}  // namespace wowee::core
