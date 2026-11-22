// Full-scale Earth: streamed road tiles, moving the anchor, and the whole simulation crossing the globe.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "Sim/UtSim.h"
#include "ut_test.h"

using namespace ut;

namespace {

std::string readFile(const char* name) {
    const std::string path = std::string(UT_DATA_DIR) + "/" + name;
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

// Stands in for the host's tile loader: every tile the world asks for gets a made-up street grid.
void feedTiles(Simulation& sim) {
    const std::vector<TileKey> wanted = sim.world.missing();
    for (const TileKey& k : wanted) sim.world.provide(k, WorldRoads::proceduralTile(k));
}

// A player who drives along the roads in a fixed compass direction, moving the anchor when asked to.
struct Traveller {
    Simulation& sim;
    PlayerInput in;
    Vec2 bearing;
    uint64_t target = 0;       // id of the junction being driven toward
    int rebases = 0;
    int lostRoad = 0;
    int maxTiles = 0;
    int minTraffic = 1 << 30;
    int strayAgents = 0;
    int badEdges = 0;
    int ticks = 0;

    Traveller(Simulation& s, Vec2 dir) : sim(s), bearing(dir.normalized()) { in.inVehicle = true; }

    void pickNext(int node) {
        float best = -2.0f;
        for (const int ei : sim.roads.node(node).out) {
            const float d = sim.roads.edge(ei).dir.dot(bearing);
            if (d > best) {
                best = d;
                target = sim.roads.node(sim.roads.edge(ei).to).id;
            }
        }
    }

    void step(float dt, float speed) {
        feedTiles(sim);
        if (sim.wantsRebase(in.pos)) {
            const FrameMap map = sim.setAnchor(sim.frame().toGeo(in.pos));
            in.pos = map.point(in.pos);
            bearing = map.direction(bearing);
            ++rebases;
        }
        int node = target ? sim.roads.nodeById(target) : sim.roads.nearestNode(in.pos, 600.0f);
        if (node < 0) {
            lostRoad += ticks > 2 ? 1 : 0;   // the first tiles arrive during the first tick
        } else {
            if (!target) target = sim.roads.node(node).id;
            const Vec2 to = sim.roads.node(node).pos - in.pos;
            const float d = to.length(), travel = speed * dt;
            if (d <= travel) {
                in.pos = sim.roads.node(node).pos;
                pickNext(node);
            } else {
                in.velocity = to / d * speed;
                in.pos += in.velocity * dt;
            }
        }
        sim.tick(dt, in);
        ++ticks;
        maxTiles = std::max(maxTiles, sim.world.tileCount());
        if (ticks > 200) minTraffic = std::min(minTraffic, sim.traffic.count());
        for (const TrafficAgent& a : sim.traffic.agents()) {
            strayAgents += distance(a.pos, in.pos) > sim.traffic.tuning.despawn + 60.0f ? 1 : 0;
            badEdges += a.edge < 0 || a.edge >= sim.roads.edgeCount() ? 1 : 0;
        }
    }
};

}  // namespace

UT_TEST(world, tile_files_parse_and_bad_ones_are_rejected) {
    TileData tile;
    std::string err;
    UT_CHECK(WorldRoads::parseTile(R"({"z":14,"x":8186,"y":5448,
        "nodes":[[101,51.5074001,-0.1278001,1],[102,51.5080123,-0.1270456,0],[103,51.5090,-0.1260,0]],
        "edges":[[101,102,2,13.4,2,0],[102,103,1,8.9,1,1]]})", tile, &err));
    UT_CHECK_EQ(static_cast<int>(tile.nodes.size()), 3);
    UT_CHECK_EQ(static_cast<int>(tile.edges.size()), 2);
    UT_CHECK_NEAR(tile.nodes[0].pos.lat, 51.5074001, 1e-12);
    UT_CHECK_NEAR(tile.nodes[1].pos.lon, -0.1270456, 1e-12);
    UT_CHECK(tile.nodes[0].signal && !tile.nodes[1].signal);
    UT_CHECK(tile.edges[0].kind == RoadKind::Arterial && tile.edges[0].lanes == 2 && !tile.edges[0].oneWay);
    UT_CHECK(tile.edges[1].oneWay);
    // OpenStreetMap ids are already past 2^33: they must survive as exact integers.
    UT_CHECK(WorldRoads::parseTile(R"({"nodes":[[12345678901,1,2,0],[12345678902,1.001,2,0]],"edges":[[12345678901,12345678902,1,10,1,0]]})", tile, &err));
    UT_CHECK(tile.nodes[0].id == 12345678901ULL && tile.edges[0].to == 12345678902ULL);

    UT_CHECK(!WorldRoads::parseTile(R"({"nodes":[[1,1,2,0]],"edges":[[1,99,1,10,1,0]]})", tile, &err));
    UT_CHECK(err.find("edge 0") != std::string::npos);
    UT_CHECK(tile.nodes.empty());
    UT_CHECK(!WorldRoads::parseTile(R"({"nodes":[[1,95.0,2,0]],"edges":[]})", tile, &err));
    UT_CHECK(!WorldRoads::parseTile(R"({"nodes":[[0,1,2,0]],"edges":[]})", tile, &err));
    UT_CHECK(!WorldRoads::parseTile("{\"nodes\": 4}", tile, &err));
    UT_CHECK(!WorldRoads::parseTile("not json", tile, &err));
}

UT_TEST(world, neighbouring_tiles_stitch_into_one_network) {
    const TileKey a = geo::tileAt(LatLon(48.137, 11.575), 14);
    TileKey b = a;
    b.x += 1;
    const TileData ta = WorldRoads::proceduralTile(a), tb = WorldRoads::proceduralTile(b);
    UT_CHECK_EQ(static_cast<int>(ta.nodes.size()), 81);
    int shared = 0;
    for (const TileNode& n : ta.nodes) {
        for (const TileNode& m : tb.nodes) {
            if (n.id != m.id) continue;
            ++shared;
            UT_CHECK(geo::distance(n.pos, m.pos) < 0.001);   // the same junction in both files
        }
    }
    UT_CHECK_EQ(shared, 9);

    WorldRoads world;
    world.setFocus(LatLon(48.137, 11.575));
    world.provide(a, ta);
    world.provide(b, tb);
    RoadGraph graph;
    world.build(graph, LocalFrame(LatLon(48.137, 11.575)));
    UT_CHECK_EQ(graph.nodeCount(), 17 * 9);                  // border junctions counted once
    UT_CHECK_EQ(graph.edgeCount(), 2 * (16 * 9 + 17 * 8));   // and border roads once
    // Blocks come out square and the same size in both tiles.
    for (const RoadEdge& e : graph.edges()) UT_CHECK_NEAR(e.length, graph.edge(0).length, 1.5);
    // A route from the far west of one tile to the far east of the other.
    int west = 0, east = 0;
    for (int i = 0; i < graph.nodeCount(); ++i) {
        if (graph.node(i).pos.x < graph.node(west).pos.x) west = i;
        if (graph.node(i).pos.x > graph.node(east).pos.x) east = i;
    }
    std::vector<int> path;
    UT_CHECK(graph.findPath(west, east, path));
    UT_CHECK(path.size() >= 16);
}

UT_TEST(world, streaming_asks_for_nearby_tiles_and_drops_distant_ones) {
    WorldRoads world;
    const LatLon start(35.6812, 139.7671);
    world.setFocus(start);
    UT_CHECK(!world.missing().empty());
    UT_CHECK(world.missing().front() == geo::tileAt(start, world.zoom));   // the tile underfoot first
    const size_t wanted = world.missing().size();
    UT_CHECK(wanted >= 4 && wanted <= 16);
    const std::vector<TileKey> first = world.missing();
    for (const TileKey& k : first) world.provide(k, WorldRoads::proceduralTile(k));
    UT_CHECK(world.missing().empty());
    UT_CHECK(world.dirty());
    RoadGraph graph;
    world.build(graph, LocalFrame(start));
    UT_CHECK(!world.dirty());
    UT_CHECK(graph.nearestNode(Vec2(0, 0), 400.0f) >= 0);
    // Every road within the load radius is there.
    for (int i = 0; i < 12; ++i) {
        const float a = static_cast<float>(i) * 0.5236f;
        UT_CHECK(graph.nearestEdge(Vec2::fromAngle(a) * 1100.0f, 300.0f) >= 0);
    }
    // Nothing changes while standing still.
    world.setFocus(start);
    UT_CHECK(world.missing().empty() && !world.dirty());
    // An empty tile (open sea) counts as provided and does not force a rebuild.
    const LatLon away = LocalFrame(start).toGeo(20000.0, 0.0);
    world.setFocus(away);
    UT_CHECK_EQ(world.tileCount(), 0);                       // the old area was dropped
    UT_CHECK(world.dirty());
    world.build(graph, LocalFrame(away));
    UT_CHECK_EQ(graph.nodeCount(), 0);
    const std::vector<TileKey> second = world.missing();
    for (const TileKey& k : second) world.provide(k, TileData());
    UT_CHECK(world.missing().empty() && !world.dirty());
}

UT_TEST(world, port_meridian_sits_on_the_globe) {
    Simulation sim(3);
    std::string err;
    UT_CHECK(sim.loadCity(readFile("city.json"), &err) && sim.loadMissions(readFile("missions.json"), &err) &&
             sim.loadEconomy(readFile("economy.json"), &err));
    UT_CHECK(sim.worldEnabled());
    UT_CHECK_EQ(sim.roads.nodeCount(), 2083);
    UT_CHECK(sim.roads.node(0).id == WorldRoads::regionNodeId(0, 0));
    const LatLon anchor = sim.frame().anchor();
    UT_CHECK(std::fabs(anchor.lat) > 1.0 && std::fabs(anchor.lon) > 1.0);
    const MissionDef* first = sim.missions.find("m01_slack_water");
    UT_CHECK(first != nullptr);
    const Vec2 startBefore = first->start;
    const Poi* lockup = &sim.city.pois().front();
    const Vec2 poiBefore = lockup->pos;
    UT_CHECK(geo::distance(lockup->geo, anchor) < 6000.0);

    // Travel to the far side of the planet: the city is no longer part of the world there.
    sim.setAnchor(LatLon(-anchor.lat, anchor.lon + 180.0));
    PlayerInput in;
    for (int i = 0; i < 30; ++i) sim.tick(0.05f, in);   // no tiles arrive: the world settles after a second
    UT_CHECK_EQ(sim.roads.nodeCount(), 0);
    UT_CHECK(sim.currentDistrict() == nullptr);
    // Come back: everything is where it was.
    sim.setAnchor(anchor);
    in.pos = startBefore;
    for (int i = 0; i < 30; ++i) sim.tick(0.05f, in);
    UT_CHECK_EQ(sim.roads.nodeCount(), 2083);
    UT_CHECK(distance(first->start, startBefore) < 0.01f);
    UT_CHECK(distance(lockup->pos, poiBefore) < 0.01f);
    UT_CHECK(sim.currentDistrict() != nullptr);
}

UT_TEST(world, moving_the_anchor_keeps_everything_in_place) {
    Simulation sim(11);
    std::string err;
    UT_CHECK(sim.loadCity(readFile("city.json"), &err) && sim.loadTuning(readFile("tuning.json"), &err) &&
             sim.loadMissions(readFile("missions.json"), &err));
    PlayerInput in;
    in.pos = sim.missions.find("m01_slack_water")->start;
    in.inVehicle = true;
    for (int i = 0; i < 100; ++i) sim.tick(0.05f, in);
    sim.reportCrime(Crime::Robbery, in.pos, true, 0);
    for (int i = 0; i < 400; ++i) sim.tick(0.05f, in);
    UT_CHECK(sim.heat.level() >= 1);
    UT_CHECK(!sim.dispatch.units().empty());
    UT_CHECK(sim.traffic.count() > 10);
    const District* district = sim.currentDistrict();

    std::map<int, Vec2> carsBefore;
    for (const TrafficAgent& a : sim.traffic.agents()) carsBefore[a.id] = a.pos;
    std::vector<float> unitDistances;
    for (const PoliceUnit& u : sim.dispatch.units()) unitDistances.push_back(distance(u.pos, in.pos));
    const float lastKnownDistance = distance(sim.heat.lastKnownPosition(), in.pos);
    const LatLon playerGeo = sim.frame().toGeo(in.pos);
    const float objectiveDistance = distance(sim.missions.find("m01_slack_water")->objectives[0].pos, in.pos);
    sim.drainEvents();

    const FrameMap map = sim.setAnchor(sim.frame().toGeo(in.pos + Vec2(900.0f, -400.0f)));
    in.pos = map.point(in.pos);

    UT_CHECK(geo::distance(sim.frame().toGeo(in.pos), playerGeo) < 0.01);
    UT_CHECK_NEAR(in.pos.x, -900.0f, 0.5);   // not exactly: the two frames are turned a few thousandths of a degree apart
    UT_CHECK_NEAR(in.pos.y, 400.0f, 0.5);
    // Every car is still there, on its road, exactly where the map says it should be.
    UT_CHECK_EQ(sim.traffic.count(), static_cast<int>(carsBefore.size()));
    for (const TrafficAgent& a : sim.traffic.agents()) UT_CHECK(distance(a.pos, map.point(carsBefore[a.id])) < 0.02f);
    UT_CHECK_EQ(sim.pendingEvents().count(EventType::TrafficRemoved), 0);
    UT_CHECK_EQ(sim.pendingEvents().count(EventType::RoadsChanged), 1);
    // Police, the search centre and mission targets keep their distance to the player.
    for (size_t i = 0; i < unitDistances.size(); ++i) UT_CHECK_NEAR(distance(sim.dispatch.units()[i].pos, in.pos), unitDistances[i], 0.01);
    UT_CHECK_NEAR(distance(sim.heat.lastKnownPosition(), in.pos), lastKnownDistance, 0.01);
    UT_CHECK_NEAR(distance(sim.missions.find("m01_slack_water")->objectives[0].pos, in.pos), objectiveDistance, 0.01);
    // And the game carries on: same district, the chase continues, traffic keeps flowing.
    for (int i = 0; i < 200; ++i) sim.tick(0.05f, in);
    UT_CHECK(sim.currentDistrict() == district);
    UT_CHECK(sim.heat.level() >= 1);
    UT_CHECK(sim.traffic.count() > 10);
    for (const PoliceUnit& u : sim.dispatch.units()) UT_CHECK(distance(u.pos, in.pos) < 600.0f);
}

UT_TEST(world, a_forty_kilometre_drive_across_tiles) {
    Simulation sim(21);
    const LatLon start(51.5074, -0.1278);
    sim.enableWorld(start);
    Traveller t(sim, Vec2(1.0f, 0.0f));
    const float dt = 0.05f, speed = 35.0f;
    const int ticks = 24000;                                   // 20 minutes at 126 km/h
    for (int i = 0; i < ticks; ++i) t.step(dt, speed);

    const LatLon end = sim.frame().toGeo(t.in.pos);
    const double travelled = geo::distance(start, end);
    UT_CHECK_NEAR(travelled, static_cast<double>(speed * dt * static_cast<float>(ticks)), 1500.0);
    UT_CHECK_NEAR(end.lat, start.lat, 0.003);                  // the road east follows a parallel
    UT_CHECK(end.lon > start.lon + 0.5);
    UT_CHECK(t.rebases >= 20);
    UT_CHECK_EQ(t.lostRoad, 0);                                // there was always road ahead
    UT_CHECK(t.maxTiles <= 30);                                // old tiles are dropped, memory stays flat
    UT_CHECK(t.minTraffic >= 5);                               // traffic never died out
    UT_CHECK_EQ(t.strayAgents, 0);
    UT_CHECK_EQ(t.badEdges, 0);
    UT_CHECK(t.in.pos.length() <= sim.rebaseDistance + 50.0f); // local coordinates stayed small the whole way
    UT_CHECK(sim.roadsVersion() >= 40);
}

UT_TEST(world, a_chase_survives_tile_changes_and_rebases) {
    Simulation sim(5);
    sim.enableWorld(LatLon(40.7580, -73.9855));
    Traveller t(sim, Vec2(0.0f, 1.0f));
    for (int i = 0; i < 200; ++i) t.step(0.05f, 15.0f);
    sim.reportCrime(Crime::AssaultOnOfficer, t.in.pos, true, 0);
    sim.reportCrime(Crime::AssaultOnOfficer, t.in.pos, true, 0);
    t.in.hasSightResult = true;                                // the police never lose sight: a pursuit throughout
    t.in.seenByPolice = true;
    int unitsSeen = 0, far = 0, closeTicks = 0;
    for (int i = 0; i < 6000; ++i) {                           // five minutes, about 5 km
        if (i % 600 == 0) sim.reportCrime(Crime::Gunfire, t.in.pos, true, 0);
        t.step(0.05f, 17.0f);
        unitsSeen = std::max(unitsSeen, static_cast<int>(sim.dispatch.units().size()));
        bool close = false;
        for (const PoliceUnit& u : sim.dispatch.units()) {
            far += distance(u.pos, t.in.pos) > 1200.0f ? 1 : 0;
            close = close || distance(u.pos, t.in.pos) < 150.0f;
        }
        closeTicks += close ? 1 : 0;
    }
    UT_CHECK(t.rebases >= 3);
    UT_CHECK(unitsSeen >= 2);
    UT_CHECK_EQ(far, 0);                                       // no unit was left behind in an old frame
    UT_CHECK(sim.heat.level() >= 2);
    UT_CHECK(closeTicks > 4500);                               // they kept up through every rebuild
}

UT_TEST(world, driving_across_the_antimeridian) {
    Simulation sim(8);
    sim.enableWorld(LatLon(-17.0, 179.97));                    // just west of the date line
    Traveller t(sim, Vec2(1.0f, 0.0f));
    for (int i = 0; i < 6000; ++i) t.step(0.05f, 30.0f);       // 9 km east
    const LatLon end = sim.frame().toGeo(t.in.pos);
    UT_CHECK(end.lon < -179.9 && end.lon > -180.0);
    UT_CHECK_NEAR(geo::distance(LatLon(-17.0, 179.97), end), 9000.0, 600.0);
    UT_CHECK_EQ(t.lostRoad, 0);
    UT_CHECK(t.minTraffic >= 3);
    UT_CHECK_EQ(t.strayAgents, 0);
}

UT_TEST(world, driving_in_the_far_north) {
    Simulation sim(9);
    const LatLon start(69.6492, 18.9553);                      // Tromso
    sim.enableWorld(start);
    Traveller t(sim, Vec2(0.0f, 1.0f));
    for (int i = 0; i < 8000; ++i) t.step(0.05f, 25.0f);       // 10 km north
    const LatLon end = sim.frame().toGeo(t.in.pos);
    UT_CHECK_NEAR(geo::distance(start, end), 10000.0, 500.0);
    UT_CHECK(end.lat > start.lat + 0.08);
    UT_CHECK_NEAR(end.lon, start.lon, 0.01);                   // the road north follows a meridian
    UT_CHECK_EQ(t.lostRoad, 0);
    UT_CHECK(t.rebases >= 5);
    UT_CHECK(t.minTraffic >= 3);
}

UT_TEST(world, missions_can_be_placed_by_latitude_and_longitude) {
    Simulation sim(4);
    const LatLon start(48.8584, 2.2945);
    sim.enableWorld(start);
    const LatLon target = LocalFrame(start).toGeo(2600.0, 1900.0);
    char json[512];
    std::snprintf(json, sizeof(json),
                  R"({"missions":[{"id":"far","title":"Far","startGeo":[%.8f,%.8f],"reward":{"cash":500},
                      "objectives":[{"type":"goto","text":"Get there","geo":[%.8f,%.8f],"radius":15}]}]})",
                  start.lat, start.lon, target.lat, target.lon);
    std::string err;
    UT_CHECK(sim.loadMissions(json, &err));
    const MissionDef* def = sim.missions.find("far");
    UT_CHECK_NEAR(def->objectives[0].pos.x, 2600.0f, 0.01);
    UT_CHECK_NEAR(def->objectives[0].pos.y, 1900.0f, 0.01);
    UT_CHECK(sim.startMission("far"));

    // Head for it in a straight line, rebasing on the way (twice: it is 3.2 km off).
    PlayerInput in;
    int rebases = 0;
    bool done = false;
    for (int i = 0; i < 4000 && !done; ++i) {
        if (sim.wantsRebase(in.pos)) {
            in.pos = sim.setAnchor(sim.frame().toGeo(in.pos)).point(in.pos);
            ++rebases;
        }
        in.pos += (def->objectives[0].pos - in.pos).normalized() * 1.5f;
        sim.tick(0.05f, in);
        for (const Event& e : sim.drainEvents()) done = done || e.type == EventType::MissionCompleted;
    }
    UT_CHECK(done);
    UT_CHECK(rebases >= 2);
    UT_CHECK(geo::distance(sim.playerGeo(), target) < 16.0);
    UT_CHECK_EQ(sim.economy.cash(), 500);
}

UT_TEST(world, saves_remember_where_on_earth_the_player_is) {
    Simulation sim(6);
    sim.enableWorld(LatLon(-33.8568, 151.2153));
    Traveller t(sim, Vec2(-1.0f, 0.0f));
    for (int i = 0; i < 2500; ++i) t.step(0.05f, 30.0f);
    UT_CHECK(t.rebases >= 1);
    const LatLon where = sim.playerGeo();
    const std::string save = sim.save();

    Simulation other(99);
    other.enableWorld(LatLon(10.0, 10.0));                     // starts somewhere else entirely
    std::string err;
    UT_CHECK(other.load(save, &err));
    UT_CHECK(geo::distance(other.frame().anchor(), sim.frame().anchor()) < 0.001);
    UT_CHECK(geo::distance(other.playerGeo(), where) < 0.01);
    UT_CHECK(distance(other.savedPlayerPosition(), t.in.pos) < 0.01f);
    // The loaded game asks for the tiles around the saved position.
    UT_CHECK(!other.world.missing().empty());
    UT_CHECK(other.world.missing().front() == geo::tileAt(where, other.world.zoom));
}

UT_TEST(world, a_malformed_tile_is_treated_as_empty) {
    Simulation sim(2);
    sim.enableWorld(LatLon(1.3521, 103.8198));
    const TileKey key = sim.world.missing().front();
    std::string err;
    UT_CHECK(!sim.provideTile(key, "{\"nodes\": [[1, 2]], \"edges\": [[1, 2, 1, 10, 1, 0]]}", &err));
    UT_CHECK(!err.empty());
    UT_CHECK(sim.world.has(key));                              // not asked for again
    UT_CHECK(sim.provideTile(sim.world.missing().front(), ""));
    PlayerInput in;
    for (int i = 0; i < 40; ++i) sim.tick(0.05f, in);          // and the game runs on with no roads
    UT_CHECK_EQ(sim.traffic.count(), 0);
}

UT_TEST(world, real_map_data_monaco) {
    // The shipped sample tiles were made from OpenStreetMap by Tools/osm_to_tiles.py.
    Simulation sim(13);
    std::string err;
    UT_CHECK(sim.loadTuning(readFile("tuning.json"), &err));
    const LatLon casino(43.7397, 7.4287), fontvieille(43.7290, 7.4159);
    sim.enableWorld(casino);
    PlayerInput in;
    int read = 0, bad = 0;
    auto feed = [&]() {
        const std::vector<TileKey> wanted = sim.world.missing();
        for (const TileKey& k : wanted) {
            const std::string name = "World/tiles/" + std::to_string(k.z) + "/" + std::to_string(k.x) + "/" + std::to_string(k.y) + ".json";
            const std::string text = readFile(name.c_str());
            read += text.empty() ? 0 : 1;
            bad += sim.provideTile(k, text, &err) ? 0 : 1;
        }
    };
    for (int i = 0; i < 200; ++i) {
        feed();
        sim.tick(0.05f, in);
    }
    UT_CHECK(read >= 4);
    UT_CHECK_EQ(bad, 0);
    UT_CHECK(sim.roads.nodeCount() > 2500);
    // Nearly all of it is one connected network.
    std::vector<int> group(static_cast<size_t>(sim.roads.nodeCount()), -1), stack;
    int best = 0;
    for (int seed = 0; seed < sim.roads.nodeCount(); ++seed) {
        if (group[static_cast<size_t>(seed)] >= 0) continue;
        int size = 0;
        stack.push_back(seed);
        group[static_cast<size_t>(seed)] = seed;
        while (!stack.empty()) {
            const int n = stack.back();
            stack.pop_back();
            ++size;
            for (const std::vector<int>* list : {&sim.roads.node(n).out, &sim.roads.node(n).in}) {
                for (const int ei : *list) {
                    const int other = sim.roads.edge(ei).from == n ? sim.roads.edge(ei).to : sim.roads.edge(ei).from;
                    if (group[static_cast<size_t>(other)] < 0) {
                        group[static_cast<size_t>(other)] = seed;
                        stack.push_back(other);
                    }
                }
            }
        }
        best = std::max(best, size);
    }
    UT_CHECK(best > sim.roads.nodeCount() * 9 / 10);
    // A sensible drive from the casino to the Fontvieille harbour: 1.4 km as the crow flies.
    const int from = sim.roads.nearestNode(sim.frame().toLocal(casino), 120.0f);
    const int to = sim.roads.nearestNode(sim.frame().toLocal(fontvieille), 120.0f);
    UT_CHECK(from >= 0 && to >= 0);
    std::vector<int> path;
    UT_CHECK(sim.roads.findPath(from, to, path));
    float length = 0.0f;
    for (const int ei : path) length += sim.roads.edge(ei).length;
    UT_CHECK(length > 1400.0f && length < 6000.0f);
    UT_CHECK(sim.roads.travelTime(path) > 90.0f && sim.roads.travelTime(path) < 900.0f);
    // Traffic fills the streets and the police come when called.
    UT_CHECK(sim.traffic.count() >= 15);
    for (const TrafficAgent& a : sim.traffic.agents()) UT_CHECK(sim.roads.nearestEdge(a.pos, 12.0f) >= 0);
    sim.reportCrime(Crime::Robbery, in.pos, true, 0);
    for (int i = 0; i < 300; ++i) {
        feed();
        sim.tick(0.05f, in);
    }
    UT_CHECK(!sim.dispatch.units().empty());
    UT_CHECK(sim.dispatch.units().front().path.size() >= 2);
}

UT_TEST(world, traffic_keeps_left_where_the_tile_says_so) {
    TileData tile;
    std::string err;
    const char* json = R"({"left":%d,"nodes":[[1,51.5000,-0.1000,0],[2,51.5000,-0.0980,0]],"edges":[[1,2,1,13.4,1,0]]})";
    for (const int left : {0, 1}) {
        char text[256];
        std::snprintf(text, sizeof(text), json, left);
        UT_CHECK(WorldRoads::parseTile(text, tile, &err));
        WorldRoads world;
        world.setFocus(LatLon(51.5, -0.099));
        world.provide(geo::tileAt(LatLon(51.5, -0.099), world.zoom), tile);
        RoadGraph graph;
        world.build(graph, LocalFrame(LatLon(51.5, -0.099)));
        const int eastbound = graph.findEdge(graph.nodeById(1), graph.nodeById(2));
        UT_CHECK(eastbound >= 0);
        // Driving east, the kerb side is south where traffic keeps right and north where it keeps left.
        const float side = graph.lanePosition(eastbound, 50.0f, 0).y;
        UT_CHECK(left ? side > 1.0f : side < -1.0f);
    }
}
