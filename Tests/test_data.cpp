// The shipped data files, loaded by the same code the game uses.
#include <cstdio>
#include <string>

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

bool loadAll(Simulation& sim, std::string& err) {
    return sim.loadCity(readFile("city.json"), &err) && sim.loadTuning(readFile("tuning.json"), &err) &&
           sim.loadMissions(readFile("missions.json"), &err) && sim.loadEconomy(readFile("economy.json"), &err);
}

}  // namespace

UT_TEST(data, shipped_files_load) {
    Simulation sim(1);
    std::string err;
    UT_CHECK(loadAll(sim, err));
    UT_CHECK_EQ(err, std::string());
    UT_CHECK_EQ(sim.city.name(), std::string("Port Meridian"));
    UT_CHECK_EQ(static_cast<int>(sim.city.districts().size()), 9);
    UT_CHECK(sim.roads.nodeCount() > 1500);
    UT_CHECK(static_cast<int>(sim.missions.missions().size()) >= 6);
    UT_CHECK_EQ(static_cast<int>(sim.missions.available().size()), 1);
    UT_CHECK_EQ(sim.economy.cash(), 500);
    UT_CHECK_NEAR(sim.clock.timeScale, 30.0f, 1e-3);
    Json extra;
    UT_CHECK(Json::parse(readFile("vehicles.json"), extra, &err));
    UT_CHECK(extra["vehicles"].size() >= sim.traffic.tuning.archetypes);
    UT_CHECK(Json::parse(readFile("weapons.json"), extra, &err));
    UT_CHECK(extra["weapons"].size() >= 4);
}

UT_TEST(data, every_location_is_reachable_by_road_from_the_first_safehouse) {
    Simulation sim(1);
    std::string err;
    UT_CHECK(loadAll(sim, err));
    const Poi* home = sim.city.findPoi("safehouse_narrows");
    UT_CHECK(home != nullptr);
    if (!home) return;
    const int from = sim.roads.nearestNode(home->pos, 60.0f);
    UT_CHECK(from >= 0);
    std::vector<int> path;
    for (const Poi& p : sim.city.pois()) {
        const int to = sim.roads.nearestNode(p.pos, 60.0f);
        UT_CHECK(to >= 0);
        UT_CHECK(sim.roads.findPath(from, to, path));
        UT_CHECK(sim.city.districtAt(p.pos) != nullptr);
    }
    // Crossing the river uses a bridge.
    const Poi* villa = sim.city.findPoi("hill_villa");
    UT_CHECK(villa != nullptr);
    if (!villa) return;
    UT_CHECK(sim.roads.findPath(from, sim.roads.nearestNode(villa->pos, 60.0f), path));
    bool usedBridge = false;
    for (const int ei : path) usedBridge = usedBridge || sim.roads.edge(ei).kind == RoadKind::Bridge;
    UT_CHECK(usedBridge);
    UT_CHECK(sim.roads.travelTime(path) > 120.0f);   // across town is minutes, not seconds
}

UT_TEST(data, a_minute_downtown_has_moving_traffic_and_a_working_chase) {
    Simulation sim(3);
    std::string err;
    UT_CHECK(loadAll(sim, err));
    const Poi* flat = sim.city.findPoi("safehouse_heights");
    UT_CHECK(flat != nullptr);
    if (!flat) return;
    PlayerInput in;
    in.pos = flat->pos;
    sim.clock.secondsOfDay = 13.0f * 3600.0f;
    for (int i = 0; i < 400; ++i) sim.tick(0.05f, in);
    UT_CHECK(sim.currentDistrict() != nullptr);
    UT_CHECK_EQ(sim.currentDistrict()->id, std::string("heights"));
    UT_CHECK(sim.traffic.count() >= 60);
    float speed = 0.0f;
    for (const TrafficAgent& a : sim.traffic.agents()) speed += a.speed;
    UT_CHECK(speed / static_cast<float>(sim.traffic.count()) > 2.5f);
    UT_CHECK(sim.pedestrianBudget() >= 40);

    sim.reportCrime(Crime::Robbery, in.pos, true, 0);
    bool caught = false;
    for (int i = 0; i < 1200 && !caught; ++i) {
        sim.tick(0.05f, in);
        caught = sim.dispatch.anyUnitNear(in.pos);
    }
    UT_CHECK(caught);                                  // a unit reached a player who stood still
    sim.tick(0.05f, in);                               // sight is evaluated at the start of the next tick
    UT_CHECK(sim.heat.state() == HeatState::Pursuit);
}
