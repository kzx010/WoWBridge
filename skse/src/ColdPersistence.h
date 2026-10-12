#pragma once
#include <algorithm>
#include <string_view>

namespace ColdPersistence {
    inline bool VerifyOnly(std::string_view only) { return only=="wow_cold_persistence_verify"; }
    inline bool SafeSaveName(std::string_view name) {
        constexpr std::string_view prefix="WBColdPersistenceTest_";
        return name.starts_with(prefix) && name.size()>prefix.size() && name.size()<96 &&
            std::all_of(name.begin()+prefix.size(),name.end(),[](char c) { return (c>='0' && c<='9') || c=='_'; });
    }
    inline bool DefaultUi(bool autorun,bool profileReload,std::string_view only) {
        return autorun && !profileReload && !VerifyOnly(only);
    }
}
