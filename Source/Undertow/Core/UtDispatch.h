// Police dispatch: decides how many units respond to the player's heat, where they come from, and where
// each one is trying to go. It does not drive them. In Unreal each unit is a physical car steered by
// UtDriver along the unit's path, and the game reports its position back with reportUnit(). In the
// headless simulation units move along their path at a fixed speed.
#pragma once

#include <cstdint>
#include <vector>

#include "Core/UtEvents.h"
#include "Core/UtHeat.h"
#include "Core/UtJson.h"
#include "Core/UtRng.h"
#include "Core/UtRoadGraph.h"

namespace ut {

enum class UnitRole : uint8_t { Pursue, Search, StandDown };

struct PoliceUnit {
    int id = 0;
    UnitRole role = UnitRole::Pursue;
    Vec2 pos;
    float heading = 0.0f;
    float speed = 0.0f;
    Vec2 goal;
    std::vector<Vec2> path;       // world points from the unit to its goal, along roads
    int pathVersion = 0;          // bumps whenever `path` is replaced, so the game knows to re-feed its driver
    float replanIn = 0.0f;
    float standDownTimer = 0.0f;
    float pathProgress = 0.0f;    // headless movement only
    bool external = false;        // true once the game drives this unit and reports its position
};

struct DispatchTuning {
    int unitsPerLevel[kMaxHeatLevel + 1] = {0, 1, 2, 3, 5, 7};
    float spawnMin = 150.0f;
    float spawnMax = 280.0f;
    float spawnInterval = 4.0f;     // seconds between new units
    float pursuitSpeed = 30.0f;
    float searchSpeed = 16.0f;
    float sightRange = 65.0f;       // headless line-of-sight stand-in; the game does real traces
    float replanInterval = 1.5f;
    float standDownSeconds = 12.0f;

    void load(const Json& j);
};

class DispatchSystem {
public:
    DispatchTuning tuning;

    void setGraph(const RoadGraph* graph) { graph_ = graph; }
    void update(float dt, const HeatSystem& heat, Vec2 playerPos, Rng& rng, EventQueue& events);
    // The game calls this every tick for units it has spawned as physical vehicles.
    void reportUnit(int id, Vec2 pos, float heading, float speed);
    // A unit was destroyed or abandoned.
    void removeUnit(int id, EventQueue& events);
    void clear(EventQueue& events);

    const std::vector<PoliceUnit>& units() const { return units_; }
    // Distance-only visibility check for headless runs. `visibility` scales range in fog and rain.
    bool anyUnitNear(Vec2 playerPos, float visibility = 1.0f) const;

private:
    void plan(PoliceUnit& unit, Vec2 goal);
    void moveHeadless(PoliceUnit& unit, float dt);

    const RoadGraph* graph_ = nullptr;
    std::vector<PoliceUnit> units_;
    std::vector<int> scratchNodes_;
    std::vector<int> scratchEdges_;
    float spawnTimer_ = 0.0f;
    int nextId_ = 1;
};

}  // namespace ut
