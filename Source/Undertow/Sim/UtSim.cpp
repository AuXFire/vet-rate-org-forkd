#include "Sim/UtSim.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace ut {
namespace sim_detail {

constexpr int kSaveVersion = 1;

// 64-bit values do not survive a round trip through JSON numbers (doubles), so the RNG state is stored as hex.
std::string hex64(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

uint64_t parseHex64(const std::string& s) { return static_cast<uint64_t>(std::strtoull(s.c_str(), nullptr, 16)); }

bool parseObject(const std::string& text, Json& out, std::string* error) {
    if (!Json::parse(text, out, error)) return false;
    if (!out.isObject()) {
        if (error) *error = "top-level value must be an object";
        return false;
    }
    return true;
}

}  // namespace sim_detail

Simulation::Simulation(uint64_t seed) : rng(seed) {
    traffic.setGraph(&roads);
    dispatch.setGraph(&roads);
}

bool Simulation::loadCity(const std::string& json, std::string* error) {
    Json root;
    if (!sim_detail::parseObject(json, root, error)) return false;
    const Json& anchor = root["meta"]["anchor"];
    if (!roads.load(root["roads"], error)) return false;
    city.load(root);
    traffic.setGraph(&roads);   // drops agents that referenced the old graph
    dispatch.setGraph(&roads);
    district_ = nullptr;
    if (anchor.size() < 2) return true;

    // The city sits at a real place on Earth. Its roads become a permanent region of the world, and everything
    // with a position is pinned to the globe so it can be found again from any anchor.
    const LatLon where(anchor.at(0).asDouble(), anchor.at(1).asDouble());
    world.clear();
    world_ = true;
    hasFocus_ = false;
    frame_ = LocalFrame(where);
    dataFrame_ = frame_;
    TileData region;
    double reach = 0.0;
    for (int i = 0; i < roads.nodeCount(); ++i) {
        const RoadNode& n = roads.node(i);
        TileNode tn;
        tn.id = WorldRoads::regionNodeId(0, i);
        tn.pos = frame_.toGeo(n.pos);
        tn.signal = n.signal;
        region.nodes.push_back(tn);
        reach = std::max(reach, static_cast<double>(n.pos.length()));
    }
    for (int i = 0; i < roads.edgeCount(); ++i) {
        const RoadEdge& e = roads.edge(i);
        if (e.reverse >= 0 && e.reverse < i) continue;   // the second half of a two-way road
        TileEdge te;
        te.from = WorldRoads::regionNodeId(0, e.from);
        te.to = WorldRoads::regionNodeId(0, e.to);
        te.lanes = e.lanes;
        te.speed = e.speed;
        te.kind = e.kind;
        te.oneWay = e.reverse < 0;
        region.edges.push_back(te);
    }
    world.addRegion(city.name(), where, reach + 200.0, std::move(region));
    city.georeference(dataFrame_);
    missions.georeference(dataFrame_);
    economy.georeference(dataFrame_);
    world.setFocus(where);
    rebuildRoads();
    return true;
}

void Simulation::enableWorld(LatLon anchor) {
    world_ = true;
    frame_ = LocalFrame(anchor);
    dataFrame_ = frame_;
    hasFocus_ = false;
    world.setFocus(anchor);
    rebuildRoads();
}

void Simulation::rebuildRoads() {
    // Remember every edge by the ids of its ends, rebuild, then tell traffic where each old edge went.
    std::vector<std::pair<uint64_t, uint64_t>> old;
    old.reserve(static_cast<size_t>(roads.edgeCount()));
    for (const RoadEdge& e : roads.edges()) old.emplace_back(roads.node(e.from).id, roads.node(e.to).id);
    world.build(roads, frame_);
    std::vector<int> oldToNew(old.size(), -1);
    for (size_t i = 0; i < old.size(); ++i) {
        const int a = roads.nodeById(old[i].first), b = roads.nodeById(old[i].second);
        if (a >= 0 && b >= 0) oldToNew[i] = roads.findEdge(a, b);
    }
    traffic.graphRebuilt(oldToNew, events_);
    dispatch.roadsChanged();
    dirtyAge_ = 0.0f;
    densityAge_ = 1e9f;
    ++roadsVersion_;
    events_.push(EventType::RoadsChanged, roads.nodeCount());
}

void Simulation::relocalizeContent() {
    city.relocalize(frame_);
    missions.relocalize(frame_);
    economy.relocalize(frame_);
}

FrameMap Simulation::setAnchor(LatLon anchor) {
    if (!world_) {
        enableWorld(anchor);
        return FrameMap();
    }
    const LocalFrame next(anchor);
    const FrameMap map(frame_, next, anchor);
    frame_ = next;
    heat.rebase(map);
    dispatch.rebase(map);
    lastPlayerPos_ = map.point(lastPlayerPos_);
    lastFocus_ = map.point(lastFocus_);
    relocalizeContent();
    rebuildRoads();
    return map;
}

bool Simulation::provideTile(TileKey key, const std::string& json, std::string* error) {
    TileData data;
    const bool ok = json.empty() || WorldRoads::parseTile(json, data, error);
    world.provide(key, ok ? std::move(data) : TileData());
    return ok;
}

float Simulation::ambientDensity(Vec2 pos) {
    // Outside any authored district, judge how built-up the area is from the roads themselves.
    if (densityAge_ > 1.0f) {
        roads.nodesInRing(pos, 0.0f, 250.0f, scratchNodes_);
        int junctions = 0;
        for (const int ni : scratchNodes_) junctions += roads.node(ni).out.size() >= 3 ? 1 : 0;
        roadDensity_ = clampf(0.08f + static_cast<float>(junctions) / 30.0f, 0.08f, 0.9f);
        densityAge_ = 0.0f;
    }
    return roadDensity_;
}

bool Simulation::loadTuning(const std::string& json, std::string* error) {
    Json root;
    if (!sim_detail::parseObject(json, root, error)) return false;
    heat.tuning.load(root["heat"]);
    traffic.tuning.load(root["traffic"]);
    dispatch.tuning.load(root["dispatch"]);
    clock.timeScale = clampf(root["clock"]["timeScale"].asFloat(clock.timeScale), 1.0f, 3600.0f);
    crowd.tuning.maxPedestrians = root["crowd"]["maxPedestrians"].asInt(crowd.tuning.maxPedestrians);
    return true;
}

bool Simulation::loadMissions(const std::string& json, std::string* error) {
    Json root;
    if (!sim_detail::parseObject(json, root, error) || !missions.load(root, error)) return false;
    if (world_) {
        missions.georeference(dataFrame_);
        missions.relocalize(frame_);
    }
    return true;
}

bool Simulation::loadEconomy(const std::string& json, std::string* error) {
    Json root;
    if (!sim_detail::parseObject(json, root, error)) return false;
    economy.load(root);
    if (world_) {
        economy.georeference(dataFrame_);
        economy.relocalize(frame_);
    }
    return true;
}

void Simulation::reportCrime(Crime crime, Vec2 pos, bool seenByPolice, int civilianWitnesses) {
    heat.reportCrime(crime, pos, seenByPolice, civilianWitnesses, events_);
    if (crime == Crime::Gunfire) traffic.panicAt(pos, 60.0f, 8.0f);
}

void Simulation::notifyMission(MissionNotice notice, const std::string& tag) { missions.notify(notice, tag, events_); }

void Simulation::tick(float dt, const PlayerInput& player) {
    if (dt <= 0.0f) return;
    dt = dt > 0.25f ? 0.25f : dt;   // a long hitch must not teleport traffic or skip heat timers
    lastPlayerPos_ = player.pos;

    // 0. World mode: keep the right road tiles loaded around the player.
    if (world_) {
        if (!hasFocus_ || distanceSq(player.pos, lastFocus_) > 60.0f * 60.0f) {
            world.setFocus(frame_.toGeo(player.pos));
            lastFocus_ = player.pos;
            hasFocus_ = true;
        }
        if (world.dirty()) {
            // Tiles tend to arrive in bursts: wait until the batch is in (or a second has passed) and rebuild once.
            dirtyAge_ += dt;
            if (world.missing().empty() || dirtyAge_ > 1.0f) rebuildRoads();
        }
        densityAge_ += dt;
    }

    // 1. Time and weather.
    const float gameDt = clock.advance(dt, events_);
    weather.update(gameDt, rng, events_);
    if (clock.day != lastDay_) {
        lastDay_ = clock.day;
        economy.payDailyIncome(events_);
    }

    // 2. Where the player is decides how busy the streets are.
    district_ = city.districtAt(player.pos);
    const float districtDensity = district_ ? district_->density : (world_ ? ambientDensity(player.pos) : 0.3f);
    trafficDensity_ = districtDensity * (0.3f + 0.7f * clock.daylight()) * weather.trafficFactor() + 0.1f;
    pedestrianBudget_ = crowd.pedestrianBudget(districtDensity, clock.daylight(), weather.pedestrianFactor());

    // 3. Police attention, then the units responding to it.
    const bool seen = player.hasSightResult ? player.seenByPolice : dispatch.anyUnitNear(player.pos, weather.visibility());
    heat.update(dt, player.pos, seen, events_);
    dispatch.update(dt, heat, player.pos, rng, events_);

    // 4. Ambient traffic drives around the player's car and around police units.
    if (player.inVehicle) traffic.addObstacle(player.pos, player.velocity, 2.0f);
    for (const PoliceUnit& u : dispatch.units()) traffic.addObstacle(u.pos, Vec2::fromAngle(u.heading) * u.speed, 2.0f);
    traffic.update(dt, player.pos, trafficDensity_, rng, events_);

    // 5. Missions last, so they see this tick's heat level.
    MissionContext ctx;
    ctx.playerPos = player.pos;
    ctx.inVehicle = player.inVehicle;
    ctx.vehicleTag = player.vehicleTag;
    ctx.heatLevel = heat.level();
    missions.update(dt, ctx, events_);
    const int reward = missions.takePendingReward();
    if (reward > 0) economy.earn(reward, events_);
}

std::string Simulation::save() const {
    Json j = Json::object();
    j.set("version", sim_detail::kSaveVersion);
    j.set("clock", clock.save());
    j.set("weather", weather.save());
    j.set("heat", heat.save());
    j.set("missions", missions.save());
    j.set("economy", economy.save());
    Json p = Json::array();
    p.push(lastPlayerPos_.x).push(lastPlayerPos_.y);
    j.set("player", p);
    if (world_) {
        // `player` and the heat state are local to this anchor; `geo` is the same place on the globe.
        const LatLon geo = playerGeo();
        Json a = Json::array(), g = Json::array();
        a.push(frame_.anchor().lat).push(frame_.anchor().lon);
        g.push(geo.lat).push(geo.lon);
        j.set("anchor", a);
        j.set("geo", g);
    }
    Json r = Json::array();
    r.push(sim_detail::hex64(rng.state())).push(sim_detail::hex64(rng.stream()));
    j.set("rng", r);
    return j.dump(1);
}

bool Simulation::load(const std::string& text, std::string* error) {
    Json j;
    if (!sim_detail::parseObject(text, j, error)) return false;
    const int version = j["version"].asInt(0);
    if (version < 1 || version > sim_detail::kSaveVersion) {
        if (error) *error = "unsupported save version " + std::to_string(version);
        return false;
    }
    EventQueue discard;   // restoring state must not replay "heat cleared" or "unit removed" at the player
    if (world_ && j["anchor"].size() >= 2) {
        // Positions in the save are local to the anchor it was made at: return there before reading them.
        frame_ = LocalFrame(LatLon(j["anchor"].at(0).asDouble(), j["anchor"].at(1).asDouble()));
        relocalizeContent();
        hasFocus_ = false;
        world.setFocus(j["geo"].size() >= 2 ? LatLon(j["geo"].at(0).asDouble(), j["geo"].at(1).asDouble()) : frame_.anchor());
        rebuildRoads();
    }
    clock.load(j["clock"]);
    lastDay_ = clock.day;
    weather.load(j["weather"]);
    heat.load(j["heat"]);
    missions.load_state(j["missions"]);
    economy.load_state(j["economy"]);
    lastPlayerPos_ = Vec2(j["player"].at(0).asFloat(), j["player"].at(1).asFloat());
    if (j["rng"].size() == 2) rng.restore(sim_detail::parseHex64(j["rng"].at(0).asString()), sim_detail::parseHex64(j["rng"].at(1).asString()));
    traffic.clear(discard);
    dispatch.clear(discard);
    events_.drain();
    return true;
}

}  // namespace ut
