#include "Sim/UtSim.h"

#include <cstdio>
#include <cstdlib>

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
    if (!roads.load(root["roads"], error)) return false;
    city.load(root);
    traffic.setGraph(&roads);   // drops agents that referenced the old graph
    dispatch.setGraph(&roads);
    district_ = nullptr;
    return true;
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
    return sim_detail::parseObject(json, root, error) && missions.load(root, error);
}

bool Simulation::loadEconomy(const std::string& json, std::string* error) {
    Json root;
    if (!sim_detail::parseObject(json, root, error)) return false;
    economy.load(root);
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

    // 1. Time and weather.
    const float gameDt = clock.advance(dt, events_);
    weather.update(gameDt, rng, events_);
    if (clock.day != lastDay_) {
        lastDay_ = clock.day;
        economy.payDailyIncome(events_);
    }

    // 2. Where the player is decides how busy the streets are.
    district_ = city.districtAt(player.pos);
    const float districtDensity = district_ ? district_->density : 0.3f;
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
