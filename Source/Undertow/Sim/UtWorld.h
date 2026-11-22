// Roads for a full-scale Earth.
//
// The planet's road network is far too large to hold at once, so it is stored as tiles on the web-map grid
// and only the tiles around the player are loaded. WorldRoads keeps that set: tell it where the player is,
// it says which tiles it is missing, the host reads them (from disk, a pak file, a server) and hands them
// back, and tiles left far behind are dropped. Whenever the set changes, build() produces the RoadGraph the
// rest of the simulation drives on, in meters around the current anchor.
//
// Nodes carry ids that are the same in every tile they appear in (a road crossing a tile border has its
// border nodes in both tiles), which is how tiles stitch together and how traffic keeps its place when the
// graph is rebuilt.
//
// Hand-built areas ("regions", such as the Port Meridian test city) are held permanently and take part
// whenever the player is near them.
//
// Tile file format (JSON, written by Tools/osm_to_tiles.py):
//   {"z": 14, "x": 8186, "y": 5448, "left": 0,            (left: 1 where traffic keeps left)
//    "nodes": [[id, lat, lon, signal01], ...],
//    "edges": [[fromId, toId, lanes, speedMps, kindIndex, oneway01], ...]}
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "Sim/UtGeo.h"
#include "Sim/UtRoadGraph.h"

namespace ut {

struct TileNode {
    uint64_t id = 0;
    LatLon pos;
    bool signal = false;
};

struct TileEdge {
    uint64_t from = 0;
    uint64_t to = 0;
    int lanes = 1;                  // per direction
    float speed = 13.9f;            // m/s
    RoadKind kind = RoadKind::Street;
    bool oneWay = false;
};

struct TileData {
    bool leftHand = false;          // traffic keeps left in this tile
    std::vector<TileNode> nodes;
    std::vector<TileEdge> edges;
};

class WorldRoads {
public:
    int zoom = 14;                  // tile size: about 2.4 km at the equator, 1.5 km at 50 degrees
    double loadRadius = 1200.0;     // tiles within this many meters of the focus are wanted
    double keepRadius = 2600.0;     // loaded tiles further than this are dropped

    // Reads a tile file. Returns false with `error` set if it is malformed.
    static bool parseTile(const std::string& json, TileData& out, std::string* error = nullptr);
    // A made-up street grid for a tile, for tests and for trying the game where no real data has been
    // prepared. Deterministic and seamless across tile borders anywhere on Earth.
    static TileData proceduralTile(TileKey key, int cells = 8);
    // Ids for the nodes of hand-built regions, well clear of OpenStreetMap's id range.
    static uint64_t regionNodeId(int regionIndex, int nodeIndex);

    void clear();
    // A permanent area, included in the graph while the focus is within `radius` + loadRadius of `centre`.
    void addRegion(const std::string& name, LatLon centre, double radius, TileData data);
    int regionCount() const { return static_cast<int>(regions_.size()); }

    // Call when the player has moved. Updates missing() and drops distant tiles.
    void setFocus(LatLon focus);
    LatLon focus() const { return focus_; }
    // Wanted tiles that have not been provided yet, nearest first.
    const std::vector<TileKey>& missing() const { return missing_; }
    // Hands over a tile. An empty TileData means "nothing here" (open sea, desert) and stops it being asked for.
    void provide(TileKey key, TileData data);
    bool has(TileKey key) const { return tiles_.count(key) != 0; }
    int tileCount() const { return static_cast<int>(tiles_.size()); }

    // True when build() would produce a different graph from last time.
    bool dirty() const { return dirty_; }
    // Replaces `graph` with the loaded roads, positioned in `frame`.
    void build(RoadGraph& graph, const LocalFrame& frame);

private:
    struct Region {
        std::string name;
        LatLon centre;
        double radius = 0.0;
        TileData data;
        bool active = false;
    };

    static void addTo(RoadGraph& graph, const LocalFrame& frame, const TileData& data);

    std::vector<Region> regions_;
    std::map<TileKey, TileData> tiles_;
    std::vector<TileKey> missing_;
    std::vector<TileKey> scratch_;
    LatLon focus_;
    bool dirty_ = false;
};

}  // namespace ut
