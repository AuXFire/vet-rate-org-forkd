// Missions are data: a list of objectives completed in order, optional fail conditions, and a reward.
// Designers add missions in Content/Data/missions.json without touching code.
//
// Objective types:
//   goto           reach `pos` within `radius`
//   enter_vehicle  be in a vehicle whose tag matches `tag` ("" = any vehicle)
//   deliver        reach `pos` while in a vehicle tagged `tag`
//   collect        receive `count` notifications of Collected(tag)
//   eliminate      receive `count` notifications of Eliminated(tag)
//   lose_heat      have heat level 0
//   wait           stay alive for `seconds`
// Any objective may carry `timeLimit` (seconds) and `maxHeat` (fail if heat rises above it).
// Fail conditions (mission-wide): vehicle_destroyed(tag), player_down.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Core/UtEvents.h"
#include "Core/UtJson.h"
#include "Core/UtMath.h"

namespace ut {

enum class ObjectiveType : uint8_t { GoTo, EnterVehicle, Deliver, Collect, Eliminate, LoseHeat, Wait };

struct Objective {
    ObjectiveType type = ObjectiveType::GoTo;
    std::string text;
    std::string tag;
    Vec2 pos;
    float radius = 8.0f;
    int count = 1;
    float seconds = 0.0f;
    float timeLimit = 0.0f;   // 0 = none
    int maxHeat = -1;         // -1 = no cap
};

struct MissionDef {
    std::string id;
    std::string title;
    std::string giver;
    std::string intro;
    Vec2 start;                           // where the mission marker sits
    std::vector<std::string> requires_;   // mission ids that must be complete
    std::vector<Objective> objectives;
    std::vector<std::string> failOnVehicleDestroyed;
    bool failOnPlayerDown = true;
    int rewardCash = 0;
};

// What the mission system needs to know about the world each tick.
struct MissionContext {
    Vec2 playerPos;
    bool inVehicle = false;
    std::string vehicleTag;
    int heatLevel = 0;
};

enum class MissionNotice : uint8_t { Collected, Eliminated, VehicleDestroyed, PlayerDown };

class MissionSystem {
public:
    // Reads {"missions": [...]}. Returns false with `error` set if a mission is malformed.
    bool load(const Json& root, std::string* error = nullptr);

    const std::vector<MissionDef>& missions() const { return defs_; }
    const MissionDef* find(const std::string& id) const;
    bool isComplete(const std::string& id) const;
    // Unlocked, not yet complete, and not the one in progress.
    std::vector<const MissionDef*> available() const;

    bool start(const std::string& id, EventQueue& events);
    void abandon(EventQueue& events);
    void update(float dt, const MissionContext& ctx, EventQueue& events);
    void notify(MissionNotice notice, const std::string& tag, EventQueue& events);

    bool active() const { return active_ >= 0; }
    const MissionDef* activeMission() const { return active_ >= 0 ? &defs_[static_cast<size_t>(active_)] : nullptr; }
    int objectiveIndex() const { return objective_; }
    const Objective* currentObjective() const;
    // Seconds left on the current objective's time limit, or -1 if it has none.
    float timeRemaining() const;
    int counter() const { return counter_; }
    // Cash earned by missions completed since the last call (the simulation pays it into the wallet).
    int takePendingReward();

    Json save() const;
    void load_state(const Json& j);

private:
    void beginObjective(EventQueue& events);
    void completeObjective(EventQueue& events);
    void fail(const std::string& reason, EventQueue& events);

    std::vector<MissionDef> defs_;
    std::vector<std::string> completed_;
    int active_ = -1;
    int objective_ = 0;
    int counter_ = 0;
    float elapsed_ = 0.0f;
    int pendingReward_ = 0;
};

}  // namespace ut
