#include "Sim/UtCrowd.h"

#include <algorithm>
#include <cmath>

namespace ut {

PedTraits Crowd::rollTraits(Rng& rng) {
    PedTraits t;
    // Averaging two rolls bunches people around the middle: most are ordinary, few are fearless or timid.
    t.boldness = (rng.nextFloat() + rng.nextFloat()) * 0.5f;
    t.civic = (rng.nextFloat() + rng.nextFloat()) * 0.5f;
    return t;
}

float Crowd::noticeRadius(Stimulus s) const {
    switch (s) {
        case Stimulus::Gunfire: return tuning.gunfireRadius;
        case Stimulus::Explosion: return tuning.explosionRadius;
        case Stimulus::VehicleThreat: return tuning.threatRadius;
        case Stimulus::Fight: return tuning.fightRadius;
        case Stimulus::Siren: return tuning.sirenRadius;
        case Stimulus::Corpse: return tuning.corpseRadius;
    }
    return 0.0f;
}

Reaction Crowd::react(const PedTraits& traits, Stimulus s, float dist, bool hasLineOfSight, Rng& rng) const {
    const float radius = noticeRadius(s);
    if (dist > radius) return Reaction::Ignore;
    const float closeness = 1.0f - dist / std::max(radius, 1.0f);   // 0 at the edge .. 1 on top of it

    if (s == Stimulus::Siren) return closeness > 0.3f ? Reaction::Watch : Reaction::Ignore;
    if (s == Stimulus::VehicleThreat) return Reaction::Flee;   // a car coming at you: everyone jumps

    // How frightening is this, for this person, at this distance?
    float danger = 0.0f;
    switch (s) {
        case Stimulus::Gunfire: danger = 0.75f; break;
        case Stimulus::Explosion: danger = 1.0f; break;
        case Stimulus::Fight: danger = 0.35f; break;
        case Stimulus::Corpse: danger = 0.45f; break;
        default: break;
    }
    const float fear = danger * (0.5f + 0.5f * closeness) - traits.boldness * 0.45f;
    const bool audible = s == Stimulus::Gunfire || s == Stimulus::Explosion;
    const bool canReport = hasLineOfSight || audible;

    if (fear > 0.45f) {
        // Too scared to think; people right next to gunfire sometimes freeze instead of running.
        if (audible && closeness > 0.8f && rng.chance(0.25f)) return Reaction::Cower;
        return Reaction::Flee;
    }
    if (canReport && rng.chance(traits.civic * (0.35f + 0.5f * danger))) return Reaction::Report;
    if (fear > 0.15f) return Reaction::Flee;
    return hasLineOfSight ? Reaction::Watch : Reaction::Ignore;
}

int Crowd::pedestrianBudget(float districtDensity, float daylight, float weatherFactor) const {
    // Streets never fully empty: at night there is still a quarter of the daytime crowd.
    const float timeFactor = 0.25f + 0.75f * saturate(daylight);
    const float n = static_cast<float>(tuning.maxPedestrians) * saturate(districtDensity) * timeFactor * saturate(weatherFactor);
    return static_cast<int>(std::lround(n));
}

}  // namespace ut
