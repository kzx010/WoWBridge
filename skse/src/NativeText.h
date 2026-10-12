#pragma once
#include <cctype>
#include <initializer_list>
#include <utility>
#include <string>
#include <string_view>

namespace NativeText
{
    // Scaleform activation prompts can embed quest diamonds and font wrappers.
    // Remove only recognized presentation tags; preserve text and unknown tokens.
    inline std::string Activation(std::string_view input)
    {
        std::string out;
        for (std::size_t at=0; at<input.size();) {
            if (input[at]=='<') {
                std::size_t end=at+1; char quote=0;
                for (;end<input.size();++end) {
                    const char c=input[end];
                    if (quote) { if (c==quote) quote=0; }
                    else if (c=='\'' || c=='"') quote=c;
                    else if (c=='>') break;
                }
                if (end<input.size()) {
                    auto tag=input.substr(at+1,end-at-1);
                    while (!tag.empty() && (tag.front()=='/' || std::isspace(static_cast<unsigned char>(tag.front())))) tag.remove_prefix(1);
                    std::string name;
                    for (const auto c : tag) {
                        if (!std::isalpha(static_cast<unsigned char>(c))) break;
                        name+=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    const bool boundary=name.size()==tag.size() || (name.size()<tag.size() &&
                        (std::isspace(static_cast<unsigned char>(tag[name.size()])) || tag[name.size()]=='/'));
                    if (boundary && (name=="img" || name=="font" || name=="br")) {
                        if (name=="br") out+='\n';
                        at=end+1; continue;
                    }
                }
            }
            if (input[at]=='&') {
                bool decoded=false;
                for (const auto& [entity,text] : {std::pair<std::string_view,std::string_view>{"&nbsp;"," "},
                        {"&amp;","&"},{"&lt;","<"},{"&gt;",">"},{"&quot;","\""},{"&#39;","'"}}) {
                    if (input.substr(at,entity.size())==entity) { out+=text; at+=entity.size(); decoded=true; break; }
                }
                if (decoded) continue;
            }
            out+=input[at++];
        }
        return out;
    }
    inline std::string ItemDescription(std::string_view input, bool survivalEnabled)
    {
        auto out=Activation(input);
        // Skyrim's inventory movie conditionally displays these expanded clauses.
        // The native formatter resolves their values but retains the SURV wrapper.
        for (std::size_t at=0; (at=out.find("[SURV=",at))!=std::string::npos;) {
            const auto end=out.find(']',at+6);
            if (end==std::string::npos) break;
            const auto clause=survivalEnabled ? out.substr(at+6,end-at-6) : std::string{};
            out.replace(at,end-at+1,clause); at+=clause.size();
        }
        while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
        return out;
    }

}
