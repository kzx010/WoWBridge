#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace wowcore {
// Logical places, independent of Skyrim's inventory ordering and the currently visible page.
// Positions are one-based: page*80 + bag*16 + slot. No compaction on removal.
class BagPlaces {
public:
    static constexpr uint32_t slotsPerBag = 16, slotsPerPage = 80;
    std::map<std::string, uint32_t> places;

    void reconcile(const std::vector<std::string>& live) {
        const std::set<std::string> present(live.begin(), live.end());
        for (auto i = places.begin(); i != places.end();)
            if (!present.contains(i->first)) i = places.erase(i); else ++i;
        std::set<uint32_t> occupied;
        for (const auto& [key, slot] : places) occupied.insert(slot);
        uint32_t free = 1;
        for (const auto& key : live) if (!places.contains(key)) {
            while (occupied.contains(free)) ++free;
            places[key] = free;
            occupied.insert(free);
        }
    }
    std::string at(uint32_t slot) const {
        for (const auto& [key, pos] : places) if (pos == slot) return key;
        return {};
    }
    uint32_t pages() const {
        uint32_t high = 1;
        for (const auto& [key, pos] : places) high = std::max(high, pos);
        return (high - 1) / slotsPerPage + 1;
    }
    // Expected target guards asynchronous commands against looting/consumption since the UI snapshot.
    bool move(const std::string& key, uint32_t to, const std::string& expected) {
        const auto it = places.find(key);
        if (it == places.end() || !to || to > pages() * slotsPerPage || at(to) != expected) return false;
        const auto from = it->second;
        if (!expected.empty()) places[expected] = from;
        places[key] = to;
        return true;
    }
    // Co-save loading rejects duplicate/invalid places instead of losing an item behind another one.
    bool restore(const std::map<std::string, uint32_t>& saved) {
        std::set<uint32_t> occupied;
        for (const auto& [key, pos] : saved)
            if (key.empty() || !pos || pos > 1000000 || !occupied.insert(pos).second) return false;
        places = saved;
        return true;
    }
};

// A pile has a stable UI key independent of its Skyrim identity. A partial cursor pile has pos=0;
// a whole cursor pile reserves its old place, preserving the client's locked-slot appearance.
class BagPiles {
public:
    struct Pile {
        std::string identity;
        uint32_t pos = 0;
        int32_t count = 0;
        bool operator==(const Pile&) const = default;
    };
    std::map<std::string, Pile> piles;
    std::string held;
    uint32_t origin = 0;
    // A character-slot pickup reserves a copy without changing Skyrim equipment.
    std::optional<uint32_t> equippedSlot;

    std::string at(uint32_t pos) const {
        if (pos) for (const auto& [key, pile] : piles) if (pile.pos == pos) return key;
        return {};
    }
    uint32_t free() const {
        uint32_t pos = 1;
        while (!at(pos).empty()) ++pos;
        return pos;
    }
    uint32_t pages() const {
        uint32_t high = 1;
        for (const auto& [key, pile] : piles) high = std::max(high, pile.pos);
        auto pages = (high - 1) / BagPlaces::slotsPerPage + 1;
        if (piles.contains(held) && !piles.at(held).pos && free() > pages * BagPlaces::slotsPerPage) ++pages;
        return pages;
    }
    bool contains(const std::string& identity) const {
        for (const auto& [key, pile] : piles) if (pile.identity == identity) return true;
        return false;
    }
    void rename(const std::string& from, const std::string& to) {
        for (auto& [key, pile] : piles) if (pile.identity == from) pile.identity = to;
    }
    int64_t total(const std::string& identity) const {
        int64_t n = 0;
        for (const auto& [key, pile] : piles) if (pile.identity == identity) n += pile.count;
        return n;
    }
    // Skyrim is authoritative. Acquisitions join the earliest pile; removal drains later places,
    // then the cursor. Explicit bag use/drop calls debit() first to select the affected pile.
    void reconcile(const std::vector<std::pair<std::string, int32_t>>& live) {
        std::set<std::string> present;
        for (const auto& [identity, count] : live) {
            if (count <= 0) continue;
            present.insert(identity);
            std::vector<std::string> keys;
            for (const auto& [key, pile] : piles) if (pile.identity == identity) keys.push_back(key);
            std::ranges::sort(keys, [&](const auto& a, const auto& b) {
                const auto pa = a == held ? 0 : piles.at(a).pos;
                const auto pb = b == held ? 0 : piles.at(b).pos;
                return pa != pb ? pa > pb : a < b;
            });
            auto delta = int64_t{count} - total(identity);
            if (delta > 0) {
                if (keys.empty()) {
                    std::string key = identity;
                    while (piles.contains(key)) key += ":pile";
                    piles[key] = {identity, free(), count};
                } else {
                    auto target = keys.rend();
                    for (auto it = keys.rbegin(); it != keys.rend(); ++it) if (*it != held) { target = it; break; }
                    if (target != keys.rend()) piles.at(*target).count += static_cast<int32_t>(delta);
                    else {
                        std::string key = identity;
                        while (piles.contains(key)) key += ":pile";
                        piles[key] = {identity, free(), static_cast<int32_t>(delta)};
                    }
                }
            } else for (const auto& key : keys) {
                if (delta == 0) break;
                auto& pile = piles.at(key);
                const auto n = std::min<int64_t>(pile.count, -delta);
                pile.count -= static_cast<int32_t>(n); delta += n;
            }
        }
        for (auto it = piles.begin(); it != piles.end();)
            if (!present.contains(it->second.identity) || it->second.count <= 0) it = piles.erase(it); else ++it;
        if (!piles.contains(held)) { held.clear(); origin = 0; equippedSlot.reset(); }
    }
    bool grab(const std::string& key, int32_t count, const std::string& splitKey) {
        auto it = piles.find(key);
        if (!held.empty() || it == piles.end() || count <= 0 || count > it->second.count) return false;
        origin = it->second.pos;
        if (count == it->second.count) held = key;
        else {
            if (splitKey.empty() || piles.contains(splitKey)) { origin = 0; return false; }
            it->second.count -= count;
            piles[splitKey] = {it->second.identity, 0, count}; held = splitKey;
        }
        return true;
    }
    bool grabEquipped(const std::string& key, uint32_t slot, const std::string& splitKey) {
        if (!grab(key, 1, splitKey)) return false;
        equippedSlot = slot;
        return true;
    }
    bool place(const std::string& key, uint32_t to, const std::string& expected) {
        if (held != key || !piles.contains(key) || !to || to > pages() * BagPlaces::slotsPerPage || at(to) != expected) return false;
        equippedSlot.reset();
        auto& source = piles.at(key);
        if (expected == key) { cancel(); return true; }
        if (!expected.empty() && piles.at(expected).identity == source.identity) {
            piles.at(expected).count += source.count;
            piles.erase(key); held.clear(); origin = 0; equippedSlot.reset();
        } else {
            const auto from = source.pos;
            source.pos = to;
            held = expected;
            if (!expected.empty()) { piles.at(expected).pos = from; if (from) origin = from; }
            else origin = 0;
            // A completed swap releases both piles. A partial pickup has no old
            // place, so cancel() parks the displaced pile in the first free one.
            cancel();
        }
        return true;
    }
    void cancel() {
        if (piles.contains(held)) {
            auto& pile = piles.at(held);
            if (!pile.pos) {
                const auto target = at(origin);
                if (!target.empty() && piles.at(target).identity == pile.identity) {
                    piles.at(target).count += pile.count; piles.erase(held);
                } else pile.pos = target.empty() && origin ? origin : free();
            }
        }
        held.clear(); origin = 0; equippedSlot.reset();
    }
    void debit(const std::string& key, int32_t n) {
        auto it = piles.find(key);
        if (it == piles.end() || n <= 0) return;
        it->second.count -= std::min(n, it->second.count);
        if (!it->second.count) { piles.erase(it); if (held == key) { held.clear(); origin = 0; equippedSlot.reset(); } }
    }
    // Apply only after Skyrim confirms equipment replacement. The incoming
    // identity must still reserve its original place; an intervening bag move
    // invalidates the request instead of moving another item.
    bool returnEquipment(uint32_t from, const std::string& incoming, const std::string& outgoing) {
        const auto source=at(from);
        if (source.empty() || piles.at(source).identity!=incoming || incoming==outgoing) return false;
        auto target=piles.end();
        for (auto it=piles.begin();it!=piles.end();++it)
            if (it->second.identity==outgoing && it->second.pos && it->first!=held &&
                (target==piles.end() || it->second.pos<target->second.pos)) target=it;
        if (target==piles.end()) return false;
        std::swap(piles.at(source).pos,target->second.pos);
        return true;
    }
    // v1 migration alone allows zero counts: the first reconciliation fills the original place.
    bool restore(const std::map<std::string, Pile>& saved, bool legacy = false) {
        std::set<uint32_t> occupied;
        for (const auto& [key, pile] : saved)
            if (key.empty() || pile.identity.empty() || !pile.pos || pile.pos > 1000000 ||
                pile.count < (legacy ? 0 : 1) || !occupied.insert(pile.pos).second) return false;
        piles = saved; held.clear(); origin = 0; equippedSlot.reset();
        return true;
    }
};
}
