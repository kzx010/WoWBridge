// The real client's event sequences (evidence/events, recorded by WBOracle), for replaying into a UiSession.
#pragma once
#include <string>
#include <vector>

namespace wowcore {
struct RecordedEvent {
    double time = 0;                 // seconds since the recorder loaded
    std::string name;
    std::vector<std::string> args;   // as UiSession::fire takes them: text, numbers as text, nil as kEventNil
};

// Reads WBOracleEvents out of a WBOracle SavedVariables file; empty if it has none.
std::vector<RecordedEvent> loadEventLog(const std::string& savedVariablesFile);
}  // namespace wowcore
