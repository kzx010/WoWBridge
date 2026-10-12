#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Save-independent policy, shared by the engine adapter and portable tests.
namespace Integration
{
    enum class Mode : std::uint32_t { Intro = 0, Skyrim = 1, WoW = 2 };
    struct State
    {
        Mode mode = Mode::WoW; // compatibility with existing saves and main-menu coc
        bool offered = false;
        std::optional<std::uint32_t> selectedSex;
        void NewGame() { mode = Mode::Intro; offered = false; selectedSex.reset(); }
        void Load(Mode saved = Mode::WoW) { mode = saved; offered = false; selectedSex.reset(); }
        bool Offer(bool creatorOpen, bool requested = false)
        {
            if (selectedSex || (!requested && (mode != Mode::Intro || offered || !creatorOpen))) return false;
            offered = true;
            return true;
        }
        void ChooseSkyrim() { mode = Mode::Skyrim; selectedSex.reset(); }
        bool ChooseSex(std::uint32_t sex)
        {
            if (sex > 1) return false;
            selectedSex = sex;
            return true;
        }
        bool Finish()
        {
            if (!selectedSex) return false;
            mode = Mode::WoW;
            return true;
        }
        bool ShouldActivate(bool inWorld, bool creatorOpen, bool movement, bool fighting) const
        { return mode == Mode::WoW && inWorld && !creatorOpen && movement && fighting; }
        static bool Valid(std::uint32_t value) { return value <= static_cast<std::uint32_t>(Mode::WoW); }
    };
    inline std::string DisplayName(std::string_view name)
    {
        const auto begin = name.find_first_not_of(" \t\r\n");
        if (begin == name.npos) return "Dragonborn";
        const auto end = name.find_last_not_of(" \t\r\n");
        name = name.substr(begin, end - begin + 1);
        if (name == "Prisoner" || name == "Player") return "Dragonborn";
        return std::string(name);
    }
}
