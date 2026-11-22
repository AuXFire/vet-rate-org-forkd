// Heat: how much police attention the player has, and how they shake it.
//
// Crimes only count when someone sees them. A police witness adds heat at once; civilian witnesses phone it in
// after a delay (more witnesses, shorter delay), which gives the player a window to leave. Heat points map to
// levels 0-5. While police can see the player the state is Pursuit; once they lose sight it becomes Search: a
// zone grows around the last known position and an evade timer runs (twice as fast outside the zone). When the
// timer fills, heat clears.
#pragma once

#include <cstdint>
#include <vector>

#include "Sim/UtEvents.h"
#include "Sim/UtJson.h"
#include "Sim/UtMath.h"

namespace ut {

enum class Crime : uint8_t {
    Trespass,
    RecklessDriving,
    PropertyDamage,
    VehicleTheft,
    Assault,
    Robbery,
    Gunfire,
    VehicularAssault,
    AssaultOnOfficer,
    Count
};

const char* crimeName(Crime c);

enum class HeatState : uint8_t { Clean, Pursuit, Search };

constexpr int kMaxHeatLevel = 5;

struct HeatTuning {
    float points[static_cast<int>(Crime::Count)] = {2.0f, 3.0f, 5.0f, 12.0f, 14.0f, 30.0f, 22.0f, 35.0f, 60.0f};
    float thresholds[kMaxHeatLevel + 1] = {0.0f, 10.0f, 30.0f, 70.0f, 130.0f, 220.0f};   // points needed for each level
    float evadeSeconds[kMaxHeatLevel + 1] = {0.0f, 20.0f, 35.0f, 55.0f, 80.0f, 110.0f};  // unseen time needed to clear
    float reportDelay = 7.0f;            // seconds for a lone civilian witness to call it in
    float searchRadiusStart = 60.0f;
    float searchRadiusGrowth = 4.0f;     // meters per second
    float searchRadiusMax = 400.0f;
    float minorDecay = 0.5f;             // points per second lost while below level 1

    void load(const Json& j);            // missing fields keep their defaults
};

class HeatSystem {
public:
    HeatTuning tuning;

    // `seenByPolice`: an officer witnessed it. `civilianWitnesses`: bystanders with line of sight.
    void reportCrime(Crime crime, Vec2 pos, bool seenByPolice, int civilianWitnesses, EventQueue& events);
    // Drops pending civilian reports made within `radius` of `pos` (the witnesses were scared off or left).
    void cancelPendingReports(Vec2 pos, float radius);
    void update(float dt, Vec2 playerPos, bool seenByPolice, EventQueue& events);
    void clear(EventQueue& events);

    int level() const { return level_; }
    float points() const { return points_; }
    HeatState state() const { return state_; }
    Vec2 lastKnownPosition() const { return lastKnown_; }
    float searchRadius() const { return searchRadius_; }
    bool inSearchZone(Vec2 pos) const { return state_ == HeatState::Search && distance(pos, lastKnown_) <= searchRadius_; }
    // 0..1 progress toward clearing heat; drives the HUD's flashing indicator.
    float evadeProgress() const;
    int pendingReports() const { return static_cast<int>(pending_.size()); }

    Json save() const;
    void load(const Json& j);

private:
    struct Pending {
        Crime crime;
        Vec2 pos;
        float timer;
    };

    void addPoints(Crime crime, Vec2 pos, bool seenByPolice, EventQueue& events);
    void setState(HeatState s, EventQueue& events);
    int levelForPoints(float pts) const;

    float points_ = 0.0f;
    int level_ = 0;
    HeatState state_ = HeatState::Clean;
    Vec2 lastKnown_;
    float searchRadius_ = 0.0f;
    float evadeTimer_ = 0.0f;
    std::vector<Pending> pending_;
};

}  // namespace ut
