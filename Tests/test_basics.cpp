#include "Core/UtClock.h"
#include "Core/UtHeat.h"
#include "Core/UtJson.h"
#include "Core/UtRoadGraph.h"
#include "ut_test.h"

using namespace ut;

// ---------------------------------------------------------------- json
UT_TEST(json, parses_nested_values) {
    Json j;
    std::string err;
    UT_CHECK(Json::parse(R"({"a": [1, 2.5, -3e2], "s": "x\nyé", "t": true, "n": null, "o": {"k": 7}})", j, &err));
    UT_CHECK_EQ(j["a"].size(), 3);
    UT_CHECK_NEAR(j["a"].at(1).asFloat(), 2.5f, 1e-6);
    UT_CHECK_NEAR(j["a"].at(2).asFloat(), -300.0f, 1e-3);
    UT_CHECK_EQ(j["s"].asString(), std::string("x\ny\xc3\xa9"));
    UT_CHECK(j["t"].asBool());
    UT_CHECK(j["n"].isNull());
    UT_CHECK_EQ(j["o"]["k"].asInt(), 7);
    UT_CHECK_EQ(j["missing"]["deeper"].asInt(42), 42);
}

UT_TEST(json, rejects_malformed_input) {
    Json j;
    std::string err;
    UT_CHECK(!Json::parse("{\"a\": [1, 2", j, &err));
    UT_CHECK(!err.empty());
    UT_CHECK(j.isNull());
    UT_CHECK(!Json::parse("{\"a\": 1} trailing", j));
    UT_CHECK(!Json::parse("", j));
    UT_CHECK(!Json::parse("[1,]", j));
}

UT_TEST(json, round_trips_through_dump) {
    Json j = Json::object();
    j.set("name", "Port \"Meridian\"");
    j.set("pi", 3.25);
    j.set("n", 12);
    Json arr = Json::array();
    arr.push(1).push(false).push(Json());
    j.set("list", arr);
    for (const int indent : {0, 2}) {
        Json back;
        UT_CHECK(Json::parse(j.dump(indent), back));
        UT_CHECK_EQ(back["name"].asString(), std::string("Port \"Meridian\""));
        UT_CHECK_NEAR(back["pi"].asDouble(), 3.25, 1e-9);
        UT_CHECK_EQ(back["n"].asInt(), 12);
        UT_CHECK_EQ(back["list"].size(), 3);
        UT_CHECK(back["list"].at(2).isNull());
    }
    UT_CHECK_EQ(Json(12).dump(), std::string("12"));
}

UT_TEST(json, allows_line_comments_in_data) {
    Json j;
    UT_CHECK(Json::parse("{ // tuning\n \"a\": 1 // trailing\n}", j));
    UT_CHECK_EQ(j["a"].asInt(), 1);
}

// ---------------------------------------------------------------- clock
UT_TEST(clock, rolls_over_at_midnight) {
    Clock c;
    EventQueue ev;
    c.secondsOfDay = 86400.0f - 60.0f;
    c.timeScale = 30.0f;
    const float gameDt = c.advance(4.0f, ev);   // 120 game seconds
    UT_CHECK_NEAR(gameDt, 120.0f, 1e-3);
    UT_CHECK_EQ(c.day, 2);
    UT_CHECK_NEAR(c.secondsOfDay, 60.0f, 1e-2);
    UT_CHECK_EQ(ev.count(EventType::DayStarted), 1);
}

UT_TEST(clock, daylight_ramps_at_dawn_and_dusk) {
    Clock c;
    c.secondsOfDay = 3.0f * 3600.0f;
    UT_CHECK_NEAR(c.daylight(), 0.0f, 1e-6);
    c.secondsOfDay = 6.5f * 3600.0f;
    UT_CHECK_NEAR(c.daylight(), 0.5f, 1e-3);
    c.secondsOfDay = 12.0f * 3600.0f;
    UT_CHECK_NEAR(c.daylight(), 1.0f, 1e-6);
    c.secondsOfDay = 19.75f * 3600.0f;
    UT_CHECK_NEAR(c.daylight(), 0.25f, 1e-3);
    UT_CHECK(c.isNight());
}

UT_TEST(clock, weather_changes_and_roads_get_wet) {
    WeatherSystem w;
    Rng rng(11);
    EventQueue ev;
    int changes = 0;
    bool sawRain = false;
    for (int hour = 0; hour < 24 * 30; ++hour) {
        w.update(3600.0f, rng, ev);
        sawRain = sawRain || w.current() == Weather::Rain;
    }
    changes = ev.count(EventType::WeatherChanged);
    UT_CHECK(changes > 20);
    UT_CHECK(sawRain);
    w.force(Weather::Storm, ev);
    for (int i = 0; i < 20; ++i) w.update(60.0f, rng, ev);   // 20 game minutes, no hourly roll yet
    UT_CHECK(w.wetness() > 0.9f);
    UT_CHECK(w.grip() < 0.7f);
}

// ---------------------------------------------------------------- heat
UT_TEST(heat, unwitnessed_crime_is_ignored) {
    HeatSystem h;
    EventQueue ev;
    h.reportCrime(Crime::Gunfire, Vec2(0, 0), false, 0, ev);
    h.update(30.0f, Vec2(0, 0), false, ev);
    UT_CHECK_EQ(h.level(), 0);
    UT_CHECK(h.state() == HeatState::Clean);
}

UT_TEST(heat, police_witness_is_immediate_and_starts_pursuit) {
    HeatSystem h;
    EventQueue ev;
    h.reportCrime(Crime::VehicleTheft, Vec2(10, 0), true, 0, ev);
    UT_CHECK_EQ(h.level(), 1);
    UT_CHECK(h.state() == HeatState::Pursuit);
    UT_CHECK_EQ(ev.count(EventType::HeatLevelChanged), 1);
    h.reportCrime(Crime::AssaultOnOfficer, Vec2(10, 0), true, 0, ev);
    UT_CHECK(h.level() >= 3);
}

UT_TEST(heat, civilian_report_arrives_after_delay_and_can_be_cancelled) {
    HeatSystem h;
    EventQueue ev;
    h.reportCrime(Crime::Robbery, Vec2(0, 0), false, 1, ev);
    h.update(h.tuning.reportDelay * 0.5f, Vec2(0, 0), false, ev);
    UT_CHECK_EQ(h.level(), 0);
    UT_CHECK_EQ(h.pendingReports(), 1);
    h.update(h.tuning.reportDelay * 0.6f, Vec2(200, 0), false, ev);
    UT_CHECK_EQ(h.level(), 2);
    UT_CHECK(h.state() == HeatState::Search);
    UT_CHECK_NEAR(h.lastKnownPosition().x, 0.0f, 1e-3);   // police go to the scene, not to the player

    HeatSystem h2;
    h2.reportCrime(Crime::Robbery, Vec2(0, 0), false, 4, ev);   // four witnesses: half the delay
    h2.update(h2.tuning.reportDelay * 0.4f, Vec2(0, 0), false, ev);
    UT_CHECK_EQ(h2.pendingReports(), 1);
    h2.cancelPendingReports(Vec2(5, 0), 20.0f);
    h2.update(60.0f, Vec2(0, 0), false, ev);
    UT_CHECK_EQ(h2.level(), 0);
}

UT_TEST(heat, evading_clears_heat_faster_outside_the_search_zone) {
    auto secondsToClear = [](Vec2 hidingSpot) {
        HeatSystem h;
        EventQueue ev;
        h.reportCrime(Crime::Gunfire, Vec2(0, 0), true, 0, ev);
        float t = 0.0f;
        while (h.level() > 0 && t < 600.0f) {
            h.update(0.5f, hidingSpot, false, ev);
            t += 0.5f;
        }
        return t;
    };
    const float inside = secondsToClear(Vec2(5, 0));
    const float outside = secondsToClear(Vec2(2000, 0));
    UT_CHECK_NEAR(inside, HeatTuning().evadeSeconds[1], 1.0f);
    UT_CHECK_NEAR(outside, HeatTuning().evadeSeconds[1] * 0.5f, 1.0f);
}

UT_TEST(heat, being_seen_resets_the_escape) {
    HeatSystem h;
    EventQueue ev;
    h.reportCrime(Crime::Gunfire, Vec2(0, 0), true, 0, ev);
    for (int i = 0; i < 30; ++i) h.update(0.5f, Vec2(0, 0), false, ev);   // 15 s unseen
    UT_CHECK(h.evadeProgress() > 0.5f);
    h.update(0.5f, Vec2(50, 0), true, ev);
    UT_CHECK(h.state() == HeatState::Pursuit);
    UT_CHECK_NEAR(h.evadeProgress(), 0.0f, 1e-6);
    UT_CHECK_NEAR(h.lastKnownPosition().x, 50.0f, 1e-3);
}

UT_TEST(heat, tuning_loads_from_json_and_state_saves) {
    Json j;
    UT_CHECK(Json::parse(R"({"points": {"gunfire": 100}, "thresholds": [0, 5, 10, 20, 40, 80], "reportDelay": 3})", j));
    HeatSystem h;
    h.tuning.load(j);
    EventQueue ev;
    h.reportCrime(Crime::Gunfire, Vec2(), true, 0, ev);
    UT_CHECK_EQ(h.level(), 5);
    HeatSystem restored;
    restored.tuning = h.tuning;
    restored.load(h.save());
    UT_CHECK_EQ(restored.level(), 5);
    UT_CHECK(restored.state() == HeatState::Search);
}

// ---------------------------------------------------------------- roads
namespace {
// 3x3 grid, 100 m spacing, node index = row * 3 + col. The middle row is a fast arterial.
RoadGraph makeGrid() {
    RoadGraph g;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) g.addNode(Vec2(static_cast<float>(c) * 100.0f, static_cast<float>(r) * 100.0f), r == 1 && c == 1);
    }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 2; ++c) g.addRoad(r * 3 + c, r * 3 + c + 1, r == 1 ? 2 : 1, r == 1 ? 25.0f : 10.0f, r == 1 ? RoadKind::Arterial : RoadKind::Street);
    }
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 2; ++r) g.addRoad(r * 3 + c, (r + 1) * 3 + c, 1, 10.0f);
    }
    g.buildIndex();
    return g;
}
}  // namespace

UT_TEST(roads, grid_has_two_directed_edges_per_road) {
    const RoadGraph g = makeGrid();
    UT_CHECK_EQ(g.nodeCount(), 9);
    UT_CHECK_EQ(g.edgeCount(), 24);
    const RoadEdge& e = g.edge(0);
    UT_CHECK_EQ(g.edge(e.reverse).reverse, 0);
    UT_CHECK_NEAR(e.length, 100.0f, 1e-3);
}

UT_TEST(roads, path_prefers_the_faster_road) {
    const RoadGraph g = makeGrid();
    std::vector<int> path;
    UT_CHECK(g.findPath(0, 2, path));          // along the slow bottom row: direct is 20 s
    UT_CHECK_EQ(static_cast<int>(path.size()), 2);
    UT_CHECK(g.findPath(3, 5, path));          // arterial
    UT_CHECK_EQ(static_cast<int>(path.size()), 2);
    for (const int ei : path) UT_CHECK(g.edge(ei).kind == RoadKind::Arterial);
    UT_CHECK(g.findPath(0, 8, path));
    UT_CHECK_EQ(static_cast<int>(path.size()), 4);
    UT_CHECK_EQ(g.edge(path.front()).from, 0);
    UT_CHECK_EQ(g.edge(path.back()).to, 8);
    for (size_t i = 1; i < path.size(); ++i) UT_CHECK_EQ(g.edge(path[i]).from, g.edge(path[i - 1]).to);
}

UT_TEST(roads, unreachable_node_reports_failure) {
    RoadGraph g = makeGrid();
    const int island = g.addNode(Vec2(900, 900));
    g.buildIndex();
    std::vector<int> path;
    UT_CHECK(!g.findPath(0, island, path));
    UT_CHECK(path.empty());
}

UT_TEST(roads, lanes_sit_right_of_centre_and_nearest_edge_picks_the_matching_side) {
    const RoadGraph g = makeGrid();
    // Edge 0 runs east from node 0 to node 1 along y = 0: its lane is on the south side (y < 0).
    const Vec2 p = g.lanePosition(0, 50.0f, 0);
    UT_CHECK_NEAR(p.x, 50.0f, 1e-3);
    UT_CHECK_NEAR(p.y, -RoadGraph::kLaneWidth * 0.5f, 1e-3);
    float s = 0.0f;
    const int south = g.nearestEdge(Vec2(40.0f, -1.5f), 20.0f, &s);
    UT_CHECK_EQ(south, 0);
    UT_CHECK_NEAR(s, 40.0f, 1e-2);
    const int north = g.nearestEdge(Vec2(40.0f, 1.5f), 20.0f);
    UT_CHECK_EQ(north, g.edge(0).reverse);
    UT_CHECK_EQ(g.nearestEdge(Vec2(5000, 5000), 50.0f), -1);
    UT_CHECK_EQ(g.nearestNode(Vec2(104, 97), 20.0f), 4);
}

UT_TEST(roads, random_next_edge_avoids_u_turns) {
    const RoadGraph g = makeGrid();
    Rng rng(3);
    for (int i = 0; i < 200; ++i) {
        const int ei = rng.below(g.edgeCount());
        const int next = g.randomNextEdge(ei, rng);
        UT_CHECK(next >= 0);
        UT_CHECK_EQ(g.edge(next).from, g.edge(ei).to);
        UT_CHECK(next != g.edge(ei).reverse);   // every grid node has at least two exits
    }
}

UT_TEST(roads, loads_the_compact_city_format) {
    Json j;
    UT_CHECK(Json::parse(R"({"nodes": [[0,0,0],[100,0,1],[100,80,0]], "edges": [[0,1,2,14,2,0],[1,2,1,8,1,1]]})", j));
    RoadGraph g;
    std::string err;
    UT_CHECK(g.load(j, &err));
    UT_CHECK_EQ(g.edgeCount(), 3);              // one two-way road + one one-way
    UT_CHECK(g.node(1).signal);
    UT_CHECK_EQ(g.edge(2).reverse, -1);
    Json bad;
    UT_CHECK(Json::parse(R"({"nodes": [[0,0,0]], "edges": [[0,7,1,10,1,0]]})", bad));
    UT_CHECK(!g.load(bad, &err));
    UT_CHECK(!err.empty());
}
