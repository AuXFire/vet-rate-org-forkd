// Headless run of the game simulation on the shipped data.
//
//   ut_sim [dataDir] [minutes] [seed]
//   ut_sim --world <lat> <lon> [minutes] [seed] [tilesDir]
//
// The second form runs anywhere on Earth. Road tiles are read from tilesDir (default
// <dataDir>/World/tiles); with "procedural" as tilesDir, or where no tile file exists and tilesDir is
// "procedural", a made-up street grid is used instead. The player drives wherever the roads lead.
//
// A scripted player drives a loop between locations, commits a witnessed crime every few minutes and then
// keeps driving. Prints one line per simulated minute so pacing (traffic volume, how long heat lasts, how
// many units respond) can be judged without the engine.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Sim/UtSim.h"

using namespace ut;

static std::string readFile(const std::string& path) {
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

// Drives around a streamed world: loads tiles as the simulation asks for them and moves the anchor when told to.
static int runWorld(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: ut_sim --world <lat> <lon> [minutes] [seed] [tilesDir|procedural]\n");
        return 1;
    }
    const LatLon start(std::atof(argv[2]), std::atof(argv[3]));
    const int minutes = argc > 4 ? std::atoi(argv[4]) : 10;
    const uint64_t seed = argc > 5 ? static_cast<uint64_t>(std::atoll(argv[5])) : 1;
    const std::string tiles = argc > 6 ? argv[6] : "Content/Data/World/tiles";
    const bool procedural = tiles == "procedural";

    Simulation sim(seed);
    std::string err;
    const std::string tuning = readFile("Content/Data/tuning.json");
    if (!tuning.empty() && !sim.loadTuning(tuning, &err)) {
        std::fprintf(stderr, "ut_sim: tuning.json: %s\n", err.c_str());
        return 1;
    }
    sim.enableWorld(start);

    PlayerInput in;
    in.inVehicle = true;
    in.vehicleTag = "sedan";
    Rng route(seed * 77U + 5U);
    uint64_t fromId = 0, toId = 0;   // the road being driven, by junction ids: indices do not survive rebuilds
    int rebases = 0, tilesRead = 0, emptyTiles = 0, badTiles = 0, stuck = 0, spawned = 0, crimes = 0;
    double driven = 0.0;
    const float dt = 0.05f;
    const int ticksPerMinute = static_cast<int>(60.0f / dt);

    std::printf("world run from %.5f, %.5f, seed %llu, tiles: %s\n", start.lat, start.lon, static_cast<unsigned long long>(seed), tiles.c_str());
    std::printf("%5s %10s %11s %6s %9s %7s %5s %5s %7s\n", "min", "lat", "lon", "tiles", "junctions", "traffic", "heat", "units", "rebases");
    for (int tick = 0; tick < minutes * ticksPerMinute; ++tick) {
        const std::vector<TileKey> wanted = sim.world.missing();
        for (const TileKey& k : wanted) {
            if (procedural) {
                sim.world.provide(k, WorldRoads::proceduralTile(k));
                ++tilesRead;
                continue;
            }
            const std::string text = readFile(tiles + "/" + std::to_string(k.z) + "/" + std::to_string(k.x) + "/" + std::to_string(k.y) + ".json");
            if (text.empty()) ++emptyTiles;
            else ++tilesRead;
            if (!sim.provideTile(k, text, &err)) ++badTiles;
        }
        if (sim.wantsRebase(in.pos)) {
            in.pos = sim.setAnchor(sim.frame().toGeo(in.pos)).point(in.pos);
            ++rebases;
        }
        // Follow the current road to its end, then take any road onward.
        const int a = sim.roads.nodeById(fromId), b = sim.roads.nodeById(toId);
        int edge = a >= 0 && b >= 0 ? sim.roads.findEdge(a, b) : -1;
        if (edge < 0) {
            edge = sim.roads.nearestEdge(in.pos, 400.0f);
            stuck += edge < 0 && tick > 40 ? 1 : 0;
        }
        if (edge >= 0) {
            const RoadEdge& e = sim.roads.edge(edge);
            fromId = sim.roads.node(e.from).id;
            toId = sim.roads.node(e.to).id;
            const Vec2 to = sim.roads.node(e.to).pos - in.pos;
            const float d = to.length(), travel = e.speed * dt;
            if (d <= travel) {
                in.pos = sim.roads.node(e.to).pos;
                const int next = sim.roads.randomNextEdge(edge, route);
                if (next >= 0) {
                    fromId = toId;
                    toId = sim.roads.node(sim.roads.edge(next).to).id;
                }
            } else {
                in.velocity = to / d * e.speed;
                in.pos += in.velocity * dt;
            }
            driven += static_cast<double>(std::min(d, travel));
        }
        if (tick % (ticksPerMinute * 3) == ticksPerMinute) {
            sim.reportCrime(Crime::Robbery, in.pos, false, 2);
            ++crimes;
        }
        sim.tick(dt, in);
        for (const Event& e : sim.drainEvents()) spawned += e.type == EventType::UnitSpawned ? 1 : 0;
        if ((tick + 1) % ticksPerMinute == 0) {
            const LatLon at = sim.playerGeo();
            std::printf("%5d %10.5f %11.5f %6d %9d %7d %5d %5d %7d\n", (tick + 1) / ticksPerMinute, at.lat, at.lon, sim.world.tileCount(),
                        sim.roads.nodeCount(), sim.traffic.count(), sim.heat.level(), static_cast<int>(sim.dispatch.units().size()), rebases);
        }
    }
    const LatLon end = sim.playerGeo();
    std::printf("drove %.1f km, ended %.1f km from the start; %d tiles read, %d without data, %d malformed; %d rebases, %d graph rebuilds\n",
                driven / 1000.0, geo::distance(start, end) / 1000.0, tilesRead, emptyTiles, badTiles, rebases, sim.roadsVersion());
    std::printf("crimes reported %d, units dispatched %d, ticks with no road in reach %d\n", crimes, spawned, stuck);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--world") return runWorld(argc, argv);
    const std::string dir = argc > 1 ? argv[1] : "Content/Data";
    const int minutes = argc > 2 ? std::atoi(argv[2]) : 10;
    const uint64_t seed = argc > 3 ? static_cast<uint64_t>(std::atoll(argv[3])) : 1;

    Simulation sim(seed);
    std::string err;
    if (!sim.loadCity(readFile(dir + "/city.json"), &err) || !sim.loadTuning(readFile(dir + "/tuning.json"), &err) ||
        !sim.loadMissions(readFile(dir + "/missions.json"), &err) || !sim.loadEconomy(readFile(dir + "/economy.json"), &err)) {
        std::fprintf(stderr, "ut_sim: cannot load data from %s: %s\n", dir.c_str(), err.c_str());
        return 1;
    }

    // Tour: every location in order, by road.
    std::vector<Vec2> stops;
    for (const Poi& p : sim.city.pois()) stops.push_back(p.pos);
    if (stops.size() < 2) {
        std::fprintf(stderr, "ut_sim: the city has no locations to drive between\n");
        return 1;
    }
    size_t stop = 0;
    std::vector<Vec2> route;
    size_t routeIndex = 0;
    PlayerInput in;
    in.pos = stops[0];
    in.inVehicle = true;
    in.vehicleTag = "sedan";
    const float speed = 16.0f, dt = 0.05f;
    int crimes = 0, spawned = 0, cleared = 0, peakUnits = 0;

    std::printf("%s, seed %llu, %d junctions\n", sim.city.name().c_str(), static_cast<unsigned long long>(seed), sim.roads.nodeCount());
    std::printf("%5s %-9s %-17s %7s %9s %5s %-8s %5s\n", "min", "weather", "district", "traffic", "avg km/h", "heat", "state", "units");
    const int ticksPerMinute = static_cast<int>(60.0f / dt);
    for (int tick = 0; tick < minutes * ticksPerMinute; ++tick) {
        if (routeIndex >= route.size()) {   // plan the next leg
            stop = (stop + 1) % stops.size();
            route.clear();
            routeIndex = 0;
            std::vector<int> edges;
            const int from = sim.roads.nearestNode(in.pos, 500.0f), to = sim.roads.nearestNode(stops[stop], 500.0f);
            if (from >= 0 && to >= 0 && sim.roads.findPath(from, to, edges)) {
                route.push_back(sim.roads.node(from).pos);
                for (const int ei : edges) route.push_back(sim.roads.node(sim.roads.edge(ei).to).pos);
            }
            route.push_back(stops[stop]);
        }
        float travel = speed * dt;
        while (travel > 0.0f && routeIndex < route.size()) {
            const Vec2 to = route[routeIndex] - in.pos;
            const float d = to.length();
            if (d <= travel) {
                in.pos = route[routeIndex++];
                travel -= d;
            } else {
                in.velocity = to.normalized() * speed;
                in.pos += in.velocity * dt;
                travel = 0.0f;
            }
        }
        if (tick % (ticksPerMinute * 3) == ticksPerMinute) {   // minute 1, 4, 7, ...
            sim.reportCrime(Crime::Robbery, in.pos, false, 2);
            ++crimes;
        }
        sim.tick(dt, in);
        for (const Event& e : sim.drainEvents()) {
            spawned += e.type == EventType::UnitSpawned ? 1 : 0;
            cleared += e.type == EventType::HeatCleared ? 1 : 0;
        }
        peakUnits = std::max(peakUnits, static_cast<int>(sim.dispatch.units().size()));
        if ((tick + 1) % ticksPerMinute == 0) {
            float sum = 0.0f;
            for (const TrafficAgent& a : sim.traffic.agents()) sum += a.speed;
            const float avg = sim.traffic.count() ? sum / static_cast<float>(sim.traffic.count()) * 3.6f : 0.0f;
            static const char* const kStates[] = {"clean", "pursuit", "search"};
            std::printf("%5d %-9s %-17s %7d %9.1f %5d %-8s %5d\n", (tick + 1) / ticksPerMinute, weatherName(sim.weather.current()),
                        sim.currentDistrict() ? sim.currentDistrict()->name.c_str() : "(water)", sim.traffic.count(), static_cast<double>(avg),
                        sim.heat.level(), kStates[static_cast<int>(sim.heat.state())], static_cast<int>(sim.dispatch.units().size()));
        }
    }
    std::printf("crimes reported %d, units dispatched %d (peak %d at once), heat cleared %d times, game clock day %d %02d:%02d\n",
                crimes, spawned, peakUnits, cleared, sim.clock.day, static_cast<int>(sim.clock.hour()),
                static_cast<int>(sim.clock.secondsOfDay / 60.0f) % 60);
    return 0;
}
