// Road network: nodes (intersections) joined by directed edges (one per driving direction).
// Used by ambient traffic, police dispatch routing and GPS route drawing.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Sim/UtJson.h"
#include "Sim/UtMath.h"
#include "Sim/UtRng.h"

namespace ut {

enum class RoadKind : uint8_t { Alley, Street, Arterial, Highway, Bridge };

struct RoadNode {
    Vec2 pos;
    bool signal = false;            // traffic lights here
    std::vector<int> out;           // outgoing edge indices
    std::vector<int> in;            // incoming edge indices
};

struct RoadEdge {
    int from = 0;
    int to = 0;
    int lanes = 1;                  // lanes in this direction
    float speed = 13.9f;            // limit, m/s
    float length = 0.0f;
    RoadKind kind = RoadKind::Street;
    int reverse = -1;               // opposite-direction edge, or -1 for one-way
    Vec2 dir;                       // unit vector from -> to
};

class RoadGraph {
public:
    static constexpr float kLaneWidth = 3.5f;

    // Reads the "roads" object of city.json:
    //   nodes: [[x, y, signal01], ...]   edges: [[a, b, lanes, speed, kindIndex, oneway01], ...]
    bool load(const Json& roads, std::string* error = nullptr);
    void clear();
    int addNode(Vec2 pos, bool signal = false);
    // Adds a->b and, unless `oneWay`, b->a. Returns the index of the a->b edge.
    int addRoad(int a, int b, int lanes, float speed, RoadKind kind = RoadKind::Street, bool oneWay = false);
    void buildIndex();              // call after the last addRoad()

    const std::vector<RoadNode>& nodes() const { return nodes_; }
    const std::vector<RoadEdge>& edges() const { return edges_; }
    const RoadNode& node(int i) const { return nodes_[static_cast<size_t>(i)]; }
    const RoadEdge& edge(int i) const { return edges_[static_cast<size_t>(i)]; }
    int nodeCount() const { return static_cast<int>(nodes_.size()); }
    int edgeCount() const { return static_cast<int>(edges_.size()); }

    // World position `s` meters along an edge, in the given lane (0 = nearest the centre line).
    Vec2 lanePosition(int edgeIndex, float s, int lane) const;
    // Fastest route by travel time. Fills `outEdges` and returns true if `toNode` is reachable.
    bool findPath(int fromNode, int toNode, std::vector<int>& outEdges) const;
    // Seconds to drive a list of edges at the speed limit.
    float travelTime(const std::vector<int>& edgeList) const;
    // Nearest edge within `maxDist`, or -1. `outS` = meters along it.
    int nearestEdge(Vec2 pos, float maxDist, float* outS = nullptr, float* outDist = nullptr) const;
    int nearestNode(Vec2 pos, float maxDist) const;
    // Edges with any part within `radius` of `pos` (approximate: by grid cell).
    void edgesNear(Vec2 pos, float radius, std::vector<int>& out) const;
    void nodesInRing(Vec2 pos, float minDist, float maxDist, std::vector<int>& out) const;
    // A plausible continuation at the end of `edgeIndex`: avoids U-turns unless it is a dead end. -1 if none.
    int randomNextEdge(int edgeIndex, Rng& rng) const;

private:
    static constexpr float kCell = 100.0f;
    static uint64_t cellKey(int cx, int cy) { return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32U) | static_cast<uint64_t>(static_cast<uint32_t>(cy)); }
    static int cellOf(float v) { return static_cast<int>(std::floor(v / kCell)); }

    std::vector<RoadNode> nodes_;
    std::vector<RoadEdge> edges_;
    std::unordered_map<uint64_t, std::vector<int>> edgeCells_;
    std::unordered_map<uint64_t, std::vector<int>> nodeCells_;
    float maxSpeed_ = 1.0f;
};

}  // namespace ut
