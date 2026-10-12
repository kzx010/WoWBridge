#include "wowcore/mpq.hpp"

// StormLib's API takes TCHAR paths; vcpkg builds the library ANSI (STORM_UNICODE off), so this file must see
// TCHAR as char even where the rest of the DLL is compiled with UNICODE.
#ifdef _WIN32
#undef UNICODE
#undef _UNICODE
#endif
#include <StormLib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>

namespace wowcore {
namespace {
// 1.12.1 client: patch archives override everything; the base archives do not overlap.
constexpr const char* kOrder[] = {"patch-2.MPQ", "patch.MPQ", "dbc.MPQ", "fonts.MPQ", "interface.MPQ", "misc.MPQ",
                                  "model.MPQ", "sound.MPQ", "speech.MPQ", "terrain.MPQ", "texture.MPQ", "wmo.MPQ",
                                  "base.MPQ", "backup.MPQ"};

// StormLib keeps state shared between archives: two threads opening, reading or closing their own archives at once
// (the avatar and the spell visuals loading while the interface reads) corrupted it and froze the game. Every call
// into it goes through this one lock.
std::mutex& storm() {
    static std::mutex m;
    return m;
}

std::string normalized(std::string_view p) {
    std::string s(p);
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}

std::string folded(std::string_view p) {
    auto s = normalized(p);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The same wildcard syntax used by the archive file listing; paths are already folded.
bool matches(std::string_view pattern, std::string_view name) {
    size_t p = 0, n = 0, star = std::string_view::npos, retry = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) { ++p; ++n; }
        else if (p < pattern.size() && pattern[p] == '*') { star = p++; retry = n; }
        else if (star != std::string_view::npos) { p = star + 1; n = ++retry; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}
}  // namespace

Mpq::Mpq(const std::filesystem::path& dataDir, const std::filesystem::path& iconDirectory,
         const std::filesystem::path& minimapDirectory) {
    for (const auto& [directory, prefix] : {std::pair{iconDirectory, "Interface\\Icons\\"},
                                          std::pair{minimapDirectory, "Interface\\Minimap\\"}}) {
        if (directory.empty() || !std::filesystem::exists(directory)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (!entry.is_regular_file() || folded(entry.path().extension().string()) != ".blp") continue;
            const auto key = folded(prefix + entry.path().filename().string());
            if (!icons_.emplace(key, entry.path()).second)
                throw std::runtime_error("duplicate loose texture path: " + key);
        }
    }
    std::lock_guard lock(storm());
    for (const char* name : kOrder) {
        const auto path = dataDir / name;
        if (!std::filesystem::exists(path)) continue;
        HANDLE h = nullptr;
        if (!SFileOpenArchive(path.string().c_str(), 0, STREAM_FLAG_READ_ONLY, &h))
            throw std::runtime_error("cannot open " + path.string() + " (StormLib error " + std::to_string(SErrGetLastError()) + ")");
        archives_.push_back({name, h});
    }
    if (archives_.empty()) throw std::runtime_error("no WoW archives in " + dataDir.string());
}

Mpq::~Mpq() {
    std::lock_guard lock(storm());
    for (auto& a : archives_) SFileCloseArchive(a.handle);
}

std::optional<std::vector<uint8_t>> Mpq::read(std::string_view path) const {
    if (const auto it = icons_.find(folded(path)); it != icons_.end()) {
        std::ifstream file(it->second, std::ios::binary | std::ios::ate);
        if (!file) return std::nullopt;  // a broken override does not silently fall back
        const auto size = file.tellg();
        if (size < 0) return std::nullopt;
        std::vector<uint8_t> data(static_cast<size_t>(size));
        file.seekg(0);
        if (!data.empty() && !file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
            return std::nullopt;
        return data;
    }
    const std::string p = normalized(path);
    std::lock_guard lock(storm());
    for (const auto& a : archives_) {
        HANDLE f = nullptr;
        if (!SFileOpenFileEx(a.handle, p.c_str(), SFILE_OPEN_FROM_MPQ, &f)) continue;
        std::vector<uint8_t> data(SFileGetFileSize(f, nullptr));
        DWORD got = 0;
        const bool ok = data.empty() || (SFileReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr) && got == data.size());
        SFileCloseFile(f);
        if (ok) return data;
        // A file present but unreadable in a higher-priority archive is corrupt there; the client would fail too.
        return std::nullopt;
    }
    return std::nullopt;
}

bool Mpq::exists(std::string_view path) const {
    if (const auto it = icons_.find(folded(path)); it != icons_.end())
        return std::filesystem::is_regular_file(it->second);
    const std::string p = normalized(path);
    std::lock_guard lock(storm());
    return std::any_of(archives_.begin(), archives_.end(), [&](const Archive& a) { return SFileHasFile(a.handle, p.c_str()); });
}

std::vector<std::string> Mpq::list(std::string_view mask) const {
    const std::string m = normalized(mask);
    std::set<std::string> names;
    for (const auto& [key, file] : icons_)
        if (matches(folded(mask), key)) names.insert(std::string(key.starts_with("interface\\minimap\\") ?
            "Interface\\Minimap\\" : "Interface\\Icons\\") + file.filename().string());
    std::lock_guard lock(storm());
    for (const auto& a : archives_) {
        SFILE_FIND_DATA fd;
        HANDLE find = SFileFindFirstFile(a.handle, m.c_str(), &fd, nullptr);
        if (!find) continue;
        do {
            if (!icons_.contains(folded(fd.cFileName))) names.insert(fd.cFileName);
        } while (SFileFindNextFile(find, &fd));
        SFileFindClose(find);
    }
    return {names.begin(), names.end()};
}

}  // namespace wowcore
