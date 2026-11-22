#include <cmath>

#include "Core/UtCrowd.h"
#include "Core/UtDispatch.h"
#include "Core/UtDriver.h"
#include "Core/UtEconomy.h"
#include "Core/UtMission.h"
#include "Core/UtSim.h"
#include "Core/UtTraffic.h"
#include "ut_test.h"

using namespace ut;

namespace {

// n x n grid of two-way streets, `spacing` meters apart. Node index = row * n + col.
RoadGraph makeGrid(int n, float spacing, bool signals = false) {
    RoadGraph g;
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) g.addNode(Vec2(static_cast<float>(c) * spacing, static_cast<float>(r) * spacing), signals);
    }
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            if (c + 1 < n) g.addRoad(r * n + c, r * n + c + 1, 1, 13.0f);
            if (r + 1 < n) g.addRoad(r * n + c, (r + 1) * n + c, 1, 13.0f);
        }
    }
    g.buildIndex();
    return g;
}

// One long straight road, east from the origin.
RoadGraph makeStraight(float length, bool signalAtEnd) {
    RoadGraph g;
    g.addNode(Vec2(0, 0));
    g.addNode(Vec2(length, 0), signalAtEnd);
    g.addNode(Vec2(length + 200.0f, 0));
    g.addRoad(0, 1, 1, 14.0f, RoadKind::Street, true);
    g.addRoad(1, 2, 1, 14.0f, RoadKind::Street, true);
    g.buildIndex();
    return g;
}

float minGapOnSameLane(const TrafficSystem& t) {
    float best = 1e9f;
    const auto& agents = t.agents();
    for (size_t i = 0; i < agents.size(); ++i) {
        for (size_t k = 0; k < agents.size(); ++k) {
            if (i == k || agents[i].edge != agents[k].edge || agents[i].lane != agents[k].lane) continue;
            if (agents[k].s > agents[i].s) best = std::min(best, agents[k].s - agents[k].length - agents[i].s);
        }
    }
    return best;
}

}  // namespace

// ---------------------------------------------------------------- traffic
UT_TEST(traffic, fills_the_ring_and_respects_the_budget) {
    const RoadGraph g = makeGrid(9, 90.0f);
    TrafficSystem t;
    t.setGraph(&g);
    t.tuning.maxAgents = 40;
    Rng rng(5);
    EventQueue ev;
    const Vec2 focus(360.0f, 360.0f);
    t.update(0.1f, focus, 1.0f, rng, ev);
    UT_CHECK(t.count() > 25);
    UT_CHECK(t.count() <= 40);
    UT_CHECK_EQ(ev.count(EventType::TrafficSpawned), t.count());
    for (int i = 0; i < 600; ++i) t.update(0.1f, focus, 1.0f, rng, ev);
    UT_CHECK(t.count() <= 40);
    UT_CHECK(t.count() >= 30);
    for (const TrafficAgent& a : t.agents()) UT_CHECK(distance(a.pos, focus) <= t.tuning.despawn + 1.0f);
    // Half density, half the cars (after the surplus drives out of range or is simply not replaced).
    TrafficSystem quiet;
    quiet.setGraph(&g);
    quiet.tuning.maxAgents = 40;
    quiet.update(0.1f, focus, 0.5f, rng, ev);
    UT_CHECK(quiet.count() <= 20);
}

UT_TEST(traffic, cars_never_overlap_and_keep_moving) {
    const RoadGraph g = makeGrid(7, 80.0f);
    TrafficSystem t;
    t.setGraph(&g);
    t.tuning.maxAgents = 70;   // dense enough to force following
    Rng rng(9);
    EventQueue ev;
    const Vec2 focus(240.0f, 240.0f);
    float worstGap = 1e9f;
    float speedSum = 0.0f;
    int samples = 0;
    for (int i = 0; i < 1200; ++i) {
        t.update(0.05f, focus, 1.0f, rng, ev);
        if (i > 200 && i % 10 == 0) {
            worstGap = std::min(worstGap, minGapOnSameLane(t));
            for (const TrafficAgent& a : t.agents()) {
                speedSum += a.speed;
                ++samples;
            }
        }
    }
    UT_CHECK(worstGap > 0.0f);                              // no car inside another
    UT_CHECK(speedSum / static_cast<float>(samples) > 3.0f);   // and no gridlock
}

UT_TEST(traffic, stops_at_red_and_goes_on_green) {
    const RoadGraph g = makeStraight(200.0f, true);
    TrafficSystem t;
    t.setGraph(&g);
    t.tuning.maxAgents = 0;   // no ambient spawning; drive time forward to find a red phase
    Rng rng(1);
    EventQueue ev;
    while (t.signalGreen(0)) t.update(0.5f, Vec2(100, 0), 1.0f, rng, ev);
    UT_CHECK(!t.signalGreen(0));
    // Spawn exactly one car by raising the budget for one update, then pin it near the start.
    t.tuning.maxAgents = 1;
    t.tuning.spawnMin = 0.0f;
    while (t.count() == 0) t.update(0.01f, Vec2(100, 0), 1.0f, rng, ev);
    float maxS = 0.0f;
    bool stopped = false;
    for (int i = 0; i < 400 && !t.signalGreen(0); ++i) {
        t.update(0.05f, Vec2(100, 0), 1.0f, rng, ev);
        if (t.count() == 1 && t.agents()[0].edge == 0) {
            maxS = std::max(maxS, t.agents()[0].s);
            stopped = stopped || (t.agents()[0].speed < 0.3f && t.agents()[0].s > 150.0f);
        }
    }
    if (t.count() == 1 && t.agents()[0].edge == 0) {
        UT_CHECK(maxS <= 200.0f - 2.0f);   // never crossed the stop line on red
        UT_CHECK(stopped || t.signalGreen(0));
    }
    // After the light turns green the car gets through.
    bool crossed = false;
    for (int i = 0; i < 2000 && !crossed; ++i) {
        t.update(0.05f, Vec2(100, 0), 1.0f, rng, ev);
        crossed = t.count() == 0 || t.agents()[0].edge != 0;
    }
    UT_CHECK(crossed);
}

UT_TEST(traffic, brakes_for_an_obstacle_and_panics_on_gunfire) {
    const RoadGraph g = makeStraight(400.0f, false);
    TrafficSystem t;
    t.setGraph(&g);
    t.tuning.maxAgents = 1;
    t.tuning.spawnMin = 0.0f;
    t.tuning.spawnMax = 500.0f;
    t.tuning.despawn = 2000.0f;
    Rng rng(4);
    EventQueue ev;
    while (t.count() == 0) t.update(0.01f, Vec2(200, 0), 1.0f, rng, ev);
    t.tuning.maxAgents = 0;
    const TrafficAgent start = t.agents()[0];
    if (start.edge == 0 && start.s < 300.0f) {
        const float wallS = start.s + 60.0f;
        const Vec2 wall = g.lanePosition(0, wallS, 0);
        for (int i = 0; i < 600; ++i) {
            t.addObstacle(wall, Vec2(), 1.5f);
            t.update(0.05f, Vec2(200, 0), 1.0f, rng, ev);
        }
        UT_CHECK_EQ(t.count(), 1);
        UT_CHECK(t.agents()[0].speed < 0.5f);
        UT_CHECK(t.agents()[0].s < wallS);
        UT_CHECK(t.agents()[0].s > wallS - 15.0f);   // stopped close behind it, not 50 m back
        // Gunfire next to the car: it floors it (the wall is gone now) and breaks the speed limit.
        const float calmTop = 14.0f * t.agents()[0].desiredFactor;
        t.panicAt(t.agents()[0].pos, 30.0f, 10.0f);
        for (int i = 0; i < 160; ++i) t.update(0.05f, Vec2(200, 0), 1.0f, rng, ev);
        UT_CHECK_EQ(t.count(), 1);
        UT_CHECK(t.agents()[0].speed > calmTop * 1.2f);
    }
}

// ---------------------------------------------------------------- driver
UT_TEST(driver, follows_an_l_shaped_path_and_stops_at_the_end) {
    Driver d;
    d.setPath({Vec2(0, 0), Vec2(150, 0), Vec2(150, 150)});
    BicycleModel car;
    car.state.pos = Vec2(0, 0);
    car.state.heading = 0.0f;
    float worstOffPath = 0.0f;
    float topSpeed = 0.0f;
    int steps = 0;
    while (!d.arrived() && steps < 4000) {
        car.step(d.update(car.state, 20.0f), 0.02f);
        const Vec2 a = closestOnSegment(Vec2(0, 0), Vec2(150, 0), car.state.pos);
        const Vec2 b = closestOnSegment(Vec2(150, 0), Vec2(150, 150), car.state.pos);
        worstOffPath = std::max(worstOffPath, std::min(distance(a, car.state.pos), distance(b, car.state.pos)));
        topSpeed = std::max(topSpeed, car.state.speed);
        ++steps;
    }
    UT_CHECK(d.arrived());
    UT_CHECK(distance(car.state.pos, Vec2(150, 150)) <= d.tuning.arriveRadius + 0.5f);
    UT_CHECK(worstOffPath < 6.0f);        // cut the corner by less than two lane widths
    UT_CHECK(topSpeed > 15.0f);           // used the straight
    UT_CHECK(topSpeed <= 21.5f);          // without overshooting cruise
    for (int i = 0; i < 200; ++i) car.step(d.update(car.state, 20.0f), 0.02f);
    UT_CHECK(car.state.speed < 0.5f);     // holds the brake once there
}

UT_TEST(driver, turns_around_when_facing_the_wrong_way) {
    Driver d;
    d.setPath({Vec2(0, 0), Vec2(200, 0)});
    BicycleModel car;
    car.state.pos = Vec2(10, 3);
    car.state.heading = kPi;   // pointing west, path goes east
    int steps = 0;
    while (!d.arrived() && steps < 6000) {
        car.step(d.update(car.state, 15.0f), 0.02f);
        ++steps;
    }
    UT_CHECK(d.arrived());
}

UT_TEST(driver, no_path_means_brake) {
    Driver d;
    VehicleState s;
    s.speed = 10.0f;
    const DriveControls c = d.update(s, 20.0f);
    UT_CHECK_NEAR(c.brake, 1.0f, 1e-6);
    UT_CHECK_NEAR(c.throttle, 0.0f, 1e-6);
}

// ---------------------------------------------------------------- dispatch
UT_TEST(dispatch, sends_units_by_heat_level_and_they_converge_on_the_player) {
    const RoadGraph g = makeGrid(11, 80.0f);
    DispatchSystem d;
    d.setGraph(&g);
    HeatSystem heat;
    Rng rng(2);
    EventQueue ev;
    const Vec2 player(400.0f, 400.0f);
    d.update(1.0f, heat, player, rng, ev);
    UT_CHECK_EQ(static_cast<int>(d.units().size()), 0);

    heat.reportCrime(Crime::AssaultOnOfficer, player, true, 0, ev);   // level 2+
    const int level = heat.level();
    UT_CHECK(level >= 2);
    bool reached = false;
    for (int i = 0; i < 1200 && !reached; ++i) {
        heat.update(0.1f, player, true, ev);
        d.update(0.1f, heat, player, rng, ev);
        reached = d.anyUnitNear(player);
    }
    UT_CHECK(reached);
    UT_CHECK_EQ(static_cast<int>(d.units().size()), d.tuning.unitsPerLevel[level]);
    for (const PoliceUnit& u : d.units()) UT_CHECK(u.role == UnitRole::Pursue);
    UT_CHECK_EQ(ev.count(EventType::UnitSpawned), d.tuning.unitsPerLevel[level]);
}

UT_TEST(dispatch, units_spawn_out_of_sight_and_stand_down_when_heat_clears) {
    const RoadGraph g = makeGrid(11, 80.0f);
    DispatchSystem d;
    d.setGraph(&g);
    HeatSystem heat;
    Rng rng(7);
    EventQueue ev;
    const Vec2 player(400.0f, 400.0f);
    heat.reportCrime(Crime::Gunfire, player, true, 0, ev);
    heat.update(0.1f, player, true, ev);
    d.update(0.1f, heat, player, rng, ev);
    UT_CHECK_EQ(static_cast<int>(d.units().size()), 1);
    UT_CHECK(distance(d.units()[0].pos, player) >= d.tuning.spawnMin * 0.8f);

    heat.clear(ev);
    d.update(0.1f, heat, player, rng, ev);
    UT_CHECK(d.units()[0].role == UnitRole::StandDown);
    for (int i = 0; i < 200; ++i) d.update(0.1f, heat, player, rng, ev);
    UT_CHECK_EQ(static_cast<int>(d.units().size()), 0);
    UT_CHECK_EQ(ev.count(EventType::UnitRemoved), 1);
}

UT_TEST(dispatch, search_sweeps_inside_the_zone_and_externally_driven_units_are_not_moved) {
    const RoadGraph g = makeGrid(11, 80.0f);
    DispatchSystem d;
    d.setGraph(&g);
    HeatSystem heat;
    Rng rng(3);
    EventQueue ev;
    const Vec2 scene(400.0f, 400.0f);
    heat.reportCrime(Crime::Gunfire, scene, true, 0, ev);
    heat.update(0.1f, scene, true, ev);
    d.update(0.1f, heat, scene, rng, ev);
    const Vec2 far(5000.0f, 5000.0f);   // the player slips away
    heat.update(0.1f, far, false, ev);
    d.update(0.1f, heat, far, rng, ev);
    UT_CHECK(heat.state() == HeatState::Search);
    UT_CHECK_EQ(static_cast<int>(d.units().size()), 1);
    UT_CHECK(d.units()[0].role == UnitRole::Search);
    UT_CHECK(distance(d.units()[0].goal, heat.lastKnownPosition()) <= heat.searchRadius() + 1.0f);
    UT_CHECK(d.units()[0].path.size() >= 2);

    const int id = d.units()[0].id;
    const int version = d.units()[0].pathVersion;
    d.reportUnit(id, Vec2(123.0f, 45.0f), 1.0f, 9.0f);
    d.update(0.1f, heat, far, rng, ev);
    UT_CHECK_NEAR(d.units()[0].pos.x, 123.0f, 1e-3);   // the game owns its position now
    UT_CHECK(d.units()[0].pathVersion >= version);
    d.removeUnit(id, ev);
    UT_CHECK_EQ(static_cast<int>(d.units().size()), 0);
}

// ---------------------------------------------------------------- mission
namespace {
const char* kMissions = R"({"missions": [
  {"id": "m1", "title": "First", "start": [0, 0], "reward": {"cash": 500},
   "objectives": [
     {"type": "goto", "pos": [100, 0], "radius": 5, "text": "Go there"},
     {"type": "enter_vehicle", "tag": "van", "text": "Take the van"},
     {"type": "deliver", "tag": "van", "pos": [300, 0], "radius": 10, "timeLimit": 60, "text": "Deliver it"},
     {"type": "lose_heat", "text": "Lose the police"}],
   "fail": [{"type": "vehicle_destroyed", "tag": "van"}, {"type": "player_down"}]},
  {"id": "m2", "title": "Second", "requires": ["m1"], "reward": {"cash": 900},
   "objectives": [{"type": "collect", "tag": "crate", "count": 3, "text": "Collect crates", "maxHeat": 1},
                  {"type": "wait", "seconds": 5, "text": "Wait"}]}
]})";

MissionSystem loadMissions() {
    MissionSystem m;
    Json j;
    std::string err;
    UT_CHECK(Json::parse(kMissions, j, &err));
    UT_CHECK(m.load(j, &err));
    return m;
}
}  // namespace

UT_TEST(mission, objectives_complete_in_order_and_pay_out) {
    MissionSystem m = loadMissions();
    EventQueue ev;
    UT_CHECK_EQ(static_cast<int>(m.available().size()), 1);   // m2 is locked behind m1
    UT_CHECK(!m.start("m2", ev));
    UT_CHECK(m.start("m1", ev));
    UT_CHECK(!m.start("m1", ev));                             // already running

    MissionContext ctx;
    ctx.playerPos = Vec2(50, 0);
    m.update(1.0f, ctx, ev);
    UT_CHECK_EQ(m.objectiveIndex(), 0);
    ctx.playerPos = Vec2(98, 0);
    m.update(1.0f, ctx, ev);
    UT_CHECK_EQ(m.objectiveIndex(), 1);
    ctx.inVehicle = true;
    ctx.vehicleTag = "sedan";                                 // wrong vehicle
    m.update(1.0f, ctx, ev);
    UT_CHECK_EQ(m.objectiveIndex(), 1);
    ctx.vehicleTag = "van";
    m.update(1.0f, ctx, ev);
    UT_CHECK_EQ(m.objectiveIndex(), 2);
    UT_CHECK_NEAR(m.timeRemaining(), 60.0f, 1e-3);
    ctx.playerPos = Vec2(295, 0);
    ctx.heatLevel = 2;
    m.update(1.0f, ctx, ev);
    UT_CHECK_EQ(m.objectiveIndex(), 3);                       // delivered; now must lose the police
    m.update(1.0f, ctx, ev);
    UT_CHECK(m.active());
    ctx.heatLevel = 0;
    m.update(1.0f, ctx, ev);
    UT_CHECK(!m.active());
    UT_CHECK(m.isComplete("m1"));
    UT_CHECK_EQ(m.takePendingReward(), 500);
    UT_CHECK_EQ(m.takePendingReward(), 0);
    UT_CHECK_EQ(ev.count(EventType::MissionCompleted), 1);
    UT_CHECK_EQ(ev.count(EventType::ObjectiveChanged), 4);
    UT_CHECK_EQ(static_cast<int>(m.available().size()), 1);   // m2 unlocked, m1 gone
    UT_CHECK_EQ(m.available()[0]->id, std::string("m2"));
}

UT_TEST(mission, fails_on_timeout_destroyed_vehicle_player_down_and_heat_cap) {
    {
        MissionSystem m = loadMissions();
        EventQueue ev;
        m.start("m1", ev);
        MissionContext ctx;
        ctx.playerPos = Vec2(100, 0);
        m.update(0.1f, ctx, ev);
        ctx.inVehicle = true;
        ctx.vehicleTag = "van";
        m.update(0.1f, ctx, ev);
        m.update(61.0f, ctx, ev);                             // deliver has a 60 s limit
        UT_CHECK(!m.active());
        UT_CHECK(!m.isComplete("m1"));
        UT_CHECK_EQ(ev.count(EventType::MissionFailed), 1);
        UT_CHECK(m.start("m1", ev));                          // can retry
        m.notify(MissionNotice::VehicleDestroyed, "sedan", ev);
        UT_CHECK(m.active());
        m.notify(MissionNotice::VehicleDestroyed, "van", ev);
        UT_CHECK(!m.active());
        UT_CHECK(m.start("m1", ev));
        m.notify(MissionNotice::PlayerDown, "", ev);
        UT_CHECK(!m.active());
    }
    {
        MissionSystem m = loadMissions();
        EventQueue ev;
        Json state;
        UT_CHECK(Json::parse(R"({"completed": ["m1", "gone_mission"]})", state));
        m.load_state(state);                                  // unknown ids from old saves are dropped
        UT_CHECK(m.isComplete("m1"));
        UT_CHECK(m.start("m2", ev));
        m.notify(MissionNotice::Collected, "crate", ev);
        m.notify(MissionNotice::Collected, "barrel", ev);     // wrong tag: not counted
        m.notify(MissionNotice::Collected, "crate", ev);
        UT_CHECK_EQ(m.counter(), 2);
        MissionContext ctx;
        ctx.heatLevel = 2;                                    // objective caps heat at 1
        m.update(0.1f, ctx, ev);
        UT_CHECK(!m.active());
    }
}

UT_TEST(mission, rejects_bad_data) {
    MissionSystem m;
    Json j;
    std::string err;
    UT_CHECK(Json::parse(R"({"missions": [{"id": "x", "objectives": [{"type": "teleport"}]}]})", j));
    UT_CHECK(!m.load(j, &err));
    UT_CHECK(err.find("teleport") != std::string::npos);
    UT_CHECK(Json::parse(R"({"missions": [{"id": "x", "requires": ["nope"], "objectives": [{"type": "wait", "seconds": 1}]}]})", j));
    UT_CHECK(!m.load(j, &err));
    UT_CHECK(err.find("nope") != std::string::npos);
    UT_CHECK(m.missions().empty());
}

// ---------------------------------------------------------------- economy
UT_TEST(economy, spending_buying_and_daily_income) {
    Economy e;
    Json j;
    UT_CHECK(Json::parse(R"({"startingCash": 1000,
        "items": [{"id": "pistol", "name": "Pistol", "category": "weapon", "price": 600}],
        "properties": [{"id": "garage", "name": "Garage", "pos": [1, 2], "price": 5000, "incomePerDay": 400}]})", j));
    e.load(j);
    EventQueue ev;
    UT_CHECK_EQ(e.cash(), 1000);
    UT_CHECK(!e.spend(1001, ev));
    UT_CHECK_EQ(e.cash(), 1000);
    UT_CHECK(e.buyItem("pistol", ev));
    UT_CHECK_EQ(e.cash(), 400);
    UT_CHECK(!e.buyItem("pistol", ev));        // cannot afford a second
    UT_CHECK(!e.buyItem("tank", ev));          // not in the catalogue
    UT_CHECK(!e.buyProperty("garage", ev));
    e.payDailyIncome(ev);                      // owns nothing yet
    UT_CHECK_EQ(e.cash(), 400);
    e.earn(5000, ev);
    UT_CHECK(e.buyProperty("garage", ev));
    UT_CHECK(!e.buyProperty("garage", ev));    // already owned
    UT_CHECK_EQ(e.cash(), 400);
    e.payDailyIncome(ev);
    UT_CHECK_EQ(e.cash(), 800);
    UT_CHECK_EQ(ev.count(EventType::PropertyIncome), 1);

    Economy restored;
    restored.load(j);
    restored.load_state(e.save());
    UT_CHECK_EQ(restored.cash(), 800);
    UT_CHECK(restored.properties()[0].owned);
    e.earn(2000000000, ev);
    e.earn(2000000000, ev);
    UT_CHECK(e.cash() > 0);                    // capped, not overflowed
}

// ---------------------------------------------------------------- crowd
UT_TEST(crowd, reactions_depend_on_distance_and_personality) {
    Crowd c;
    Rng rng(8);
    PedTraits timid;
    timid.boldness = 0.05f;
    timid.civic = 0.0f;
    PedTraits bold;
    bold.boldness = 1.0f;
    bold.civic = 0.0f;
    UT_CHECK(c.react(timid, Stimulus::Gunfire, 500.0f, true, rng) == Reaction::Ignore);   // out of earshot
    int timidFlees = 0, boldFlees = 0;
    for (int i = 0; i < 200; ++i) {
        const Reaction a = c.react(timid, Stimulus::Gunfire, 20.0f, true, rng);
        timidFlees += (a == Reaction::Flee || a == Reaction::Cower) ? 1 : 0;
        boldFlees += c.react(bold, Stimulus::Gunfire, 60.0f, true, rng) == Reaction::Flee ? 1 : 0;
    }
    UT_CHECK_EQ(timidFlees, 200);
    UT_CHECK_EQ(boldFlees, 0);
    UT_CHECK(c.react(bold, Stimulus::VehicleThreat, 5.0f, true, rng) == Reaction::Flee);

    PedTraits citizen;
    citizen.boldness = 0.9f;
    citizen.civic = 1.0f;
    int seenReports = 0, unseenFightReports = 0;
    for (int i = 0; i < 400; ++i) {
        seenReports += c.react(citizen, Stimulus::Fight, 10.0f, true, rng) == Reaction::Report ? 1 : 0;
        unseenFightReports += c.react(citizen, Stimulus::Fight, 10.0f, false, rng) == Reaction::Report ? 1 : 0;
    }
    UT_CHECK(seenReports > 100);
    UT_CHECK_EQ(unseenFightReports, 0);        // nobody reports a fight they could not see
}

UT_TEST(crowd, budget_scales_with_place_time_and_weather) {
    Crowd c;
    const int noon = c.pedestrianBudget(1.0f, 1.0f, 1.0f);
    const int night = c.pedestrianBudget(1.0f, 0.0f, 1.0f);
    const int storm = c.pedestrianBudget(1.0f, 1.0f, 0.25f);
    UT_CHECK_EQ(noon, c.tuning.maxPedestrians);
    UT_CHECK_EQ(night, c.tuning.maxPedestrians / 4);
    UT_CHECK_EQ(storm, c.tuning.maxPedestrians / 4);
    UT_CHECK_EQ(c.pedestrianBudget(0.0f, 1.0f, 1.0f), 0);
}

// ---------------------------------------------------------------- sim
namespace {

std::string gridCityJson() {
    Json roads = Json::object();
    Json nodes = Json::array(), edges = Json::array();
    const int n = 11;
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            Json node = Json::array();
            node.push(c * 80).push(r * 80).push((r % 3 == 1 && c % 3 == 1) ? 1 : 0);
            nodes.push(node);
        }
    }
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            for (int dir = 0; dir < 2; ++dir) {
                const int rr = r + dir, cc = c + 1 - dir;
                if (rr >= n || cc >= n) continue;
                Json e = Json::array();
                e.push(r * n + c).push(rr * n + cc).push(1).push(13).push(1).push(0);
                edges.push(e);
            }
        }
    }
    roads.set("nodes", nodes);
    roads.set("edges", edges);
    Json city = Json::object();
    Json meta = Json::object();
    meta.set("name", "Testville");
    city.set("meta", meta);
    city.set("roads", roads);
    Json district = Json::object();
    district.set("id", "centre");
    district.set("name", "Centre");
    district.set("density", 0.9);
    Json poly = Json::array();
    for (const auto& p : {std::pair<int, int>{-10, -10}, {810, -10}, {810, 810}, {-10, 810}}) {
        Json pt = Json::array();
        pt.push(p.first).push(p.second);
        poly.push(pt);
    }
    district.set("poly", poly);
    Json districts = Json::array();
    districts.push(district);
    city.set("districts", districts);
    return city.dump();
}

Simulation makeSim(uint64_t seed) {
    Simulation sim(seed);
    std::string err;
    UT_CHECK(sim.loadCity(gridCityJson(), &err));
    UT_CHECK(sim.loadMissions(kMissions, &err));
    UT_CHECK(sim.loadEconomy(R"({"startingCash": 250, "properties": [{"id": "dock", "price": 100, "incomePerDay": 75}]})", &err));
    UT_CHECK(sim.loadTuning(R"({"clock": {"timeScale": 60}, "traffic": {"maxAgents": 50}})", &err));
    return sim;
}

// A scripted two minutes: drive around, commit a witnessed crime, then hide.
uint64_t runScript(Simulation& sim, int* outPeakHeat = nullptr, int* outPeakUnits = nullptr) {
    PlayerInput in;
    in.inVehicle = true;
    in.vehicleTag = "sedan";
    uint64_t hash = 1469598103934665603ULL;
    int peakHeat = 0, peakUnits = 0;
    for (int i = 0; i < 2400; ++i) {
        const float t = static_cast<float>(i) * 0.05f;
        in.pos = i < 1200 ? Vec2(400.0f + 60.0f * std::cos(t * 0.2f), 400.0f + 60.0f * std::sin(t * 0.2f)) : Vec2(40.0f, 40.0f);
        if (i == 200) sim.reportCrime(Crime::Gunfire, in.pos, false, 3);
        if (i == 400) sim.reportCrime(Crime::AssaultOnOfficer, in.pos, true, 0);
        sim.tick(0.05f, in);
        peakHeat = std::max(peakHeat, sim.heat.level());
        peakUnits = std::max(peakUnits, static_cast<int>(sim.dispatch.units().size()));
        for (const Event& e : sim.drainEvents()) {
            hash = (hash ^ static_cast<uint64_t>(e.type)) * 1099511628211ULL;
            hash = (hash ^ static_cast<uint64_t>(static_cast<uint32_t>(e.a))) * 1099511628211ULL;
        }
    }
    for (const TrafficAgent& a : sim.traffic.agents()) {
        hash = (hash ^ static_cast<uint64_t>(static_cast<uint32_t>(a.id))) * 1099511628211ULL;
        hash = (hash ^ static_cast<uint64_t>(static_cast<int64_t>(a.s * 100.0f))) * 1099511628211ULL;
    }
    if (outPeakHeat) *outPeakHeat = peakHeat;
    if (outPeakUnits) *outPeakUnits = peakUnits;
    return hash;
}

}  // namespace

UT_TEST(sim, scripted_session_raises_heat_brings_police_and_clears) {
    Simulation sim = makeSim(42);
    int peakHeat = 0, peakUnits = 0;
    runScript(sim, &peakHeat, &peakUnits);
    UT_CHECK(peakHeat >= 2);
    UT_CHECK(peakUnits >= 2);
    UT_CHECK_EQ(sim.heat.level(), 0);                 // a minute hiding across town was enough
    UT_CHECK(sim.traffic.count() > 10);
    UT_CHECK(sim.currentDistrict() != nullptr);
    UT_CHECK(sim.pedestrianBudget() > 0);
}

UT_TEST(sim, same_seed_replays_identically_and_different_seeds_differ) {
    Simulation a = makeSim(42), b = makeSim(42), c = makeSim(43);
    const uint64_t ha = runScript(a), hb = runScript(b), hc = runScript(c);
    UT_CHECK(ha == hb);
    UT_CHECK(ha != hc);
}

UT_TEST(sim, mission_reward_and_property_income_reach_the_wallet) {
    Simulation sim = makeSim(1);
    UT_CHECK(sim.buyProperty("dock"));
    UT_CHECK_EQ(sim.economy.cash(), 150);
    UT_CHECK(sim.startMission("m1"));
    PlayerInput in;
    in.pos = Vec2(100, 0);
    sim.tick(0.1f, in);
    in.inVehicle = true;
    in.vehicleTag = "van";
    sim.tick(0.1f, in);
    in.pos = Vec2(300, 0);
    sim.tick(0.1f, in);
    sim.tick(0.1f, in);
    UT_CHECK(sim.missions.isComplete("m1"));
    UT_CHECK_EQ(sim.economy.cash(), 650);
    // Run past midnight: one day of income. timeScale 60 -> a game day is 24 real minutes.
    const int dayBefore = sim.clock.day;
    for (int i = 0; i < 24 * 60 * 5 && sim.clock.day == dayBefore; ++i) sim.tick(0.2f, in);
    UT_CHECK_EQ(sim.clock.day, dayBefore + 1);
    UT_CHECK_EQ(sim.economy.cash(), 725);
}

UT_TEST(sim, save_and_load_restore_progress) {
    Simulation sim = makeSim(5);
    sim.buyProperty("dock");
    sim.startMission("m1");
    PlayerInput in;
    in.pos = Vec2(100, 0);
    sim.tick(0.1f, in);
    in.inVehicle = true;
    in.vehicleTag = "van";
    sim.tick(0.1f, in);
    in.pos = Vec2(300, 0);
    sim.tick(0.1f, in);
    sim.tick(0.1f, in);
    sim.reportCrime(Crime::Robbery, in.pos, true, 0);
    sim.tick(0.1f, in);
    const std::string text = sim.save();

    Simulation fresh = makeSim(99);
    std::string err;
    UT_CHECK(fresh.load(text, &err));
    UT_CHECK_EQ(fresh.economy.cash(), sim.economy.cash());
    UT_CHECK(fresh.missions.isComplete("m1"));
    UT_CHECK(fresh.economy.properties()[0].owned);
    UT_CHECK_EQ(fresh.heat.level(), sim.heat.level());
    UT_CHECK_EQ(fresh.clock.day, sim.clock.day);
    UT_CHECK_NEAR(fresh.savedPlayerPosition().x, 300.0f, 1e-3);
    UT_CHECK(fresh.rng.state() == sim.rng.state());
    UT_CHECK(fresh.pendingEvents().events().empty());

    UT_CHECK(!fresh.load("{\"version\": 99}", &err));
    UT_CHECK(!fresh.load("not json", &err));
    UT_CHECK_EQ(fresh.economy.cash(), sim.economy.cash());   // a rejected save changes nothing
}

UT_TEST(sim, bad_data_is_reported_not_crashed_on) {
    Simulation sim(1);
    std::string err;
    UT_CHECK(!sim.loadCity("{\"roads\": {\"nodes\": 5}}", &err));
    UT_CHECK(!err.empty());
    UT_CHECK(!sim.loadCity("[1, 2]", &err));
    UT_CHECK(!sim.loadMissions("{", &err));
    PlayerInput in;
    sim.tick(0.1f, in);                                // no city loaded: still safe to tick
    sim.tick(-1.0f, in);
    UT_CHECK_EQ(sim.traffic.count(), 0);
}
