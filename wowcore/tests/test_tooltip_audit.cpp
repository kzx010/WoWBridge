// The tooltip audit: every druid spell rank (action and aura tooltips, in Skyrim and WoW units), talent rank, book
// entry, imported gear item and Armory item rendered in the real GameTooltip, with the service's answers supplied as
// the live game supplies them (fixtures/tooltip_audit.json, kept current by service/tests/test_tooltip_audit.py),
// and checked for unresolved tokens and format specifiers, leftover markup, nil/NaN, empty, repeated and overflowing
// lines, misordered ranges, negative or zero amounts, numbers the service does not deal, heal or charge, and missing
// Skyrim lines (fixtures/tooltip_audit.lua). Justified exceptions are the allowlist below, each with its reason.
#include <catch_amalgamated.hpp>
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"
#include "wowcore/ui_session.hpp"
#include "wowcore/wowapi.hpp"
#include "wowcore/soft_raster.hpp"
extern "C" {
#include <lauxlib.h>
}
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <nlohmann/json.hpp>

namespace {
struct Allowed {
    const char* cls;
    const char* where;   // a substring of the defect's place ("" any)
    const char* text;    // a substring of its text ("" any)
    const char* reason;
};
// Justified exceptions only: what the client itself shows, or what cannot be compared.
const Allowed kAllowed[] = {
    {"fallback-service-mismatch", "without profile", "",
     "No service profile at all (only before its first answer): the client's skill-based guess. Every class spell has "
     "one once either the book's or the costs answer arrives (the spells and book parts check those numbers)."},
    {"token", "client description", "$z",
     "The player's home (bind point): WoWBridge has no inn binding, and no druid spell or Armory item uses it."},
    {"token", "client description Increased Pet", "$1%", "Malformed Spell.dbc text (no token letter); hunter pet talents."},
    {"token", "client description Ward of Laze", "$3826%", "Malformed Spell.dbc text (no token letter); an NPC ward."},
    {"duplicate-line", "Warblade of the Hakkari", "  Warblade of the Hakkari",
     "The set's two pieces (19865, 19866) share one name; the set lists each of its items, as ItemSet.dbc does."},
};

bool allowed(const std::string& cls, const std::string& where, const std::string& text) {
    for (const auto& a : kAllowed) {
        if (*a.cls && cls == a.cls && where.find(a.where) != std::string::npos && text.find(a.text) != std::string::npos) return true;
    }
    return false;
}

void push(lua_State* L, const nlohmann::json& value) {
    if (value.is_object() || value.is_array()) {
        lua_newtable(L);
        int index = 0;
        for (const auto& [key, v] : value.items()) {
            if (value.is_array()) lua_pushinteger(L, ++index);
            else lua_pushstring(L, key.c_str());
            push(L, v);
            lua_settable(L, -3);
        }
    } else if (value.is_string()) lua_pushstring(L, value.get<std::string>().c_str());
    else if (value.is_number()) lua_pushnumber(L, value.get<double>());
    else if (value.is_boolean()) lua_pushboolean(L, value.get<bool>());
    else lua_pushnil(L);
}

std::string field(lua_State* L, int table, const char* key) {
    lua_getfield(L, table, key);
    std::string out = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return out;
}
}

TEST_CASE("tooltip audit: every spell, talent, book, gear and Armory tooltip is clean and agrees with the service",
          "[tooltips][audit]") {
    const char* env = std::getenv("WOWBRIDGE_WOW_DATA");
    const std::filesystem::path data = env ? env : WOWBRIDGE_ROOT "/../WoW/client/Data";
    if (!std::filesystem::exists(data / "interface.MPQ")) SKIP("WoW 1.12 client data not present");
    const std::string root = WOWBRIDGE_ROOT;
    std::ifstream fixtureFile(root + "/wowcore/tests/fixtures/tooltip_audit.json");
    REQUIRE(fixtureFile.good());
    const auto fixture = nlohmann::json::parse(fixtureFile);
    auto mpq = std::make_shared<wowcore::Mpq>(data);
    const wowcore::ClientDb db(*mpq);
    wowcore::SoftTextures textures;
    wowcore::UiSession ui(mpq, db, textures, 2560, 1440);
    std::string err;
    REQUIRE(ui.start(&err));
    auto* L = ui.lua();
    auto run = [&](const std::string& script) {
        const int result = luaL_dostring(L, script.c_str());
        INFO((result ? lua_tostring(L, -1) : ""));
        REQUIRE(result == 0);
    };
    REQUIRE(luaL_dofile(L, (root + "/wowcore/tests/fixtures/elunara.lua").c_str()) == 0);
    run("__WBFixture('idle')");
    REQUIRE(ui.loadInterface(&err));
    run("__WBLogin()");

    // Items as Bags.cpp and Armory.cpp hand them over: the imported gear's rows as they are, and each Armory row
    // without its columns' defaults, with its Skyrim stats, quality, display and set (test_bags.cpp's recipe).
    auto items = nlohmann::json{{"gear", nlohmann::json::array()}, {"armory", nlohmann::json::array()}};
    bool haveGear = false, haveArmory = false;
    if (std::ifstream gearFile(root + "/build/gear.json"); gearFile.good()) {
        const auto gear = nlohmann::json::parse(gearFile);
        // Its gear, the class books and mount reins (Book forms, with what they teach) and the reagents (MiscItems).
        for (const char* list : {"items", "books", "mounts", "reagents"}) {
            for (const auto& [entry, row] : gear.at(list).items()) {
                auto item = row;
                item["display"] = row.at("displayid");
                item["key"] = entry;
                item["pos"] = 1;
                item["count"] = 1;
                if (std::string(list) == "books" || std::string(list) == "mounts") item["kind"] = "Book";
                if (std::string(list) == "reagents") item["kind"] = "Miscellaneous";
                items["gear"].push_back(item);
            }
        }
        haveGear = true;
    }
    if (std::ifstream armoryFile(root + "/build/armory.json"); armoryFile.good()) {
        const auto armory = nlohmann::json::parse(armoryFile);
        for (const auto& [entry, row] : armory.at("items").items()) {
            auto item = nlohmann::json{{"entry", std::stoi(entry)}, {"key", entry}, {"form", 0x02000800 + row.at("form").get<int>()},
                {"pos", 1}, {"count", 1}, {"name", row.at("wow").at("name")}, {"display", row.at("wow").at("displayid")},
                {"quality", row.at("wow").value("Quality", 1)}, {"wow", row.at("wow")}, {"skyrim", row.at("skyrim")},
                {"kind", row.at("kind") == "WEAP" ? "Weapon" : "Armor"}};
            if (row.contains("set")) item["set"] = armory.at("sets").at(std::to_string(row.at("set").get<int>()));
            items["armory"].push_back(item);
        }
        haveArmory = true;
    }
    push(L, fixture);
    lua_setglobal(L, "__tooltipAudit");
    push(L, items);
    lua_setglobal(L, "__tooltipAuditItems");
    REQUIRE(luaL_dofile(L, (root + "/wowcore/tests/fixtures/tooltip_audit.lua").c_str()) == 0);

    const bool verbose = std::getenv("WB_TOOLTIP_AUDIT_VERBOSE") != nullptr;
    auto audit = [&](const char* parts) {
        run(std::string("__auditIssues, __auditStats = __WBTooltipAudit({") + parts + "=true})");
        std::map<std::string, int> counts, allowedCounts;
        std::vector<std::string> failures;
        lua_getglobal(L, "__auditIssues");
        const int n = static_cast<int>(lua_objlen(L, -1));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            const int t = lua_gettop(L);
            const auto cls = field(L, t, "class"), where = field(L, t, "where"), text = field(L, t, "text");
            lua_pop(L, 1);
            if (allowed(cls, where, text)) { ++allowedCounts[cls]; continue; }
            ++counts[cls];
            if (counts[cls] <= (verbose ? 100000 : 25)) failures.push_back(cls + " | " + where + " | " + text);
        }
        lua_pop(L, 1);
        // What was rendered and found (shown on failure, or always with WB_TOOLTIP_AUDIT_VERBOSE=1).
        std::ostringstream report;
        report << "tooltip audit (" << parts << "):";
        lua_getglobal(L, "__auditStats");
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            report << " " << lua_tostring(L, -2) << "=" << lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        report << "\n";
        for (const auto& [cls, count] : counts) report << "  defect " << cls << ": " << count << "\n";
        for (const auto& [cls, count] : allowedCounts) report << "  allowed " << cls << ": " << count << "\n";
        for (const auto& f : failures) report << "    " << f << "\n";
        if (verbose) std::cout << report.str();
        INFO(report.str());
        CHECK(failures.empty());
    };
    SECTION("spells") { audit("spells"); }
    SECTION("talents") { audit("talents"); }
    SECTION("book") { audit("book"); }
    SECTION("client spells") { audit("client"); }
    SECTION("Skyrim items") { audit("native"); }
    SECTION("gear") {
        if (!haveGear) SKIP("build/gear.json not built");
        audit("gear");
    }
    SECTION("armory") {
        if (!haveArmory) SKIP("build/armory.json not built");
        audit("armory");
    }
}
