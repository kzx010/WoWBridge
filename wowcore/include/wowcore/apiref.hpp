// The API reference check: WoWBridge's 1.12 API against the answers the real client gave (evidence/apiref).
#pragma once
#include <string>
#include <vector>

struct lua_State;

namespace wowcore {
struct ApiRefResult {
    int probes = 0, matches = 0;
    std::string summary;   // per state, then the functions that differ, worst first
    std::string details;   // every differing probe with both answers
    std::string error;     // set when the check could not run
};

// Runs the evidence file (it defines WBOracleAPI), then for each state calls __WBFixture(state) and asks every
// recorded probe. Answers that depend on the moment or the machine (GetTime, GetNetStats ...) are not compared.
ApiRefResult checkApiReference(lua_State* L, const std::string& evidencePath, const std::vector<std::string>& states);
struct TooltipRefResult {
    int tooltips = 0, sameLines = 0, sameSize = 0;
    std::string summary, details, error;
};

// After checkApiReference: every recorded tooltip (/wbo tips) filled by our GameTooltip in the same state, compared
// line by line (text and colour) and by size.
TooltipRefResult checkTooltipReference(lua_State* L, const std::vector<std::string>& states);
}  // namespace wowcore
