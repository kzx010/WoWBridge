#pragma once

namespace wowcore {
// A fresh native quote, in Skyrim skill points and septims. Native Train still
// owns payment, skill XP and the per-level counter; this guards stale UI requests.
struct TrainingOffer {
    int current, maximum, trained, limit, cost, gold;
    bool available() const {
        return current >= 0 && current < maximum && trained >= 0 && trained < limit &&
               cost >= 0 && gold >= cost;
    }
    bool matches(int expectedCurrent, int expectedCost, int expectedTrained) const {
        return available() && current == expectedCurrent && cost == expectedCost && trained == expectedTrained;
    }
};
}
