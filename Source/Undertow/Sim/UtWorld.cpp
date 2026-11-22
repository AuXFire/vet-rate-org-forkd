#include "Sim/UtWorld.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace ut {
namespace world_detail {

constexpr uint64_t kRegionIdBase = 1ULL << 52U;
constexpr uint64_t kProceduralIdBase = 1ULL << 51U;

RoadKind kindFromIndex(int i) { return static_cast<RoadKind>(std::min(std::max(i, 0), static_cast<int>(RoadKind::Bridge))); }

}  // namespace world_detail

bool WorldRoads::parseTile(const std::string& json, TileData& out, std::string* error) {
    out = TileData();
    Json root;
    if (!Json::parse(json, root, error)) return false;
    const Json& jn = root["nodes"];
    const Json& je = root["edges"];
    if (!root.isObject() || !jn.isArray() || !je.isArray()) {
        if (error) *error = "a tile needs \"nodes\" and \"edges\" arrays";
        return false;
    }
    out.leftHand = root["left"].asInt() != 0;
    std::unordered_set<uint64_t> ids;
    out.nodes.reserve(static_cast<size_t>(jn.size()));
    for (int i = 0; i < jn.size(); ++i) {
        const Json& n = jn.at(i);
        TileNode node;
        node.id = static_cast<uint64_t>(n.at(0).asInt64());
        node.pos = LatLon(n.at(1).asDouble(), n.at(2).asDouble());
        node.signal = n.at(3).asInt() != 0;
        if (n.size() < 3 || node.id == 0 || !(std::fabs(node.pos.lat) <= 90.0) || !(std::fabs(node.pos.lon) <= 180.0)) {
            if (error) *error = "node " + std::to_string(i) + " is not [id, lat, lon, signal]";
            out = TileData();
            return false;
        }
        ids.insert(node.id);
        out.nodes.push_back(node);
    }
    out.edges.reserve(static_cast<size_t>(je.size()));
    for (int i = 0; i < je.size(); ++i) {
        const Json& e = je.at(i);
        TileEdge edge;
        edge.from = static_cast<uint64_t>(e.at(0).asInt64());
        edge.to = static_cast<uint64_t>(e.at(1).asInt64());
        edge.lanes = std::min(std::max(e.at(2).asInt(1), 1), 8);
        edge.speed = clampf(e.at(3).asFloat(13.9f), 1.0f, 60.0f);
        edge.kind = world_detail::kindFromIndex(e.at(4).asInt(1));
        edge.oneWay = e.at(5).asInt() != 0;
        if (edge.from == edge.to || ids.count(edge.from) == 0 || ids.count(edge.to) == 0) {
            if (error) *error = "edge " + std::to_string(i) + " references a node that is not in the tile";
            out = TileData();
            return false;
        }
        out.edges.push_back(edge);
    }
    return true;
}

uint64_t WorldRoads::regionNodeId(int regionIndex, int nodeIndex) {
    return world_detail::kRegionIdBase + (static_cast<uint64_t>(static_cast<uint32_t>(regionIndex)) << 32U) +
           static_cast<uint64_t>(static_cast<uint32_t>(nodeIndex));
}

TileData WorldRoads::proceduralTile(TileKey key, int cells) {
    // Streets run along lines of a grid fixed to the web-map projection, `cells` to a tile. The projection keeps
    // angles true, so the blocks are square everywhere, and because the grid is global the lines of one tile
    // continue in the next. Every fourth line is a wider, faster road with signals where two of them cross.
    TileData tile;
    cells = std::min(std::max(cells, 1), 64);
    const int64_t perSide = static_cast<int64_t>(cells) << key.z;        // grid lines around the world
    const double total = static_cast<double>(perSide);
    auto gridX = [&](int i) { return ((static_cast<int64_t>(key.x) * cells + i) % perSide + perSide) % perSide; };
    auto gridY = [&](int j) { return static_cast<int64_t>(key.y) * cells + j; };
    auto idOf = [&](int i, int j) {
        return world_detail::kProceduralIdBase + (static_cast<uint64_t>(gridX(i)) << 24U) + static_cast<uint64_t>(gridY(j));
    };
    for (int j = 0; j <= cells; ++j) {
        for (int i = 0; i <= cells; ++i) {
            TileNode n;
            n.id = idOf(i, j);
            const double fx = static_cast<double>(static_cast<int64_t>(key.x) * cells + i) / total;
            const double fy = static_cast<double>(gridY(j)) / total;
            n.pos.lon = geo::wrapLon(fx * 360.0 - 180.0);
            n.pos.lat = std::atan(std::sinh(geo::kPiD * (1.0 - 2.0 * fy))) * 180.0 / geo::kPiD;
            n.signal = gridX(i) % 4 == 0 && gridY(j) % 4 == 0;
            tile.nodes.push_back(n);
        }
    }
    auto addEdge = [&](int i0, int j0, int i1, int j1, bool major) {
        TileEdge e;
        e.from = idOf(i0, j0);
        e.to = idOf(i1, j1);
        e.lanes = major ? 2 : 1;
        e.speed = major ? 16.7f : 13.9f;
        e.kind = major ? RoadKind::Arterial : RoadKind::Street;
        tile.edges.push_back(e);
    };
    for (int j = 0; j <= cells; ++j) {
        for (int i = 0; i <= cells; ++i) {
            if (i < cells) addEdge(i, j, i + 1, j, gridY(j) % 4 == 0);
            if (j < cells) addEdge(i, j, i, j + 1, gridX(i) % 4 == 0);
        }
    }
    return tile;
}

void WorldRoads::clear() {
    regions_.clear();
    tiles_.clear();
    missing_.clear();
    dirty_ = true;
}

void WorldRoads::addRegion(const std::string& name, LatLon centre, double radius, TileData data) {
    Region r;
    r.name = name;
    r.centre = centre;
    r.radius = radius;
    r.data = std::move(data);
    r.active = geo::distance(focus_, centre) <= radius + loadRadius;
    regions_.push_back(std::move(r));
    dirty_ = true;
}

void WorldRoads::setFocus(LatLon focus) {
    focus_ = focus;
    for (Region& r : regions_) {
        const bool active = geo::distance(focus, r.centre) <= r.radius + loadRadius;
        if (active != r.active) {
            r.active = active;
            dirty_ = true;
        }
    }
    // Drop tiles that are no longer near.
    geo::tilesAround(focus, std::max(keepRadius, loadRadius), zoom, scratch_);
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        if (!std::binary_search(scratch_.begin(), scratch_.end(), it->first)) {
            if (!it->second.edges.empty()) dirty_ = true;
            it = tiles_.erase(it);
        } else {
            ++it;
        }
    }
    // What is wanted but absent, nearest first so the host loads the tile under the player before the rest.
    geo::tilesAround(focus, loadRadius, zoom, scratch_);
    missing_.clear();
    for (const TileKey& k : scratch_) {
        if (tiles_.count(k) == 0) missing_.push_back(k);
    }
    auto centreDistance = [&](const TileKey& k) {
        LatLon nw, se;
        geo::tileBounds(k, nw, se);
        return geo::distance(focus, LatLon((nw.lat + se.lat) * 0.5, nw.lon + (se.lon - nw.lon) * 0.5));
    };
    std::sort(missing_.begin(), missing_.end(), [&](const TileKey& a, const TileKey& b) {
        const double da = centreDistance(a), db = centreDistance(b);
        return da != db ? da < db : a < b;
    });
}

void WorldRoads::provide(TileKey key, TileData data) {
    if (key.z != zoom) return;
    missing_.erase(std::remove(missing_.begin(), missing_.end(), key), missing_.end());
    const auto found = tiles_.find(key);
    const bool hadRoads = found != tiles_.end() && !found->second.edges.empty();
    if (hadRoads || !data.edges.empty()) dirty_ = true;
    tiles_[key] = std::move(data);
}

void WorldRoads::addTo(RoadGraph& graph, const LocalFrame& frame, const TileData& data) {
    for (const TileNode& n : data.nodes) graph.addNode(frame.toLocal(n.pos), n.signal, n.id);
    for (const TileEdge& e : data.edges) {
        const int a = graph.nodeById(e.from), b = graph.nodeById(e.to);
        if (a < 0 || b < 0 || a == b) continue;
        // A road on a tile border is in both tiles: add it once.
        if (graph.findEdge(a, b) >= 0 || (!e.oneWay && graph.findEdge(b, a) >= 0)) continue;
        graph.addRoad(a, b, e.lanes, e.speed, e.kind, e.oneWay, data.leftHand);
    }
}

void WorldRoads::build(RoadGraph& graph, const LocalFrame& frame) {
    graph.clear();
    // Regions first and in a fixed order, so a region's nodes keep the same indices while it is loaded.
    for (const Region& r : regions_) {
        if (r.active) addTo(graph, frame, r.data);
    }
    for (const auto& entry : tiles_) addTo(graph, frame, entry.second);
    graph.buildIndex();
    dirty_ = false;
}

}  // namespace ut
