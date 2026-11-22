// Ambient traffic: cars that follow lanes around the player. Only a ring around the player is simulated;
// agents are spawned ahead of the ring's inner edge and removed once they fall far behind.
//
// Each agent keeps a distance along its edge and follows the car in front with the Intelligent Driver Model
// (smooth acceleration, brakes for slower leaders, keeps a time headway). Red lights and anything the game
// reports as an obstacle (the player's car, police, a roadblock) act as stopped or moving leaders.
// The Unreal layer mirrors agents() onto pooled vehicle actors each frame.
#pragma once

#include <cstdint>
#include <vector>

#include "Core/UtEvents.h"
#include "Core/UtJson.h"
#include "Core/UtRng.h"
#include "Core/UtRoadGraph.h"

namespace ut {

struct TrafficAgent {
    int id = 0;
    int edge = 0;
    int nextEdge = -1;
    int lane = 0;
    float s = 0.0f;              // meters along `edge`
    float speed = 0.0f;
    float desiredFactor = 1.0f;  // this driver's speed relative to the limit
    float length = 4.6f;
    float panic = 0.0f;          // seconds left of fleeing (ignores lights, drives fast)
    uint8_t archetype = 0;       // index into the vehicle list; the Unreal layer picks the mesh
    Vec2 pos;
    float heading = 0.0f;
};

struct TrafficTuning {
    int maxAgents = 110;
    float spawnMin = 110.0f;     // never spawn closer than this to the focus
    float spawnMax = 300.0f;
    float despawn = 380.0f;
    float accel = 1.9f;          // comfortable acceleration, m/s^2
    float brake = 2.6f;          // comfortable braking, m/s^2
    float headway = 1.3f;        // seconds
    float minGap = 2.4f;         // meters bumper to bumper at rest
    float signalCycle = 36.0f;   // seconds for a full green/red cycle
    int archetypes = 8;

    void load(const Json& j);
};

class TrafficSystem {
public:
    TrafficTuning tuning;

    void setGraph(const RoadGraph* graph);
    // `focus` is the player's position; `density` scales the agent budget (time of day, weather, district).
    void update(float dt, Vec2 focus, float density, Rng& rng, EventQueue& events);
    // Obstacles last for one update; report them every tick before update().
    void addObstacle(Vec2 pos, Vec2 velocity, float radius);
    // Drivers within `radius` flee for `seconds` (gunfire, explosions).
    void panicAt(Vec2 pos, float radius, float seconds);
    void clear(EventQueue& events);

    const std::vector<TrafficAgent>& agents() const { return agents_; }
    int count() const { return static_cast<int>(agents_.size()); }
    bool signalGreen(int edgeIndex) const;
    float time() const { return time_; }

private:
    struct Obstacle {
        int edge;
        float s;
        float speed;
    };

    void spawn(Vec2 focus, int wanted, Rng& rng, EventQueue& events);
    float leaderGap(const TrafficAgent& a, float& leaderSpeed) const;
    void rebuildBuckets();

    const RoadGraph* graph_ = nullptr;
    std::vector<TrafficAgent> agents_;
    std::vector<std::vector<int>> byEdge_;     // agent indices on each edge
    std::vector<int> touchedEdges_;
    std::vector<Obstacle> obstacles_;
    std::vector<int> spawnCandidates_;
    float candidatesAge_ = 1e9f;
    Vec2 candidatesFocus_;
    float time_ = 0.0f;
    int nextId_ = 1;
    bool warm_ = false;
};

}  // namespace ut
