#include "Sim/UtDispatch.h"

#include <algorithm>
#include <cmath>

namespace ut {

void DispatchTuning::load(const Json& j) {
    const Json& u = j["unitsPerLevel"];
    for (int i = 0; i <= kMaxHeatLevel; ++i) {
        if (u.at(i).isNumber()) unitsPerLevel[i] = u.at(i).asInt();
    }
    spawnMin = j["spawnMin"].asFloat(spawnMin);
    spawnMax = j["spawnMax"].asFloat(spawnMax);
    spawnInterval = j["spawnInterval"].asFloat(spawnInterval);
    pursuitSpeed = j["pursuitSpeed"].asFloat(pursuitSpeed);
    searchSpeed = j["searchSpeed"].asFloat(searchSpeed);
    sightRange = j["sightRange"].asFloat(sightRange);
    replanInterval = j["replanInterval"].asFloat(replanInterval);
    standDownSeconds = j["standDownSeconds"].asFloat(standDownSeconds);
}

void DispatchSystem::plan(PoliceUnit& unit, Vec2 goal) {
    unit.goal = goal;
    unit.path.clear();
    unit.pathProgress = 0.0f;
    ++unit.pathVersion;
    unit.path.push_back(unit.pos);
    if (graph_) {
        const int to = graph_->nearestNode(goal, 400.0f);
        // Join the road network at whichever end of the road the unit is on gets it there sooner. Always
        // using the nearest node would send a unit that has just left a junction straight back to it.
        float s = 0.0f;
        const int onEdge = graph_->nearestEdge(unit.pos, 400.0f, &s);
        if (onEdge >= 0 && to >= 0) {
            const RoadEdge& e = graph_->edge(onEdge);
            int bestStart = -1;
            float bestTime = 1e30f;
            std::vector<int> bestPath;
            for (const int start : {e.to, e.from}) {
                if (!graph_->findPath(start, to, scratchEdges_)) continue;
                const float time = distance(unit.pos, graph_->node(start).pos) / e.speed + graph_->travelTime(scratchEdges_);
                if (time < bestTime) {
                    bestTime = time;
                    bestStart = start;
                    bestPath = scratchEdges_;
                }
            }
            if (bestStart >= 0) {
                if (distance(unit.pos, graph_->node(bestStart).pos) > 2.0f) unit.path.push_back(graph_->node(bestStart).pos);
                for (const int ei : bestPath) unit.path.push_back(graph_->node(graph_->edge(ei).to).pos);
            }
        }
    }
    unit.path.push_back(goal);
}

void DispatchSystem::moveHeadless(PoliceUnit& unit, float dt) {
    if (unit.path.size() < 2) return;
    float travel = unit.speed * dt;
    while (travel > 0.0f && unit.path.size() >= 2) {
        const Vec2 next = unit.path[1];
        const float d = distance(unit.pos, next);
        if (d <= travel) {
            unit.pos = next;
            travel -= d;
            unit.path.erase(unit.path.begin());
            unit.path[0] = unit.pos;
        } else {
            const Vec2 dir = (next - unit.pos).normalized();
            unit.pos += dir * travel;
            unit.heading = dir.angle();
            unit.path[0] = unit.pos;
            travel = 0.0f;
        }
    }
}

void DispatchSystem::update(float dt, const HeatSystem& heat, Vec2 playerPos, Rng& rng, EventQueue& events) {
    const int level = heat.level();
    const HeatState state = heat.state();
    const int wanted = tuning.unitsPerLevel[std::min(std::max(level, 0), kMaxHeatLevel)];

    int active = 0;
    for (const PoliceUnit& u : units_) active += u.role != UnitRole::StandDown ? 1 : 0;

    // --- call in more units, one at a time, from out of sight
    spawnTimer_ = std::max(0.0f, spawnTimer_ - dt);
    if (graph_ && active < wanted && spawnTimer_ <= 0.0f) {
        const Vec2 around = state == HeatState::Pursuit ? playerPos : heat.lastKnownPosition();
        graph_->nodesInRing(around, tuning.spawnMin, tuning.spawnMax, scratchNodes_);
        // Keep spawn points away from the player even when the ring is centred on a stale position.
        scratchNodes_.erase(std::remove_if(scratchNodes_.begin(), scratchNodes_.end(),
                                           [&](int ni) { return distance(graph_->node(ni).pos, playerPos) < tuning.spawnMin * 0.8f; }),
                            scratchNodes_.end());
        if (!scratchNodes_.empty()) {
            PoliceUnit u;
            u.id = nextId_++;
            u.pos = graph_->node(scratchNodes_[static_cast<size_t>(rng.below(static_cast<int>(scratchNodes_.size())))]).pos;
            u.heading = (around - u.pos).angle();
            units_.push_back(u);
            events.push(EventType::UnitSpawned, u.id, 0, u.pos);
            spawnTimer_ = tuning.spawnInterval;
            ++active;
        }
    }

    // --- direct each unit
    for (size_t i = 0; i < units_.size();) {
        PoliceUnit& u = units_[i];
        if (level == 0 || active > wanted) {
            if (u.role != UnitRole::StandDown) {
                u.role = UnitRole::StandDown;
                u.standDownTimer = tuning.standDownSeconds;
                if (active > wanted) --active;
            }
        } else if (u.role == UnitRole::StandDown && active < wanted) {
            u.role = UnitRole::Search;   // heat came back before the unit left
            u.replanIn = 0.0f;
            ++active;
        }

        u.replanIn -= dt;
        if (u.role == UnitRole::StandDown) {
            u.speed = u.external ? u.speed : approach(u.speed, 8.0f, 6.0f * dt);
            u.standDownTimer -= dt;
            if (u.standDownTimer <= 0.0f) {
                events.push(EventType::UnitRemoved, u.id);
                units_.erase(units_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        } else if (state == HeatState::Pursuit) {
            u.role = UnitRole::Pursue;
            if (u.replanIn <= 0.0f) {
                plan(u, playerPos);
                u.replanIn = tuning.replanInterval;
            }
            if (!u.external) u.speed = approach(u.speed, tuning.pursuitSpeed, 8.0f * dt);
        } else {
            // Search: sweep random points inside the search zone, picking a new one on arrival.
            const bool arrived = distance(u.pos, u.goal) < 12.0f;
            if (u.role != UnitRole::Search || arrived || u.replanIn <= -20.0f || u.path.size() < 2) {
                u.role = UnitRole::Search;
                const float r = heat.searchRadius() * std::sqrt(rng.nextFloat());
                plan(u, heat.lastKnownPosition() + Vec2::fromAngle(rng.range(0.0f, kTwoPi)) * r);
                u.replanIn = tuning.replanInterval;
            }
            if (!u.external) u.speed = approach(u.speed, tuning.searchSpeed, 6.0f * dt);
        }
        if (!u.external) moveHeadless(u, dt);
        ++i;
    }
}

void DispatchSystem::reportUnit(int id, Vec2 pos, float heading, float speed) {
    for (PoliceUnit& u : units_) {
        if (u.id == id) {
            u.pos = pos;
            u.heading = heading;
            u.speed = speed;
            u.external = true;
            return;
        }
    }
}

void DispatchSystem::removeUnit(int id, EventQueue& events) {
    for (size_t i = 0; i < units_.size(); ++i) {
        if (units_[i].id == id) {
            events.push(EventType::UnitRemoved, id);
            units_.erase(units_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void DispatchSystem::rebase(const FrameMap& map) {
    for (PoliceUnit& u : units_) {
        u.pos = map.point(u.pos);
        u.goal = map.point(u.goal);
        u.heading = map.heading(u.heading);
        for (Vec2& p : u.path) p = map.point(p);
        ++u.pathVersion;
    }
}

void DispatchSystem::roadsChanged() {
    for (PoliceUnit& u : units_) u.replanIn = 0.0f;
}

void DispatchSystem::clear(EventQueue& events) {
    for (const PoliceUnit& u : units_) events.push(EventType::UnitRemoved, u.id);
    units_.clear();
    spawnTimer_ = 0.0f;
}

bool DispatchSystem::anyUnitNear(Vec2 playerPos, float visibility) const {
    const float range = tuning.sightRange * clampf(visibility, 0.2f, 1.0f);
    for (const PoliceUnit& u : units_) {
        if (u.role != UnitRole::StandDown && distance(u.pos, playerPos) <= range) return true;
    }
    return false;
}

}  // namespace ut
