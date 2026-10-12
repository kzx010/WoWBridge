#pragma once

// Native Skyrim barter is authoritative. Only legacy-client cleanup remains.
namespace Merchant
{
    void Init();
    void Reset();
    void Command(std::string a_action, std::string a_key, int a_count,
        std::uint32_t a_session, std::uint32_t a_revision);
}
