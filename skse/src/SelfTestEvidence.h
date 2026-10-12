#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace SelfTest::Evidence
{
    inline bool DamageObserved(float before, float after, float requested, float tolerance = 1)
    {
        return requested > tolerance && after < before - tolerance && std::abs(before - after - requested) <= tolerance;
    }

    inline bool StunObserved(bool activeCarrier, bool paralyzed, double remaining,
                             double minimum = 1, double maximum = 3)
    {
        return activeCarrier && paralyzed && remaining > minimum && remaining <= maximum;
    }

    inline bool ChargeObserved(bool root, bool lockout, double remaining, float speed,
                               float moved, float landingYards, float currentYards,
                               int paidSpell, float paidCost, int power)
    {
        return root && lockout && remaining > 3 && remaining <= 4 && speed < 1 &&
            moved > 0 && landingYards <= 2.01f && currentYards <= 5 &&
            paidSpell == 16979 && std::abs(paidCost - 5) < .01f && power == 95;
    }

    inline bool StatGainObserved(bool profileReady, float before, float after, float talented,
                                 float baseline, float scale, float formBonus = 0)
    {
        const float expected = (talented - baseline) * scale;
        return profileReady && expected > 0 && std::abs(after - before - formBonus - expected) < .1f;
    }

    inline bool ManaPaymentObserved(int spell, int paidSpell, float cost, float paidCost)
    {
        return spell == paidSpell && cost > 0 && std::abs(cost - paidCost) < .01f;
    }

    // Measure the damaged actor, not an assumed before-minus-100 value. Native
    // carriers can change maximum health between frames; regen alone is not a heal.
    inline bool HealingObserved(float damaged, float after, float requested, float maximum, float tolerance = 3)
    {
        const float expected = std::min(requested, maximum - damaged);
        return expected > tolerance && after > damaged + tolerance && std::abs(after - damaged - expected) <= tolerance;
    }

    inline bool ReflectionObserved(bool castStarted, float rate, float restoredBefore, float restoredAfter,
                                   float manaBefore, float manaAfter)
    {
        return castStarted && rate > 0 && restoredAfter > restoredBefore && manaAfter > manaBefore + 0.1f;
    }

    // Native SWF text is already localized. Require all three distinct attribute
    // fields, a prompt body and a visible movie, without guessed settings/AS types.
    inline bool LevelChoices(bool movieVisible, std::string_view body, const std::array<std::string, 3>& labels)
    {
        return movieVisible && !body.empty() && !labels[0].empty() && !labels[1].empty() && !labels[2].empty() &&
            labels[0] != labels[1] && labels[0] != labels[2] && labels[1] != labels[2];
    }
}
