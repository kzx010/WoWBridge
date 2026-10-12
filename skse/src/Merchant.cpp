#include "Merchant.h"
#include "ui/WowUi.h"

namespace Merchant
{
    void Init()
    {
        // Skyrim owns its visible barter movie, flags, input and transactions.
        // ItemPreview's independent native-menu hook still renders imported gear.
        logger::info("merchant: native Skyrim barter enabled (no replacement hooks)");
    }

    void Reset() { WowUi::Run("__WBMerchant(nil)"); }

    void Command(std::string, std::string, int, std::uint32_t, std::uint32_t)
    {
        // Retained for old/debug Lua callers; they cannot transact or close a
        // native trade. The classic shim remains available to offline fixtures.
        logger::warn("merchant: ignored legacy WoW merchant command; use Skyrim barter");
        Reset();
    }
}
