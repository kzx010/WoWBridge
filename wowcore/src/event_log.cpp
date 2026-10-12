#include "wowcore/event_log.hpp"

#include "addons/lua_api_helpers.hpp"

#include <fstream>
#include <sstream>

namespace wowcore {
namespace {
// Lua's %q / SavedVariables escapes: \" \\ \n and an escaped newline.
std::string unescape(const std::string& s, size_t from, size_t to) {
    std::string out;
    for (size_t i = from; i < to; ++i) {
        if (s[i] == '\\' && i + 1 < to) {
            const char c = s[++i];
            out += c == 'n' ? '\n' : c == 'r' ? '\r' : c;
        } else {
            out += s[i];
        }
    }
    return out;
}

// One record: time|EVENT|arg... where a string argument is %q-quoted (and may hold '|') and anything else is bare.
RecordedEvent parse(const std::string& rec) {
    std::vector<std::string> fields;
    size_t i = 0;
    while (i <= rec.size()) {
        if (i < rec.size() && rec[i] == '"') {
            size_t j = i + 1;
            while (j < rec.size() && rec[j] != '"') j += rec[j] == '\\' ? 2 : 1;
            fields.push_back(unescape(rec, i + 1, j));
            i = j + 2;   // past the quote and the separator
        } else {
            const size_t j = rec.find('|', i);
            const std::string bare = rec.substr(i, j == std::string::npos ? std::string::npos : j - i);
            fields.push_back(bare == "nil" ? std::string(wowee::game::kEventNil) : bare);
            if (j == std::string::npos) break;
            i = j + 1;
        }
    }
    RecordedEvent e;
    if (fields.size() >= 2) {
        e.time = std::stod(fields[0]);
        e.name = fields[1];
        e.args.assign(fields.begin() + 2, fields.end());
    }
    return e;
}
}  // namespace

std::vector<RecordedEvent> loadEventLog(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<RecordedEvent> out;
    bool inTable = false;
    for (std::string line; std::getline(in, line);) {
        if (!inTable) { inTable = line.rfind("WBOracleEvents = {", 0) == 0; continue; }
        if (line == "}") break;
        const size_t q0 = line.find("= \"");
        const size_t q1 = line.rfind("\",");
        if (q0 == std::string::npos || q1 == std::string::npos || q1 <= q0 + 3) continue;
        out.push_back(parse(unescape(line, q0 + 3, q1)));
    }
    return out;
}
}  // namespace wowcore
