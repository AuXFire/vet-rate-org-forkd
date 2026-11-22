#include "Core/UtRoadGraph.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace ut {

void RoadGraph::clear() {
    nodes_.clear();
    edges_.clear();
    edgeCells_.clear();
    nodeCells_.clear();
    maxSpeed_ = 1.0f;
}

int RoadGraph::addNode(Vec2 pos, bool signal) {
    RoadNode n;
    n.pos = pos;
    n.signal = signal;
    nodes_.push_back(n);
    return nodeCount() - 1;
}

int RoadGraph::addRoad(int a, int b, int lanes, float speed, RoadKind kind, bool oneWay) {
    if (a < 0 || b < 0 || a >= nodeCount() || b >= nodeCount() || a == b) return -1;
    const Vec2 delta = nodes_[static_cast<size_t>(b)].pos - nodes_[static_cast<size_t>(a)].pos;
    RoadEdge e;
    e.from = a;
    e.to = b;
    e.lanes = std::max(1, lanes);
    e.speed = std::max(1.0f, speed);
    e.length = std::max(0.5f, delta.length());
    e.kind = kind;
    e.dir = delta.normalized();
    const int forward = edgeCount();
    edges_.push_back(e);
    nodes_[static_cast<size_t>(a)].out.push_back(forward);
    nodes_[static_cast<size_t>(b)].in.push_back(forward);
    if (!oneWay) {
        RoadEdge r = e;
        r.from = b;
        r.to = a;
        r.dir = e.dir * -1.0f;
        r.reverse = forward;
        edges_[static_cast<size_t>(forward)].reverse = forward + 1;
        edges_.push_back(r);
        nodes_[static_cast<size_t>(b)].out.push_back(forward + 1);
        nodes_[static_cast<size_t>(a)].in.push_back(forward + 1);
    }
    maxSpeed_ = std::max(maxSpeed_, e.speed);
    return forward;
}

void RoadGraph::buildIndex() {
    edgeCells_.clear();
    nodeCells_.clear();
    for (int i = 0; i < edgeCount(); ++i) {
        const RoadEdge& e = edges_[static_cast<size_t>(i)];
        const Vec2 a = nodes_[static_cast<size_t>(e.from)].pos;
        const Vec2 b = nodes_[static_cast<size_t>(e.to)].pos;
        const int x0 = cellOf(std::min(a.x, b.x)), x1 = cellOf(std::max(a.x, b.x));
        const int y0 = cellOf(std::min(a.y, b.y)), y1 = cellOf(std::max(a.y, b.y));
        for (int cx = x0; cx <= x1; ++cx) {
            for (int cy = y0; cy <= y1; ++cy) edgeCells_[cellKey(cx, cy)].push_back(i);
        }
    }
    for (int i = 0; i < nodeCount(); ++i) {
        const Vec2 p = nodes_[static_cast<size_t>(i)].pos;
        nodeCells_[cellKey(cellOf(p.x), cellOf(p.y))].push_back(i);
    }
}

bool RoadGraph::load(const Json& roads, std::string* error) {
    clear();
    const Json& jn = roads["nodes"];
    const Json& je = roads["edges"];
    if (!jn.isArray() || !je.isArray()) {
        if (error) *error = "roads.nodes and roads.edges must be arrays";
        return false;
    }
    for (int i = 0; i < jn.size(); ++i) {
        const Json& n = jn.at(i);
        addNode(Vec2(n.at(0).asFloat(), n.at(1).asFloat()), n.at(2).asInt() != 0);
    }
    for (int i = 0; i < je.size(); ++i) {
        const Json& e = je.at(i);
        const int kind = std::min(std::max(e.at(4).asInt(1), 0), static_cast<int>(RoadKind::Bridge));
        if (addRoad(e.at(0).asInt(-1), e.at(1).asInt(-1), e.at(2).asInt(1), e.at(3).asFloat(13.9f),
                    static_cast<RoadKind>(kind), e.at(5).asInt() != 0) < 0) {
            if (error) *error = "edge " + std::to_string(i) + " references a missing node";
            clear();
            return false;
        }
    }
    buildIndex();
    return true;
}

Vec2 RoadGraph::lanePosition(int edgeIndex, float s, int lane) const {
    const RoadEdge& e = edge(edgeIndex);
    const Vec2 base = node(e.from).pos + e.dir * clampf(s, 0.0f, e.length);
    // Two-way roads: lanes sit to the right of the centre line. One-way roads: lanes are centred on it.
    const float laneOffset = e.reverse >= 0 ? (static_cast<float>(lane) + 0.5f)
                                            : (static_cast<float>(lane) + 0.5f - static_cast<float>(e.lanes) * 0.5f);
    return base + e.dir.right() * (laneOffset * kLaneWidth);
}

bool RoadGraph::findPath(int fromNode, int toNode, std::vector<int>& outEdges) const {
    outEdges.clear();
    if (fromNode < 0 || toNode < 0 || fromNode >= nodeCount() || toNode >= nodeCount()) return false;
    if (fromNode == toNode) return true;

    const size_t n = nodes_.size();
    std::vector<float> cost(n, 1e30f);
    std::vector<int> via(n, -1);   // edge used to reach each node
    struct Open {
        float f;
        int nodeIndex;
        bool operator<(const Open& o) const { return f > o.f; }   // min-heap
    };
    std::priority_queue<Open> open;
    const Vec2 goal = nodes_[static_cast<size_t>(toNode)].pos;
    cost[static_cast<size_t>(fromNode)] = 0.0f;
    open.push({distance(nodes_[static_cast<size_t>(fromNode)].pos, goal) / maxSpeed_, fromNode});

    while (!open.empty()) {
        const Open cur = open.top();
        open.pop();
        if (cur.nodeIndex == toNode) break;
        const float g = cost[static_cast<size_t>(cur.nodeIndex)];
        if (cur.f - distance(nodes_[static_cast<size_t>(cur.nodeIndex)].pos, goal) / maxSpeed_ > g + 1e-3f) continue;   // stale entry
        for (const int ei : nodes_[static_cast<size_t>(cur.nodeIndex)].out) {
            const RoadEdge& e = edges_[static_cast<size_t>(ei)];
            const float ng = g + e.length / e.speed;
            if (ng < cost[static_cast<size_t>(e.to)]) {
                cost[static_cast<size_t>(e.to)] = ng;
                via[static_cast<size_t>(e.to)] = ei;
                open.push({ng + distance(nodes_[static_cast<size_t>(e.to)].pos, goal) / maxSpeed_, e.to});
            }
        }
    }
    if (via[static_cast<size_t>(toNode)] < 0) return false;
    for (int at = toNode; at != fromNode;) {
        const int ei = via[static_cast<size_t>(at)];
        outEdges.push_back(ei);
        at = edges_[static_cast<size_t>(ei)].from;
    }
    std::reverse(outEdges.begin(), outEdges.end());
    return true;
}

float RoadGraph::travelTime(const std::vector<int>& edgeList) const {
    float t = 0.0f;
    for (const int ei : edgeList) t += edge(ei).length / edge(ei).speed;
    return t;
}

void RoadGraph::edgesNear(Vec2 pos, float radius, std::vector<int>& out) const {
    out.clear();
    const int x0 = cellOf(pos.x - radius), x1 = cellOf(pos.x + radius);
    const int y0 = cellOf(pos.y - radius), y1 = cellOf(pos.y + radius);
    for (int cx = x0; cx <= x1; ++cx) {
        for (int cy = y0; cy <= y1; ++cy) {
            const auto it = edgeCells_.find(cellKey(cx, cy));
            if (it == edgeCells_.end()) continue;
            out.insert(out.end(), it->second.begin(), it->second.end());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

int RoadGraph::nearestEdge(Vec2 pos, float maxDist, float* outS, float* outDist) const {
    int best = -1;
    float bestD2 = maxDist * maxDist;
    float bestS = 0.0f;
    const int x0 = cellOf(pos.x - maxDist), x1 = cellOf(pos.x + maxDist);
    const int y0 = cellOf(pos.y - maxDist), y1 = cellOf(pos.y + maxDist);
    for (int cx = x0; cx <= x1; ++cx) {
        for (int cy = y0; cy <= y1; ++cy) {
            const auto it = edgeCells_.find(cellKey(cx, cy));
            if (it == edgeCells_.end()) continue;
            for (const int ei : it->second) {
                const RoadEdge& e = edges_[static_cast<size_t>(ei)];
                float t = 0.0f;
                const Vec2 c = closestOnSegment(nodes_[static_cast<size_t>(e.from)].pos, nodes_[static_cast<size_t>(e.to)].pos, pos, &t);
                const float d2 = distanceSq(c, pos);
                // Of the two directions of a road, prefer the one whose lanes are on this side.
                const bool rightSide = (pos - c).dot(e.dir.right()) >= 0.0f;
                if (d2 < bestD2 - 1e-4f || (best >= 0 && std::fabs(d2 - bestD2) <= 1e-4f && rightSide)) {
                    bestD2 = d2;
                    best = ei;
                    bestS = t * e.length;
                }
            }
        }
    }
    if (best >= 0) {
        if (outS) *outS = bestS;
        if (outDist) *outDist = std::sqrt(bestD2);
    }
    return best;
}

int RoadGraph::nearestNode(Vec2 pos, float maxDist) const {
    int best = -1;
    float bestD2 = maxDist * maxDist;
    const int x0 = cellOf(pos.x - maxDist), x1 = cellOf(pos.x + maxDist);
    const int y0 = cellOf(pos.y - maxDist), y1 = cellOf(pos.y + maxDist);
    for (int cx = x0; cx <= x1; ++cx) {
        for (int cy = y0; cy <= y1; ++cy) {
            const auto it = nodeCells_.find(cellKey(cx, cy));
            if (it == nodeCells_.end()) continue;
            for (const int ni : it->second) {
                const float d2 = distanceSq(nodes_[static_cast<size_t>(ni)].pos, pos);
                if (d2 <= bestD2) {
                    bestD2 = d2;
                    best = ni;
                }
            }
        }
    }
    return best;
}

void RoadGraph::nodesInRing(Vec2 pos, float minDist, float maxDist, std::vector<int>& out) const {
    out.clear();
    const float min2 = minDist * minDist, max2 = maxDist * maxDist;
    const int x0 = cellOf(pos.x - maxDist), x1 = cellOf(pos.x + maxDist);
    const int y0 = cellOf(pos.y - maxDist), y1 = cellOf(pos.y + maxDist);
    for (int cx = x0; cx <= x1; ++cx) {
        for (int cy = y0; cy <= y1; ++cy) {
            const auto it = nodeCells_.find(cellKey(cx, cy));
            if (it == nodeCells_.end()) continue;
            for (const int ni : it->second) {
                const float d2 = distanceSq(nodes_[static_cast<size_t>(ni)].pos, pos);
                if (d2 >= min2 && d2 <= max2) out.push_back(ni);
            }
        }
    }
    std::sort(out.begin(), out.end());   // deterministic order regardless of hash iteration
}

int RoadGraph::randomNextEdge(int edgeIndex, Rng& rng) const {
    const RoadEdge& e = edge(edgeIndex);
    const std::vector<int>& out = node(e.to).out;
    float total = 0.0f;
    for (const int ei : out) {
        if (ei != e.reverse) total += static_cast<float>(edge(ei).lanes);   // busier roads attract more traffic
    }
    if (total <= 0.0f) return e.reverse;   // dead end: turn around (or -1 on a one-way stub)
    float r = rng.nextFloat() * total;
    int last = -1;
    for (const int ei : out) {
        if (ei == e.reverse) continue;
        last = ei;
        r -= static_cast<float>(edge(ei).lanes);
        if (r < 0.0f) return ei;
    }
    return last;
}

}  // namespace ut
