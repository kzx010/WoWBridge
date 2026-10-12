// WoW 1.12 archive set, read in the client's own priority order: patch-2 > patch > base archives.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wowcore {

class Mpq {
public:
    // Optional flat loose icon directory (SKSE/Plugins/WoWBridge/Icons). Its BLPs
    // override Interface\\Icons\\<filename> before archive lookup, case-insensitively.
    // Optional flat minimap directory overrides Interface\\Minimap\\<filename>.
    explicit Mpq(const std::filesystem::path& dataDir, const std::filesystem::path& iconDirectory = {},
                 const std::filesystem::path& minimapDirectory = {});
    ~Mpq();
    Mpq(const Mpq&) = delete;
    Mpq& operator=(const Mpq&) = delete;

    std::optional<std::vector<uint8_t>> read(std::string_view path) const;   // WoW path, any case or slash
    bool exists(std::string_view path) const;
    std::vector<std::string> list(std::string_view mask) const;              // e.g. "Interface\\FrameXML\\*"
    size_t archiveCount() const { return archives_.size(); }

private:
    struct Archive { std::string name; void* handle; };
    std::vector<Archive> archives_;        // highest priority first
    std::map<std::string, std::filesystem::path> icons_; // lower-case WoW path -> loose icon/minimap BLP

};

}  // namespace wowcore
