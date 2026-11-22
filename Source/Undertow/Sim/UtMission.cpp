#include "Sim/UtMission.h"

#include <algorithm>

namespace ut {
namespace mission_detail {

bool parseType(const std::string& s, ObjectiveType& out) {
    if (s == "goto") out = ObjectiveType::GoTo;
    else if (s == "enter_vehicle") out = ObjectiveType::EnterVehicle;
    else if (s == "deliver") out = ObjectiveType::Deliver;
    else if (s == "collect") out = ObjectiveType::Collect;
    else if (s == "eliminate") out = ObjectiveType::Eliminate;
    else if (s == "lose_heat") out = ObjectiveType::LoseHeat;
    else if (s == "wait") out = ObjectiveType::Wait;
    else return false;
    return true;
}

Vec2 readPos(const Json& j) { return Vec2(j.at(0).asFloat(), j.at(1).asFloat()); }

bool tagMatches(const std::string& wanted, const std::string& actual) { return wanted.empty() || wanted == actual; }

}  // namespace mission_detail

bool MissionSystem::load(const Json& root, std::string* error) {
    defs_.clear();
    const Json& list = root["missions"];
    for (int i = 0; i < list.size(); ++i) {
        const Json& m = list.at(i);
        MissionDef def;
        def.id = m["id"].asString();
        def.title = m["title"].asString(def.id);
        def.giver = m["giver"].asString();
        def.intro = m["intro"].asString();
        def.start = mission_detail::readPos(m["start"]);
        def.rewardCash = m["reward"]["cash"].asInt(0);
        for (int k = 0; k < m["requires"].size(); ++k) def.requires_.push_back(m["requires"].at(k).asString());
        const Json& fails = m["fail"];
        def.failOnPlayerDown = !fails.isArray();   // default when no fail list is given
        for (int k = 0; k < fails.size(); ++k) {
            const std::string type = fails.at(k)["type"].asString();
            if (type == "vehicle_destroyed") def.failOnVehicleDestroyed.push_back(fails.at(k)["tag"].asString());
            else if (type == "player_down") def.failOnPlayerDown = true;
        }
        const Json& objs = m["objectives"];
        for (int k = 0; k < objs.size(); ++k) {
            const Json& o = objs.at(k);
            Objective obj;
            if (!mission_detail::parseType(o["type"].asString(), obj.type)) {
                if (error) *error = "mission '" + def.id + "' objective " + std::to_string(k) + ": unknown type '" + o["type"].asString() + "'";
                defs_.clear();
                return false;
            }
            obj.text = o["text"].asString();
            obj.tag = o["tag"].asString();
            obj.pos = mission_detail::readPos(o["pos"]);
            obj.radius = o["radius"].asFloat(8.0f);
            obj.count = std::max(1, o["count"].asInt(1));
            obj.seconds = o["seconds"].asFloat(0.0f);
            obj.timeLimit = o["timeLimit"].asFloat(0.0f);
            obj.maxHeat = o["maxHeat"].asInt(-1);
            def.objectives.push_back(obj);
        }
        if (def.id.empty() || def.objectives.empty()) {
            if (error) *error = "mission " + std::to_string(i) + " needs an id and at least one objective";
            defs_.clear();
            return false;
        }
        defs_.push_back(def);
    }
    // Every prerequisite must name a real mission, or that mission could never unlock.
    for (const MissionDef& def : defs_) {
        for (const std::string& req : def.requires_) {
            if (!find(req)) {
                if (error) *error = "mission '" + def.id + "' requires unknown mission '" + req + "'";
                defs_.clear();
                return false;
            }
        }
    }
    return true;
}

const MissionDef* MissionSystem::find(const std::string& id) const {
    for (const MissionDef& d : defs_) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

bool MissionSystem::isComplete(const std::string& id) const {
    return std::find(completed_.begin(), completed_.end(), id) != completed_.end();
}

std::vector<const MissionDef*> MissionSystem::available() const {
    std::vector<const MissionDef*> out;
    for (size_t i = 0; i < defs_.size(); ++i) {
        const MissionDef& d = defs_[i];
        if (static_cast<int>(i) == active_ || isComplete(d.id)) continue;
        bool unlocked = true;
        for (const std::string& req : d.requires_) unlocked = unlocked && isComplete(req);
        if (unlocked) out.push_back(&d);
    }
    return out;
}

bool MissionSystem::start(const std::string& id, EventQueue& events) {
    if (active_ >= 0) return false;
    for (const MissionDef* d : available()) {
        if (d->id != id) continue;
        active_ = static_cast<int>(d - defs_.data());
        objective_ = 0;
        events.push(EventType::MissionStarted, 0, 0, d->start, d->id);
        beginObjective(events);
        return true;
    }
    return false;
}

void MissionSystem::beginObjective(EventQueue& events) {
    counter_ = 0;
    elapsed_ = 0.0f;
    const Objective* o = currentObjective();
    if (o) events.push(EventType::ObjectiveChanged, objective_, 0, o->pos, o->text);
}

void MissionSystem::completeObjective(EventQueue& events) {
    const MissionDef& def = defs_[static_cast<size_t>(active_)];
    ++objective_;
    if (objective_ < static_cast<int>(def.objectives.size())) {
        beginObjective(events);
        return;
    }
    completed_.push_back(def.id);
    pendingReward_ += def.rewardCash;
    events.push(EventType::MissionCompleted, def.rewardCash, 0, Vec2(), def.id);
    active_ = -1;
    objective_ = 0;
}

void MissionSystem::fail(const std::string& reason, EventQueue& events) {
    if (active_ < 0) return;
    events.push(EventType::MissionFailed, 0, 0, Vec2(), reason);
    active_ = -1;
    objective_ = 0;
}

void MissionSystem::abandon(EventQueue& events) { fail("Mission abandoned", events); }

const Objective* MissionSystem::currentObjective() const {
    if (active_ < 0) return nullptr;
    const MissionDef& def = defs_[static_cast<size_t>(active_)];
    return objective_ < static_cast<int>(def.objectives.size()) ? &def.objectives[static_cast<size_t>(objective_)] : nullptr;
}

float MissionSystem::timeRemaining() const {
    const Objective* o = currentObjective();
    return o && o->timeLimit > 0.0f ? std::max(0.0f, o->timeLimit - elapsed_) : -1.0f;
}

void MissionSystem::update(float dt, const MissionContext& ctx, EventQueue& events) {
    const Objective* o = currentObjective();
    if (!o) return;
    elapsed_ += dt;
    if (o->maxHeat >= 0 && ctx.heatLevel > o->maxHeat) {
        fail("You drew too much heat", events);
        return;
    }
    bool done = false;
    switch (o->type) {
        case ObjectiveType::GoTo:
            done = distance(ctx.playerPos, o->pos) <= o->radius;
            break;
        case ObjectiveType::EnterVehicle:
            done = ctx.inVehicle && mission_detail::tagMatches(o->tag, ctx.vehicleTag);
            break;
        case ObjectiveType::Deliver:
            done = ctx.inVehicle && mission_detail::tagMatches(o->tag, ctx.vehicleTag) && distance(ctx.playerPos, o->pos) <= o->radius;
            break;
        case ObjectiveType::Collect:
        case ObjectiveType::Eliminate:
            done = counter_ >= o->count;
            break;
        case ObjectiveType::LoseHeat:
            done = ctx.heatLevel == 0;
            break;
        case ObjectiveType::Wait:
            done = elapsed_ >= o->seconds;
            break;
    }
    if (done) {
        completeObjective(events);
    } else if (o->timeLimit > 0.0f && elapsed_ >= o->timeLimit) {
        fail("Out of time", events);
    }
}

void MissionSystem::notify(MissionNotice notice, const std::string& tag, EventQueue& events) {
    if (active_ < 0) return;
    const MissionDef& def = defs_[static_cast<size_t>(active_)];
    if (notice == MissionNotice::PlayerDown) {
        if (def.failOnPlayerDown) fail("You went down", events);
        return;
    }
    if (notice == MissionNotice::VehicleDestroyed) {
        if (std::find(def.failOnVehicleDestroyed.begin(), def.failOnVehicleDestroyed.end(), tag) != def.failOnVehicleDestroyed.end()) {
            fail("The vehicle was destroyed", events);
        }
        return;
    }
    const Objective* o = currentObjective();
    if (!o || !mission_detail::tagMatches(o->tag, tag)) return;
    if ((notice == MissionNotice::Collected && o->type == ObjectiveType::Collect) ||
        (notice == MissionNotice::Eliminated && o->type == ObjectiveType::Eliminate)) {
        ++counter_;
    }
}

int MissionSystem::takePendingReward() {
    const int r = pendingReward_;
    pendingReward_ = 0;
    return r;
}

Json MissionSystem::save() const {
    // An in-progress mission is not saved: loading puts the player back at the mission marker.
    Json j = Json::object();
    Json done = Json::array();
    for (const std::string& id : completed_) done.push(id);
    j.set("completed", done);
    return j;
}

void MissionSystem::load_state(const Json& j) {
    completed_.clear();
    active_ = -1;
    objective_ = 0;
    pendingReward_ = 0;
    const Json& done = j["completed"];
    for (int i = 0; i < done.size(); ++i) {
        const std::string id = done.at(i).asString();
        if (find(id) && !isComplete(id)) completed_.push_back(id);   // ignore ids removed from the data
    }
}

}  // namespace ut
