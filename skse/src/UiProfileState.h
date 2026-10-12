#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace UiProfile
{
    inline constexpr std::string_view Legacy = "Elunara";
    inline constexpr std::size_t MaxLength = 42;
    inline bool Valid(std::string_view key)
    {
        if (key == Legacy) return true;
        if (key.size() != MaxLength || !key.starts_with("character-")) return false;
        return std::all_of(key.begin() + 10, key.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    }
    struct State
    {
        std::string key{Legacy};
        void LegacyLoad() { key = Legacy; }
        bool Load(std::uint32_t version, std::string_view text)
        {
            if (version != 1 || !Valid(text)) return false;
            key = text; return true;
        }
    };
    inline std::filesystem::path Directory(const std::filesystem::path& root, std::string_view key)
    {
        return root / (Valid(key) ? std::string(key) : std::string(Legacy));
    }
}
