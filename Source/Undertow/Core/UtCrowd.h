// Bystanders: how many pedestrians the game should keep around the player, and how each one reacts when
// something happens nearby. Movement and animation belong to the engine; this decides *what* they do.
#pragma once

#include <cstdint>

#include "Core/UtMath.h"
#include "Core/UtRng.h"

namespace ut {

enum class Stimulus : uint8_t { Gunfire, Explosion, VehicleThreat, Fight, Siren, Corpse };
enum class Reaction : uint8_t { Ignore, Watch, Cower, Flee, Report };

// Personality, rolled once per pedestrian when it spawns.
struct PedTraits {
    float boldness = 0.5f;   // 0 runs from everything .. 1 stands and watches
    float civic = 0.5f;      // likelihood of phoning the police
};

struct CrowdTuning {
    int maxPedestrians = 60;
    float gunfireRadius = 70.0f;
    float explosionRadius = 120.0f;
    float threatRadius = 18.0f;
    float fightRadius = 22.0f;
    float sirenRadius = 45.0f;
    float corpseRadius = 15.0f;
};

class Crowd {
public:
    CrowdTuning tuning;

    static PedTraits rollTraits(Rng& rng);
    // How far away this kind of event is noticed at all.
    float noticeRadius(Stimulus s) const;
    // What a pedestrian does about an event `dist` meters away. `hasLineOfSight` matters for Report:
    // nobody phones in something they only heard, except gunfire and explosions.
    Reaction react(const PedTraits& traits, Stimulus s, float dist, bool hasLineOfSight, Rng& rng) const;
    // Pedestrian budget for the player's surroundings.
    //   districtDensity: 0..1 from the city data   daylight: 0..1   weatherFactor: WeatherSystem::pedestrianFactor()
    int pedestrianBudget(float districtDensity, float daylight, float weatherFactor) const;
};

}  // namespace ut
